// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/runtime/runtime-utils.h"

#include "src/arguments.h"
#include "src/compiler.h"
#include "src/deoptimizer.h"
#include "src/frames-inl.h"
#include "src/full-codegen/full-codegen.h"
#include "src/isolate-inl.h"
#include "src/messages.h"
#include "src/v8threads.h"
#include "src/vm-state-inl.h"

namespace v8 {
namespace internal {

RUNTIME_FUNCTION(Runtime_CompileLazy) {
  HandleScope scope(isolate);
  DCHECK(args.length() == 1);
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, function, 0);
#ifdef DEBUG
  if (FLAG_trace_lazy && !function->shared()->is_compiled()) {
    PrintF("[unoptimized: ");
    function->PrintName();
    PrintF("]\n");
  }
#endif
  StackLimitCheck check(isolate);
  if (check.JsHasOverflowed(1 * KB)) return isolate->StackOverflow();

  // Compile the target function.
  DCHECK(function->shared()->allows_lazy_compilation());

  // There is one special case where we have optimized code but we
  // couldn't find a literals array for the native context. That's with
  // FLAG_turbo_cache_shared_code.
  if (FLAG_turbo_cache_shared_code) {
    SharedFunctionInfo* shared = function->shared();
    CodeAndLiterals result;
    result = shared->SearchOptimizedCodeMap(*isolate->native_context(),
                                            BailoutId::None());
    if (result.code != nullptr) {
      function->ReplaceCode(result.code);
      JSFunction::EnsureLiterals(function);
      return result.code;
    }
  }

  Handle<Code> code;
  ASSIGN_RETURN_FAILURE_ON_EXCEPTION(isolate, code,
                                     Compiler::GetLazyCode(function));
  DCHECK(code->IsJavaScriptCode());

  function->ReplaceCode(*code);
  JSFunction::EnsureLiterals(function);
  return *code;
}


namespace {

Object* CompileOptimized(Isolate* isolate, Handle<JSFunction> function,
                         Compiler::ConcurrencyMode mode) {
  StackLimitCheck check(isolate);
  if (check.JsHasOverflowed(1 * KB)) return isolate->StackOverflow();

  Handle<Code> code;
  if (Compiler::GetOptimizedCode(function, mode).ToHandle(&code)) {
    // Optimization succeeded, return optimized code.
    function->ReplaceCode(*code);
  } else {
    // Optimization failed, get unoptimized code.
    if (isolate->has_pending_exception()) {  // Possible stack overflow.
      return isolate->heap()->exception();
    }
    code = Handle<Code>(function->shared()->code(), isolate);
    if (code->kind() != Code::FUNCTION &&
        code->kind() != Code::OPTIMIZED_FUNCTION) {
      ASSIGN_RETURN_FAILURE_ON_EXCEPTION(
          isolate, code, Compiler::GetUnoptimizedCode(function));
    }
    function->ReplaceCode(*code);
  }

  DCHECK(function->code()->kind() == Code::FUNCTION ||
         function->code()->kind() == Code::OPTIMIZED_FUNCTION ||
         (function->code()->is_interpreter_entry_trampoline() &&
          function->shared()->HasBytecodeArray()) ||
         function->IsInOptimizationQueue());
  return function->code();
}

}  // namespace


RUNTIME_FUNCTION(Runtime_CompileOptimized_Concurrent) {
  HandleScope scope(isolate);
  DCHECK(args.length() == 1);
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, function, 0);
  return CompileOptimized(isolate, function, Compiler::CONCURRENT);
}


RUNTIME_FUNCTION(Runtime_CompileOptimized_NotConcurrent) {
  HandleScope scope(isolate);
  DCHECK(args.length() == 1);
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, function, 0);
  return CompileOptimized(isolate, function, Compiler::NOT_CONCURRENT);
}


RUNTIME_FUNCTION(Runtime_NotifyStubFailure) {
  HandleScope scope(isolate);
  DCHECK(args.length() == 0);
  Deoptimizer* deoptimizer = Deoptimizer::Grab(isolate);
  DCHECK(AllowHeapAllocation::IsAllowed());
  delete deoptimizer;
  return isolate->heap()->undefined_value();
}


