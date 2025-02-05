// Copyright 2013 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/crankshaft/typing.h"

#include "src/ast/scopes.h"
#include "src/frames.h"
#include "src/frames-inl.h"
#include "src/ostreams.h"
#include "src/parsing/parser.h"  // for CompileTimeValue; TODO(rossberg): move
#include "src/splay-tree-inl.h"

namespace v8 {
namespace internal {

AstTyper::AstTyper(Isolate* isolate, Zone* zone, Handle<JSFunction> closure,
                   Scope* scope, BailoutId osr_ast_id, FunctionLiteral* root)
    : isolate_(isolate),
      zone_(zone),
      closure_(closure),
      scope_(scope),
      osr_ast_id_(osr_ast_id),
      root_(root),
      oracle_(isolate, zone, handle(closure->shared()->code()),
              handle(closure->feedback_vector()),
              handle(closure->context()->native_context())),
      store_(zone) {
  InitializeAstVisitor(isolate);
}


#ifdef OBJECT_PRINT
  static void PrintObserved(Variable* var, Object* value, Type* type) {
    OFStream os(stdout);
    os << "  observed " << (var->IsParameter() ? "param" : "local") << "  ";
    var->name()->Print(os);
    os << " : " << Brief(value) << " -> ";
    type->PrintTo(os);
    os << std::endl;
  }
#endif  // OBJECT_PRINT


Effect AstTyper::ObservedOnStack(Object* value) {
  Type* lower = Type::NowOf(value, zone());
  return Effect(Bounds(lower, Type::Any()));
}


void AstTyper::ObserveTypesAtOsrEntry(IterationStatement* stmt) {
  if (stmt->OsrEntryId() != osr_ast_id_) return;

  DisallowHeapAllocation no_gc;
  JavaScriptFrameIterator it(isolate_);
  JavaScriptFrame* frame = it.frame();

  // Assert that the frame on the stack belongs to the function we want to OSR.
  DCHECK_EQ(*closure_, frame->function());

  int params = scope_->num_parameters();
  int locals = scope_->StackLocalCount();

  // Use sequential composition to achieve desired narrowing.
  // The receiver is a parameter with index -1.
  store_.Seq(parameter_index(-1), ObservedOnStack(frame->receiver()));
  for (int i = 0; i < params; i++) {
    store_.Seq(parameter_index(i), ObservedOnStack(frame->GetParameter(i)));
  }

  for (int i = 0; i < locals; i++) {
    store_.Seq(stack_local_index(i), ObservedOnStack(frame->GetExpression(i)));
  }

#ifdef OBJECT_PRINT
  if (FLAG_trace_osr && FLAG_print_scopes) {
    PrintObserved(scope_->receiver(), frame->receiver(),
                  store_.LookupBounds(parameter_index(-1)).lower);

    for (int i = 0; i < params; i++) {
      PrintObserved(scope_->parameter(i), frame->GetParameter(i),
                    store_.LookupBounds(parameter_index(i)).lower);
    }

    ZoneList<Variable*> local_vars(locals, zone());
    ZoneList<Variable*> context_vars(scope_->ContextLocalCount(), zone());
    ZoneList<Variable*> global_vars(scope_->ContextGlobalCount(), zone());
    scope_->CollectStackAndContextLocals(&local_vars, &context_vars,
                                         &global_vars);
    for (int i = 0; i < locals; i++) {
      PrintObserved(local_vars.at(i),
                    frame->GetExpression(i),
                    store_.LookupBounds(stack_local_index(i)).lower);
    }
  }
#endif  // OBJECT_PRINT
}


#define RECURSE(call)                \
  do {                               \
    DCHECK(!HasStackOverflow());     \
    call;                            \
    if (HasStackOverflow()) return;  \
  } while (false)


void AstTyper::Run() {
  RECURSE(VisitDeclarations(scope_->declarations()));
  RECURSE(VisitStatements(root_->body()));
}


void AstTyper::VisitStatements(ZoneList<Statement*>* stmts) {
  for (int i = 0; i < stmts->length(); ++i) {
    Statement* stmt = stmts->at(i);
    RECURSE(Visit(stmt));
    if (stmt->IsJump()) break;
  }
}


void AstTyper::VisitBlock(Block* stmt) {
  RECURSE(VisitStatements(stmt->statements()));
  if (stmt->labels() != NULL) {
    store_.Forget();  // Control may transfer here via 'break l'.
  }
}


void AstTyper::VisitExpressionStatement(ExpressionStatement* stmt) {
  RECURSE(Visit(stmt->expression()));
}


void AstTyper::VisitEmptyStatement(EmptyStatement* stmt) {
}


void AstTyper::VisitSloppyBlockFunctionStatement(
    SloppyBlockFunctionStatement* stmt) {
  Visit(stmt->statement());
}


void AstTyper::VisitIfStatement(IfStatement* stmt) {
  // Collect type feedback.
  if (!stmt->condition()->ToBooleanIsTrue() &&
      !stmt->condition()->ToBooleanIsFalse()) {
    stmt->condition()->RecordToBooleanTypeFeedback(oracle());
  }

  RECURSE(Visit(stmt->condition()));
  Effects then_effects = EnterEffects();
  RECURSE(Visit(stmt->then_statement()));
  ExitEffects();
  Effects else_effects = EnterEffects();
  RECURSE(Visit(stmt->else_statement()));
  ExitEffects();
  then_effects.Alt(else_effects);
  store_.Seq(then_effects);
}


void AstTyper::VisitContinueStatement(ContinueStatement* stmt) {
  // TODO(rossberg): is it worth having a non-termination effect?
}


void AstTyper::VisitBreakStatement(BreakStatement* stmt) {
  // TODO(rossberg): is it worth having a non-termination effect?
}


void AstTyper::VisitReturnStatement(ReturnStatement* stmt) {
  // Collect type feedback.
  // TODO(rossberg): we only need this for inlining into test contexts...
  stmt->expression()->RecordToBooleanTypeFeedback(oracle());

  RECURSE(Visit(stmt->expression()));
  // TODO(rossberg): is it worth having a non-termination effect?
}


void AstTyper::VisitWithStatement(WithStatement* stmt) {
  RECURSE(stmt->expression());
  RECURSE(stmt->statement());
}


void AstTyper::VisitSwitchStatement(SwitchStatement* stmt) {
  RECURSE(Visit(stmt->tag()));

  ZoneList<CaseClause*>* clauses = stmt->cases();
  Effects local_effects(zone());
  bool complex_effects = false;  // True for label effects or fall-through.

  for (int i = 0; i < clauses->length(); ++i) {
    CaseClause* clause = clauses->at(i);

    Effects clause_effects = EnterEffects();

    if (!clause->is_default()) {
      Expression* label = clause->label();
      // Collect type feedback.
      Type* tag_type;
      Type* label_type;
      Type* combined_type;
      oracle()->CompareType(clause->CompareId(),
                            &tag_type, &label_type, &combined_type);
      NarrowLowerType(stmt->tag(), tag_type);
      NarrowLowerType(label, label_type);
      clause->set_compare_type(combined_type);

      RECURSE(Visit(label));
      if (!clause_effects.IsEmpty()) complex_effects = true;
    }

    ZoneList<Statement*>* stmts = clause->statements();
    RECURSE(VisitStatements(stmts));
    ExitEffects();
    if (stmts->is_empty() || stmts->last()->IsJump()) {
      local_effects.Alt(clause_effects);
    } else {
      complex_effects = true;
    }
  }

  if (complex_effects) {
    store_.Forget();  // Reached this in unknown state.
  } else {
    store_.Seq(local_effects);
  }
}


void AstTyper::VisitCaseClause(CaseClause* clause) {
  UNREACHABLE();
}


void AstTyper::VisitDoWhileStatement(DoWhileStatement* stmt) {
  // Collect type feedback.
  if (!stmt->cond()->ToBooleanIsTrue()) {
    stmt->cond()->RecordToBooleanTypeFeedback(oracle());
  }

  // TODO(rossberg): refine the unconditional Forget (here and elsewhere) by
  // computing the set of variables assigned in only some of the origins of the
  // control transfer (such as the loop body here).
  store_.Forget();  // Control may transfer here via looping or 'continue'.
  ObserveTypesAtOsrEntry(stmt);
  RECURSE(Visit(stmt->body()));
  RECURSE(Visit(stmt->cond()));
  store_.Forget();  // Control may transfer here via 'break'.
}


void AstTyper::VisitWhileStatement(WhileStatement* stmt) {
  // Collect type feedback.
  if (!stmt->cond()->ToBooleanIsTrue()) {
    stmt->cond()->RecordToBooleanTypeFeedback(oracle());
  }

  store_.Forget();  // Control may transfer here via looping or 'continue'.
  RECURSE(Visit(stmt->cond()));
  ObserveTypesAtOsrEntry(stmt);
  RECURSE(Visit(stmt->body()));
  store_.Forget();  // Control may transfer here via termination or 'break'.
}


void AstTyper::VisitForStatement(ForStatement* stmt) {
  if (stmt->init() != NULL) {
    RECURSE(Visit(stmt->init()));
  }
  store_.Forget();  // Control may transfer here via looping.
  if (stmt->cond() != NULL) {
    // Collect type feedback.
    stmt->cond()->RecordToBooleanTypeFeedback(oracle());

    RECURSE(Visit(stmt->cond()));
  }
  ObserveTypesAtOsrEntry(stmt);
  RECURSE(Visit(stmt->body()));
  if (stmt->next() != NULL) {
    store_.Forget();  // Control may transfer here via 'continue'.
    RECURSE(Visit(stmt->next()));
  }
  store_.Forget();  // Control may transfer here via termination or 'break'.
}


void AstTyper::VisitForInStatement(ForInStatement* stmt) {
  // Collect type feedback.
  stmt->set_for_in_type(static_cast<ForInStatement::ForInType>(
      oracle()->ForInType(stmt->ForInFeedbackSlot())));

  RECURSE(Visit(stmt->enumerable()));
  store_.Forget();  // Control may transfer here via looping or 'continue'.
  ObserveTypesAtOsrEntry(stmt);
  RECURSE(Visit(stmt->body()));
  store_.Forget();  // Control may transfer here via 'break'.
}


void AstTyper::VisitForOfStatement(ForOfStatement* stmt) {
  RECURSE(Visit(stmt->iterable()));
  store_.Forget();  // Control may transfer here via looping or 'continue'.
  RECURSE(Visit(stmt->body()));
  store_.Forget();  // Control may transfer here via 'break'.
}


void AstTyper::VisitTryCatchStatement(TryCatchStatement* stmt) {
  Effects try_effects = EnterEffects();
  RECURSE(Visit(stmt->try_block()));
  ExitEffects();
  Effects catch_effects = EnterEffects();
  store_.Forget();  // Control may transfer here via 'throw'.
  RECURSE(Visit(stmt->catch_block()));
  ExitEffects();
  try_effects.Alt(catch_effects);
  store_.Seq(try_effects);
  // At this point, only variables that were reassigned in the catch block are
  // still remembered.
}


void AstTyper::VisitTryFinallyStatement(TryFinallyStatement* stmt) {
  RECURSE(Visit(stmt->try_block()));
  store_.Forget();  // Control may transfer here via 'throw'.
  RECURSE(Visit(stmt->finally_block()));
}


void AstTyper::VisitDebuggerStatement(DebuggerStatement* stmt) {
  store_.Forget();  // May do whatever.
}


void AstTyper::VisitFunctionLiteral(FunctionLiteral* expr) {}


void AstTyper::VisitClassLiteral(ClassLiteral* expr) {}


void AstTyper::VisitNativeFunctionLiteral(NativeFunctionLiteral* expr) {
}


void AstTyper::VisitDoExpression(DoExpression* expr) {
  RECURSE(VisitBlock(expr->block()));
  RECURSE(VisitVariableProxy(expr->result()));
  NarrowType(expr, expr->result()->bounds());
}


void AstTyper::VisitConditional(Conditional* expr) {
  // Collect type feedback.
  expr->condition()->RecordToBooleanTypeFeedback(oracle());

  RECURSE(Visit(expr->condition()));
  Effects then_effects = EnterEffects();
  RECURSE(Visit(expr->then_expression()));
  ExitEffects();
  Effects else_effects = EnterEffects();
  RECURSE(Visit(expr->else_expression()));
  ExitEffects();
  then_effects.Alt(else_effects);
  store_.Seq(then_effects);

  NarrowType(expr, Bounds::Either(
      expr->then_expression()->bounds(),
      expr->else_expression()->bounds(), zone()));
}


void AstTyper::VisitVariableProxy(VariableProxy* expr) {
  Variable* var = expr->var();
  if (var->IsStackAllocated()) {
    NarrowType(expr, store_.LookupBounds(variable_index(var)));
  }
}


void AstTyper::VisitLiteral(Literal* expr) {
  Type* type = Type::Constant(expr->value(), zone());
  NarrowType(expr, Bounds(type));
}


void AstTyper::VisitRegExpLiteral(RegExpLiteral* expr) {
  // TODO(rossberg): Reintroduce RegExp type.
  NarrowType(expr, Bounds(Type::Object()));
}


void AstTyper::VisitObjectLiteral(ObjectLiteral* expr) {
  ZoneList<ObjectLiteral::Property*>* properties = expr->properties();
  for (int i = 0; i < properties->length(); ++i) {
    ObjectLiteral::Property* prop = properties->at(i);

    // Collect type feedback.
    if ((prop->kind() == ObjectLiteral::Property::MATERIALIZED_LITERAL &&
        !CompileTimeValue::IsCompileTimeValue(prop->value())) ||
        prop->kind() == ObjectLiteral::Property::COMPUTED) {
      if (!prop->is_computed_name() &&
          prop->key()->AsLiteral()->value()->IsInternalizedString() &&
          prop->emit_store()) {
        // Record type feed back for the property.
        FeedbackVectorSlot slot = prop->GetSlot();
        SmallMapList maps;
        oracle()->CollectReceiverTypes(slot, &maps);
        prop->set_receiver_type(maps.length() == 1 ? maps.at(0)
                                                   : Handle<Map>::null());
      }
    }

    RECURSE(Visit(prop->value()));
  }

  NarrowType(expr, Bounds(Type::Object()));
}


void AstTyper::VisitArrayLiteral(ArrayLiteral* expr) {
  ZoneList<Expression*>* values = expr->values();
  for (int i = 0; i < values->length(); ++i) {
    Expression* value = values->at(i);
    RECURSE(Visit(value));
  }

  NarrowType(expr, Bounds(Type::Object()));
}


void AstTyper::VisitAssignment(Assignment* expr) {
  // Collect type feedback.
  Property* prop = expr->target()->AsProperty();
  if (prop != NULL) {
    FeedbackVectorSlot slot = expr->AssignmentSlot();
    expr->set_is_uninitialized(oracle()->StoreIsUninitialized(slot));
    if (!expr->IsUninitialized()) {
      SmallMapList* receiver_types = expr->GetReceiverTypes();
      if (prop->key()->IsPropertyName()) {
        Literal* lit_key = prop->key()->AsLiteral();
        DCHECK(lit_key != NULL && lit_key->value()->IsString());
        Handle<String> name = Handle<String>::cast(lit_key->value());
        oracle()->AssignmentReceiverTypes(slot, name, receiver_types);
      } else {
        KeyedAccessStoreMode store_mode;
        IcCheckType key_type;
        oracle()->KeyedAssignmentReceiverTypes(slot, receiver_types,
                                               &store_mode, &key_type);
        expr->set_store_mode(store_mode);
        expr->set_key_type(key_type);
      }
    }
  }

  Expression* rhs =
      expr->is_compound() ? expr->binary_operation() : expr->value();
  RECURSE(Visit(expr->target()));
  RECURSE(Visit(rhs));
  NarrowType(expr, rhs->bounds());

  VariableProxy* proxy = expr->target()->AsVariableProxy();
  if (proxy != NULL && proxy->var()->IsStackAllocated()) {
    store_.Seq(variable_index(proxy->var()), Effect(expr->bounds()));
  }
}


void AstTyper::VisitYield(Yield* expr) {
  RECURSE(Visit(expr->generator_object()));
  RECURSE(Visit(expr->expression()));

  // We don't know anything about the result type.
}


void AstTyper::VisitThrow(Throw* expr) {
  RECURSE(Visit(expr->exception()));
  // TODO(rossberg): is it worth having a non-termination effect?

  NarrowType(expr, Bounds(Type::None()));
}


void AstTyper::VisitProperty(Property* expr) {
  // Collect type feedback.
  FeedbackVectorSlot slot = expr->PropertyFeedbackSlot();
  expr->set_inline_cache_state(oracle()->LoadInlineCacheState(slot));

  if (!expr->IsUninitialized()) {
    if (expr->key()->IsPropertyName()) {
      Literal* lit_key = expr->key()->AsLiteral();
      DCHECK(lit_key != NULL && lit_key->value()->IsString());
      Handle<String> name = Handle<String>::cast(lit_key->value());
      oracle()->PropertyReceiverTypes(slot, name, expr->GetReceiverTypes());
    } else {
      bool is_string;
      IcCheckType key_type;
      oracle()->KeyedPropertyReceiverTypes(slot, expr->GetReceiverTypes(),
                                           &is_string, &key_type);
      expr->set_is_string_access(is_string);
      expr->set_key_type(key_type);
    }
  }

  RECURSE(Visit(expr->obj()));
  RECURSE(Visit(expr->key()));

  // We don't know anything about the result type.
}


void AstTyper::VisitCall(Call* expr) {
  // Collect type feedback.
  RECURSE(Visit(expr->expression()));
  bool is_uninitialized = true;
  if (expr->IsUsingCallFeedbackICSlot(isolate_)) {
    FeedbackVectorSlot slot = expr->CallFeedbackICSlot();
    is_uninitialized = oracle()->CallIsUninitialized(slot);
    if (!expr->expression()->IsProperty() &&
        oracle()->CallIsMonomorphic(slot)) {
      expr->set_target(oracle()->GetCallTarget(slot));
      Handle<AllocationSite> site = oracle()->GetCallAllocationSite(slot);
      expr->set_allocation_site(site);
    }
  }

  expr->set_is_uninitialized(is_uninitialized);

  ZoneList<Expression*>* args = expr->arguments();
  for (int i = 0; i < args->length(); ++i) {
    Expression* arg = args->at(i);
    RECURSE(Visit(arg));
  }

  VariableProxy* proxy = expr->expression()->AsVariableProxy();
  if (proxy != NULL && proxy->var()->is_possibly_eval(isolate_)) {
    store_.Forget();  // Eval could do whatever to local variables.
  }

  // We don't know anything about the result type.
}


void AstTyper::VisitCallNew(CallNew* expr) {
  // Collect type feedback.
  FeedbackVectorSlot allocation_site_feedback_slot =
      expr->CallNewFeedbackSlot();
  expr->set_allocation_site(
      oracle()->GetCallNewAllocationSite(allocation_site_feedback_slot));
  bool monomorphic =
      oracle()->CallNewIsMonomorphic(expr->CallNewFeedbackSlot());
  expr->set_is_monomorphic(monomorphic);
  if (monomorphic) {
    expr->set_target(oracle()->GetCallNewTarget(expr->CallNewFeedbackSlot()));
  }

  RECURSE(Visit(expr->expression()));
  ZoneList<Expression*>* args = expr->arguments();
  for (int i = 0; i < args->length(); ++i) {
    Expression* arg = args->at(i);
    RECURSE(Visit(arg));
  }

  NarrowType(expr, Bounds(Type::None(), Type::Receiver()));
}


void AstTyper::VisitCallRuntime(CallRuntime* expr) {
  ZoneList<Expression*>* args = expr->arguments();
  for (int i = 0; i < args->length(); ++i) {
    Expression* arg = args->at(i);
    RECURSE(Visit(arg));
  }

  // We don't know anything about the result type.
}


void AstTyper::VisitUnaryOperation(UnaryOperation* expr) {
  // Collect type feedback.
  if (expr->op() == Token::NOT) {
    // TODO(rossberg): only do in test or value context.
    expr->expression()->RecordToBooleanTypeFeedback(oracle());
  }

  RECURSE(Visit(expr->expression()));

  switch (expr->op()) {
    case Token::NOT:
    case Token::DELETE:
      NarrowType(expr, Bounds(Type::Boolean()));
      break;
    case Token::VOID:
      NarrowType(expr, Bounds(Type::Undefined()));
      break;
    case Token::TYPEOF:
      NarrowType(expr, Bounds(Type::InternalizedString()));
      break;
    default:
      UNREACHABLE();
  }
}


void AstTyper::VisitCountOperation(CountOperation* expr) {
  // Collect type feedback.
  FeedbackVectorSlot slot = expr->CountSlot();
  KeyedAccessStoreMode store_mode;
  IcCheckType key_type;
  oracle()->GetStoreModeAndKeyType(slot, &store_mode, &key_type);
  oracle()->CountReceiverTypes(slot, expr->GetReceiverTypes());
  expr->set_store_mode(store_mode);
  expr->set_key_type(key_type);
  expr->set_type(oracle()->CountType(expr->CountBinOpFeedbackId()));
  // TODO(rossberg): merge the count type with the generic expression type.

  RECURSE(Visit(expr->expression()));

  NarrowType(expr, Bounds(Type::SignedSmall(), Type::Number()));

  VariableProxy* proxy = expr->expression()->AsVariableProxy();
  if (proxy != NULL && proxy->var()->IsStackAllocated()) {
    store_.Seq(variable_index(proxy->var()), Effect(expr->bounds()));
  }
}


void AstTyper::VisitBinaryOperation(BinaryOperation* expr) {
  // Collect type feedback.
  Type* type;
  Type* left_type;
  Type* right_type;
  Maybe<int> fixed_right_arg = Nothing<int>();
  Handle<AllocationSite> allocation_site;
  oracle()->BinaryType(expr->BinaryOperationFeedbackId(),
      &left_type, &right_type, &type, &fixed_right_arg,
      &allocation_site, expr->op());
  NarrowLowerType(expr, type);
  NarrowLowerType(expr->left(), left_type);
  NarrowLowerType(expr->right(), right_type);
  expr->set_allocation_site(allocation_site);
  expr->set_fixed_right_arg(fixed_right_arg);
  if (expr->op() == Token::OR || expr->op() == Token::AND) {
    expr->left()->RecordToBooleanTypeFeedback(oracle());
  }

  switch (expr->op()) {
    case Token::COMMA:
      RECURSE(Visit(expr->left()));
      RECURSE(Visit(expr->right()));
      NarrowType(expr, expr->right()->bounds());
      break;
    case Token::OR:
    case Token::AND: {
      Effects left_effects = EnterEffects();
      RECURSE(Visit(expr->left()));
      ExitEffects();
      Effects right_effects = EnterEffects();
      RECURSE(Visit(expr->right()));
      ExitEffects();
      left_effects.Alt(right_effects);
      store_.Seq(left_effects);

      NarrowType(expr, Bounds::Either(
          expr->left()->bounds(), expr->right()->bounds(), zone()));
      break;
    }
    case Token::BIT_OR:
    case Token::BIT_AND: {
      RECURSE(Visit(expr->left()));
      RECURSE(Visit(expr->right()));
      Type* upper = Type::Union(
          expr->left()->bounds().upper, expr->right()->bounds().upper, zone());
      if (!upper->Is(Type::Signed32())) upper = Type::Signed32();
      Type* lower = Type::Intersect(Type::SignedSmall(), upper, zone());
      NarrowType(expr, Bounds(lower, upper));
      break;
    }
    case Token::BIT_XOR:
    case Token::SHL:
    case Token::SAR:
      RECURSE(Visit(expr->left()));
      RECURSE(Visit(expr->right()));
      NarrowType(expr, Bounds(Type::SignedSmall(), Type::Signed32()));
      break;
    case Token::SHR:
      RECURSE(Visit(expr->left()));
      RECURSE(Visit(expr->right()));
      // TODO(rossberg): The upper bound would be Unsigned32, but since there
      // is no 'positive Smi' type for the lower bound, we use the smallest
      // union of Smi and Unsigned32 as upper bound instead.
      NarrowType(expr, Bounds(Type::SignedSmall(), Type::Number()));
      break;
    case Token::ADD: {
      RECURSE(Visit(expr->left()));
      RECURSE(Visit(expr->right()));
      Bounds l = expr->left()->bounds();
      Bounds r = expr->right()->bounds();
      Type* lower =
          !l.lower->IsInhabited() || !r.lower->IsInhabited()
              ? Type::None()
              : l.lower->Is(Type::String()) || r.lower->Is(Type::String())
                    ? Type::String()
                    : l.lower->Is(Type::Number()) && r.lower->Is(Type::Number())
                          ? Type::SignedSmall()
                          : Type::None();
      Type* upper =
          l.upper->Is(Type::String()) || r.upper->Is(Type::String())
              ? Type::String()
              : l.upper->Is(Type::Number()) && r.upper->Is(Type::Number())
                    ? Type::Number()
                    : Type::NumberOrString();
      NarrowType(expr, Bounds(lower, upper));
      break;
    }
    case Token::SUB:
    case Token::MUL:
    case Token::DIV:
    case Token::MOD:
      RECURSE(Visit(expr->left()));
      RECURSE(Visit(expr->right()));
      NarrowType(expr, Bounds(Type::SignedSmall(), Type::Number()));
      break;
    default:
      UNREACHABLE();
  }
}


void AstTyper::VisitCompareOperation(CompareOperation* expr) {
  // Collect type feedback.
  Type* left_type;
  Type* right_type;
  Type* combined_type;
  oracle()->CompareType(expr->CompareOperationFeedbackId(),
      &left_type, &right_type, &combined_type);
  NarrowLowerType(expr->left(), left_type);
  NarrowLowerType(expr->right(), right_type);
  expr->set_combined_type(combined_type);

  RECURSE(Visit(expr->left()));
  RECURSE(Visit(expr->right()));

  NarrowType(expr, Bounds(Type::Boolean()));
}


void AstTyper::VisitSpread(Spread* expr) { UNREACHABLE(); }


void AstTyper::VisitEmptyParentheses(EmptyParentheses* expr) {
  UNREACHABLE();
}


void AstTyper::VisitThisFunction(ThisFunction* expr) {}


void AstTyper::VisitSuperPropertyReference(SuperPropertyReference* expr) {}


void AstTyper::VisitSuperCallReference(SuperCallReference* expr) {}


void AstTyper::VisitRewritableAssignmentExpression(
    RewritableAssignmentExpression* expr) {
  Visit(expr->expression());
}


void AstTyper::VisitDeclarations(ZoneList<Declaration*>* decls) {
  for (int i = 0; i < decls->length(); ++i) {
    Declaration* decl = decls->at(i);
    RECURSE(Visit(decl));
  }
}


void AstTyper::VisitVariableDeclaration(VariableDeclaration* declaration) {
}


void AstTyper::VisitFunctionDeclaration(FunctionDeclaration* declaration) {
  RECURSE(Visit(declaration->fun()));
}


void AstTyper::VisitImportDeclaration(ImportDeclaration* declaration) {
}


void AstTyper::VisitExportDeclaration(ExportDeclaration* declaration) {
}


}  // namespace internal
}  // namespace v8
