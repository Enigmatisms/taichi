#include "ir.h"
#include <numeric>
#include "tlang.h"
#include <Eigen/Dense>

TLANG_NAMESPACE_BEGIN

DecoratorRecorder dec;

// Vector width, vectorization plan etc
class PropagateSchedule : public IRVisitor {};

IRBuilder::ScopeGuard IRBuilder::create_scope(std::unique_ptr<Block> &list) {
  TC_ASSERT(list == nullptr);
  list = std::make_unique<Block>();
  if (!stack.empty()) {
    list->parent = stack.back();
  }
  return ScopeGuard(this, list.get());
}

void Expr::operator=(const Expr &o) {
  if (expr == nullptr || !expr->is_lvalue()) {
    set(o.eval());
  } else {
    current_ast_builder().insert(
        std::make_unique<FrontendAssignStmt>(*this, load_if_ptr(o)));
  }
}

FrontendContext::FrontendContext() {
  root_node = std::make_unique<Block>();
  current_builder = std::make_unique<IRBuilder>(root_node.get());
}

Expr::Expr(int32 x) : Expr() {
  expr = std::make_shared<ConstExpression>(x);
}

Expr::Expr(float32 x) : Expr() {
  expr = std::make_shared<ConstExpression>(x);
}

Expr::Expr(Identifier id) : Expr() {
  expr = std::make_shared<IdExpression>(id);
}

Expr Expr::eval() const {
  TC_ASSERT(expr != nullptr);
  if (is<EvalExpression>()) {
    return *this;
  }
  auto eval_stmt = Stmt::make<FrontendEvalStmt>(*this);
  auto eval_expr = Expr(std::make_shared<EvalExpression>(eval_stmt.get()));
  eval_stmt->as<FrontendEvalStmt>()->eval_expr.set(eval_expr);
  // needed in lower_ast to replace the statement itself with the
  // lowered statement
  current_ast_builder().insert(std::move(eval_stmt));
  return eval_expr;
}

void Expr::operator+=(const Expr &o) {
  (*this) = (*this) + o;
}
void Expr::operator-=(const Expr &o) {
  (*this) = (*this) - o;
}
void Expr::operator*=(const Expr &o) {
  (*this) = (*this) * o;
}
void Expr::operator/=(const Expr &o) {
  (*this) = (*this) / o;
}

FrontendForStmt::FrontendForStmt(Expr loop_var, Expr begin, Expr end)
    : begin(begin), end(end) {
  vectorize = dec.vectorize;
  parallelize = dec.parallelize;
  dec.reset();
  if (vectorize == -1)
    vectorize = 1;
  loop_var_id = loop_var.cast<IdExpression>()->id;
}

IRNode *Stmt::get_ir_root() {
  auto block = parent;
  while (block->parent)
    block = block->parent;
  return dynamic_cast<IRNode *>(block);
}

FrontendAssignStmt::FrontendAssignStmt(Expr lhs, Expr rhs)
    : lhs(lhs), rhs(rhs) {
  TC_ASSERT(lhs->is_lvalue());
}

IRNode *FrontendContext::root() {
  return static_cast<IRNode *>(root_node.get());
}

int Identifier::id_counter = 0;
std::atomic<int> Statement::instance_id_counter(0);

std::unique_ptr<FrontendContext> context;

void *Expr::evaluate_addr(int i, int j, int k, int l) {
  auto snode = this->cast<GlobalVariableExpression>()->snode;
  return snode->evaluate(get_current_program().data_structure, i, j, k, l);
}

template <int i, typename... Indices>
std::enable_if_t<(i < sizeof...(Indices)), int> get_if_exists(
    std::tuple<Indices...> tup) {
  return std::get<i>(tup);
}

template <int i, typename... Indices>
std::enable_if_t<!(i < sizeof...(Indices)), int> get_if_exists(
    std::tuple<Indices...> tup) {
  return 0;
}

template <typename... Indices>
void *Expr::val_tmp(DataType dt, Indices... indices) {
  auto snode = this->cast<GlobalVariableExpression>()->snode;
  if (dt != snode->dt) {
    TC_ERROR("Cannot access type {} as type {}", data_type_name(snode->dt),
             data_type_name(dt));
  }
  TC_ASSERT(sizeof...(indices) == snode->num_active_indices);
  int ind[max_num_indices];
  std::memset(ind, 0, sizeof(ind));
  auto tup = std::make_tuple(indices...);
#define LOAD_IND(i) ind[snode->index_order[i]] = get_if_exists<i>(tup);
  LOAD_IND(0);
  LOAD_IND(1);
  LOAD_IND(2);
  LOAD_IND(3);
#undef LOAD_IND
  TC_ASSERT(max_num_indices == 4);
  return evaluate_addr(ind[0], ind[1], ind[2], ind[3]);
}

template void *Expr::val_tmp<>(DataType);
template void *Expr::val_tmp<int>(DataType, int);
template void *Expr::val_tmp<int, int>(DataType, int, int);
template void *Expr::val_tmp<int, int, int>(DataType, int, int, int);
template void *Expr::val_tmp<int, int, int, int>(DataType, int, int, int, int);

void Stmt::insert_before_me(std::unique_ptr<Stmt> &&new_stmt) {
  TC_ASSERT(parent);
  auto &stmts = parent->statements;
  int loc = -1;
  for (int i = 0; i < (int)stmts.size(); i++) {
    if (stmts[i].get() == this) {
      loc = i;
      break;
    }
  }
  TC_ASSERT(loc != -1);
  new_stmt->parent = parent;
  stmts.insert(stmts.begin() + loc, std::move(new_stmt));
}

void Stmt::insert_after_me(std::unique_ptr<Stmt> &&new_stmt) {
  TC_ASSERT(parent);
  auto &stmts = parent->statements;
  int loc = -1;
  for (int i = 0; i < (int)stmts.size(); i++) {
    if (stmts[i].get() == this) {
      loc = i;
      break;
    }
  }
  TC_ASSERT(loc != -1);
  new_stmt->parent = parent;
  stmts.insert(stmts.begin() + loc + 1, std::move(new_stmt));
}

Block *current_block = nullptr;

template <>
std::string to_string(const LaneAttribute<LocalAddress> &ptr) {
  std::string ret = " [";
  for (int i = 0; i < (int)ptr.size(); i++) {
    ret += fmt::format("{}[{}]", ptr[i].var->name(), ptr[i].offset);
    if (i + 1 < (int)ptr.size())
      ret += ", ";
  }
  ret += "]";
  return ret;
}

Stmt *LocalLoadStmt::previous_store_or_alloca_in_block() {
  int position = parent->locate(this);
  // TC_ASSERT(width() == 1);
  // TC_ASSERT(this->ptr[0].offset == 0);
  for (int i = position - 1; i >= 0; i--) {
    if (parent->statements[i]->is<LocalStoreStmt>()) {
      auto store = parent->statements[i]->as<LocalStoreStmt>();
      // TC_ASSERT(store->width() == 1);
      if (store->ptr == this->ptr[0].var) {
        // found
        return store;
      }
    } else if (parent->statements[i]->is<AllocaStmt>()) {
      auto alloca = parent->statements[i]->as<AllocaStmt>();
      // TC_ASSERT(alloca->width() == 1);
      if (alloca == this->ptr[0].var) {
        return alloca;
      }
    }
  }
  return nullptr;
}

TLANG_NAMESPACE_END