class ActivationsFinder : public ThreadVisitor {
 public:
  Code* code_;
  bool has_code_activations_;

  explicit ActivationsFinder(Code* code)
      : code_(code), has_code_activations_(false) {}

  void VisitThread(Isolate* isolate, ThreadLocalTop* top) {
    JavaScriptFrameIterator it(isolate, top);
    VisitFrames(&it);
  }

  void VisitFrames(JavaScriptFrameIterator* it) {
    for (; !it->done(); it->Advance()) {
      JavaScriptFrame* frame = it->frame();
      if (code_->contains(frame->pc())) has_code_activations_ = true;
    }
  }
};


RUNTIME_FUNCTION(Runtime_NotifyDeoptimized) {
  HandleScope scope(isolate);
  DCHECK(args.length() == 1);
  CONVERT_SMI_ARG_CHECKED(type_arg, 0);
  Deoptimizer::BailoutType type =
      static_cast<Deoptimizer::BailoutType>(type_arg);
  Deoptimizer* deoptimizer = Deoptimizer::Grab(isolate);
  DCHECK(AllowHeapAllocation::IsAllowed());

  Handle<JSFunction> function = deoptimizer->function();
  Handle<Code> optimized_code = deoptimizer->compiled_code();

  DCHECK(optimized_code->kind() == Code::OPTIMIZED_FUNCTION);
  DCHECK(type == deoptimizer->bailout_type());

  // Make sure to materialize objects before causing any allocation.
  JavaScriptFrameIterator it(isolate);
  deoptimizer->MaterializeHeapObjects(&it);
  delete deoptimizer;

  JavaScriptFrame* frame = it.frame();
  RUNTIME_ASSERT(frame->function()->IsJSFunction());
  DCHECK(frame->function() == *function);

  // Ensure the context register is updated for materialized objects.
  JavaScriptFrameIterator top_it(isolate);
  JavaScriptFrame* top_frame = top_it.frame();
  isolate->set_context(Context::cast(top_frame->context()));

  if (type == Deoptimizer::LAZY) {
    return isolate->heap()->undefined_value();
  }

  // Search for other activations of the same function and code.
  ActivationsFinder activations_finder(*optimized_code);
  activations_finder.VisitFrames(&it);
  isolate->thread_manager()->IterateArchivedThreads(&activations_finder);

  if (!activations_finder.has_code_activations_) {
    if (function->code() == *optimized_code) {
      if (FLAG_trace_deopt) {
        PrintF("[removing optimized code for: ");
        function->PrintName();
        PrintF("]\n");
      }
      function->ReplaceCode(function->shared()->code());
    }
    // Evict optimized code for this function from the cache so that it
    // doesn't get used for new closures.
    function->shared()->EvictFromOptimizedCodeMap(*optimized_code,
                                                  "notify deoptimized");
  } else {
    // TODO(titzer): we should probably do DeoptimizeCodeList(code)
    // unconditionally if the code is not already marked for deoptimization.
    // If there is an index by shared function info, all the better.
    Deoptimizer::DeoptimizeFunction(*function);
  }

  return isolate->heap()->undefined_value();
}


static bool IsSuitableForOnStackReplacement(Isolate* isolate,
                                            Handle<JSFunction> function) {
  // Keep track of whether we've succeeded in optimizing.
  if (function->shared()->optimization_disabled()) return false;
  // If we are trying to do OSR when there are already optimized
  // activations of the function, it means (a) the function is directly or
  // indirectly recursive and (b) an optimized invocation has been
  // deoptimized so that we are currently in an unoptimized activation.
  // Check for optimized activations of this function.
  for (JavaScriptFrameIterator it(isolate); !it.done(); it.Advance()) {
    JavaScriptFrame* frame = it.frame();
    if (frame->is_optimized() && frame->function() == *function) return false;
  }

  return true;
}


RUNTIME_FUNCTION(Runtime_CompileForOnStackReplacement) {
  HandleScope scope(isolate);
  DCHECK(args.length() == 1);
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, function, 0);
  Handle<Code> caller_code(function->shared()->code());

  // We're not prepared to handle a function with arguments object.
  DCHECK(!function->shared()->uses_arguments());

  RUNTIME_ASSERT(FLAG_use_osr);

  // Passing the PC in the javascript frame from the caller directly is
  // not GC safe, so we walk the stack to get it.
  JavaScriptFrameIterator it(isolate);
  JavaScriptFrame* frame = it.frame();
  if (!caller_code->contains(frame->pc())) {
    // Code on the stack may not be the code object referenced by the shared
    // function info.  It may have been replaced to include deoptimization data.
    caller_code = Handle<Code>(frame->LookupCode());
  }

  uint32_t pc_offset =
      static_cast<uint32_t>(frame->pc() - caller_code->instruction_start());

#ifdef DEBUG
  DCHECK_EQ(frame->function(), *function);
  DCHECK_EQ(frame->LookupCode(), *caller_code);
  DCHECK(caller_code->contains(frame->pc()));
#endif  // DEBUG


  BailoutId ast_id = caller_code->TranslatePcOffsetToAstId(pc_offset);
  DCHECK(!ast_id.IsNone());

  // Disable concurrent OSR for asm.js, to enable frame specialization.
  Compiler::ConcurrencyMode mode = (isolate->concurrent_osr_enabled() &&
                                    !function->shared()->asm_function() &&
                                    function->shared()->ast_node_count() > 512)
                                       ? Compiler::CONCURRENT
                                       : Compiler::NOT_CONCURRENT;
  Handle<Code> result = Handle<Code>::null();

  OptimizedCompileJob* job = NULL;
  if (mode == Compiler::CONCURRENT) {
    // Gate the OSR entry with a stack check.
    BackEdgeTable::AddStackCheck(caller_code, pc_offset);
    // Poll already queued compilation jobs.
    OptimizingCompileDispatcher* dispatcher =
        isolate->optimizing_compile_dispatcher();
    if (dispatcher->IsQueuedForOSR(function, ast_id)) {
      if (FLAG_trace_osr) {
        PrintF("[OSR - Still waiting for queued: ");
        function->PrintName();
        PrintF(" at AST id %d]\n", ast_id.ToInt());
      }
      return NULL;
    }

    job = dispatcher->FindReadyOSRCandidate(function, ast_id);
  }

  if (job != NULL) {
    if (FLAG_trace_osr) {
      PrintF("[OSR - Found ready: ");
      function->PrintName();
      PrintF(" at AST id %d]\n", ast_id.ToInt());
    }
    result = Compiler::GetConcurrentlyOptimizedCode(job);
  } else if (IsSuitableForOnStackReplacement(isolate, function)) {
    if (FLAG_trace_osr) {
      PrintF("[OSR - Compiling: ");
      function->PrintName();
      PrintF(" at AST id %d]\n", ast_id.ToInt());
    }
    MaybeHandle<Code> maybe_result = Compiler::GetOptimizedCode(
        function, mode, ast_id,
        (mode == Compiler::NOT_CONCURRENT) ? frame : nullptr);
    if (maybe_result.ToHandle(&result) &&
        result.is_identical_to(isolate->builtins()->InOptimizationQueue())) {
      // Optimization is queued.  Return to check later.
      return NULL;
    }
  }

  // Revert the patched back edge table, regardless of whether OSR succeeds.
  BackEdgeTable::Revert(isolate, *caller_code);

  // Check whether we ended up with usable optimized code.
  if (!result.is_null() && result->kind() == Code::OPTIMIZED_FUNCTION) {
    DeoptimizationInputData* data =
        DeoptimizationInputData::cast(result->deoptimization_data());

    if (data->OsrPcOffset()->value() >= 0) {
      DCHECK(BailoutId(data->OsrAstId()->value()) == ast_id);
      if (FLAG_trace_osr) {
        PrintF("[OSR - Entry at AST id %d, offset %d in optimized code]\n",
               ast_id.ToInt(), data->OsrPcOffset()->value());
      }
      // TODO(titzer): this is a massive hack to make the deopt counts
      // match. Fix heuristics for reenabling optimizations!
      function->shared()->increment_deopt_count();

      if (result->is_turbofanned()) {
        // TurboFanned OSR code cannot be installed into the function.
        // But the function is obviously hot, so optimize it next time.
        function->ReplaceCode(
            isolate->builtins()->builtin(Builtins::kCompileOptimized));
      } else {
        // Crankshafted OSR code can be installed into the function.
        function->ReplaceCode(*result);
      }
      return *result;
    }
  }

  // Failed.
  if (FLAG_trace_osr) {
    PrintF("[OSR - Failed: ");
    function->PrintName();
    PrintF(" at AST id %d]\n", ast_id.ToInt());
  }

  if (!function->IsOptimized()) {
    function->ReplaceCode(function->shared()->code());
  }
  return NULL;
}


RUNTIME_FUNCTION(Runtime_TryInstallOptimizedCode) {
  HandleScope scope(isolate);
  DCHECK(args.length() == 1);
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, function, 0);

  // First check if this is a real stack overflow.
  StackLimitCheck check(isolate);
  if (check.JsHasOverflowed()) {
    SealHandleScope shs(isolate);
    return isolate->StackOverflow();
  }

  isolate->optimizing_compile_dispatcher()->InstallOptimizedFunctions();
  return (function->IsOptimized()) ? function->code()
                                   : function->shared()->code();
}


bool CodeGenerationFromStringsAllowed(Isolate* isolate,
                                      Handle<Context> context) {
  DCHECK(context->allow_code_gen_from_strings()->IsFalse());
  // Check with callback if set.
  AllowCodeGenerationFromStringsCallback callback =
      isolate->allow_code_gen_callback();
  if (callback == NULL) {
    // No callback set and code generation disallowed.
    return false;
  } else {
    // Callback set. Let it decide if code generation is allowed.
    VMState<EXTERNAL> state(isolate);
    return callback(v8::Utils::ToLocal(context));
  }
}


static Object* CompileGlobalEval(Isolate* isolate, Handle<String> source,
                                 Handle<SharedFunctionInfo> outer_info,
                                 LanguageMode language_mode,
                                 int scope_position) {
  Handle<Context> context = Handle<Context>(isolate->context());
  Handle<Context> native_context = Handle<Context>(context->native_context());

  // Check if native context allows code generation from
  // strings. Throw an exception if it doesn't.
  if (native_context->allow_code_gen_from_strings()->IsFalse() &&
      !CodeGenerationFromStringsAllowed(isolate, native_context)) {
    Handle<Object> error_message =
        native_context->ErrorMessageForCodeGenerationFromStrings();
    Handle<Object> error;
    MaybeHandle<Object> maybe_error = isolate->factory()->NewEvalError(
        MessageTemplate::kCodeGenFromStrings, error_message);
    if (maybe_error.ToHandle(&error)) isolate->Throw(*error);
    return isolate->heap()->exception();
  }

  // Deal with a normal eval call with a string argument. Compile it
  // and return the compiled function bound in the local context.
  static const ParseRestriction restriction = NO_PARSE_RESTRICTION;
  Handle<JSFunction> compiled;
  ASSIGN_RETURN_ON_EXCEPTION_VALUE(
      isolate, compiled,
      Compiler::GetFunctionFromEval(source, outer_info, context, language_mode,
                                    restriction, scope_position),
      isolate->heap()->exception());
  return *compiled;
}


RUNTIME_FUNCTION(Runtime_ResolvePossiblyDirectEval) {
  HandleScope scope(isolate);
  DCHECK(args.length() == 5);

  Handle<Object> callee = args.at<Object>(0);

  // If "eval" didn't refer to the original GlobalEval, it's not a
  // direct call to eval.
  // (And even if it is, but the first argument isn't a string, just let
  // execution default to an indirect call to eval, which will also return
  // the first argument without doing anything).
  if (*callee != isolate->native_context()->global_eval_fun() ||
      !args[1]->IsString()) {
    return *callee;
  }

  DCHECK(args[3]->IsSmi());
  DCHECK(is_valid_language_mode(args.smi_at(3)));
  LanguageMode language_mode = static_cast<LanguageMode>(args.smi_at(3));
  DCHECK(args[4]->IsSmi());
  Handle<SharedFunctionInfo> outer_info(args.at<JSFunction>(2)->shared(),
                                        isolate);
  return CompileGlobalEval(isolate, args.at<String>(1), outer_info,
                           language_mode, args.smi_at(4));
}
}  // namespace internal
}  // namespace v8
