// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_FRAMES_H_
#define V8_FRAMES_H_

#include "src/allocation.h"
#include "src/handles.h"
#include "src/safepoint-table.h"

namespace v8 {
namespace internal {

#if V8_TARGET_ARCH_ARM64
typedef uint64_t RegList;
#else
typedef uint32_t RegList;
#endif

// Get the number of registers in a given register list.
int NumRegs(RegList list);

void SetUpJSCallerSavedCodeData();

// Return the code of the n-th saved register available to JavaScript.
int JSCallerSavedCode(int n);


// Forward declarations.
class ExternalCallbackScope;
class StackFrameIteratorBase;
class ThreadLocalTop;
class Isolate;

class InnerPointerToCodeCache {
 public:
  struct InnerPointerToCodeCacheEntry {
    Address inner_pointer;
    Code* code;
    SafepointEntry safepoint_entry;
  };

  explicit InnerPointerToCodeCache(Isolate* isolate) : isolate_(isolate) {
    Flush();
  }

  Code* GcSafeFindCodeForInnerPointer(Address inner_pointer);
  Code* GcSafeCastToCode(HeapObject* object, Address inner_pointer);

  void Flush() {
    memset(&cache_[0], 0, sizeof(cache_));
  }

  InnerPointerToCodeCacheEntry* GetCacheEntry(Address inner_pointer);

 private:
  InnerPointerToCodeCacheEntry* cache(int index) { return &cache_[index]; }

  Isolate* isolate_;

  static const int kInnerPointerToCodeCacheSize = 1024;
  InnerPointerToCodeCacheEntry cache_[kInnerPointerToCodeCacheSize];

  DISALLOW_COPY_AND_ASSIGN(InnerPointerToCodeCache);
};


// Every try-block pushes the context register.
class TryBlockConstant : public AllStatic {
 public:
  static const int kElementCount = 1;
};


class StackHandlerConstants : public AllStatic {
 public:
  static const int kNextOffset = 0 * kPointerSize;

  static const int kSize = kNextOffset + kPointerSize;
  static const int kSlotCount = kSize >> kPointerSizeLog2;
};


class StackHandler BASE_EMBEDDED {
 public:
  // Get the address of this stack handler.
  inline Address address() const;

  // Get the next stack handler in the chain.
  inline StackHandler* next() const;

  // Conversion support.
  static inline StackHandler* FromAddress(Address address);

 private:
  DISALLOW_IMPLICIT_CONSTRUCTORS(StackHandler);
};


#define STACK_FRAME_TYPE_LIST(V)                         \
  V(ENTRY, EntryFrame)                                   \
  V(ENTRY_CONSTRUCT, EntryConstructFrame)                \
  V(EXIT, ExitFrame)                                     \
  V(JAVA_SCRIPT, JavaScriptFrame)                        \
  V(OPTIMIZED, OptimizedFrame)                           \
  V(INTERPRETED, InterpretedFrame)                       \
  V(STUB, StubFrame)                                     \
  V(STUB_FAILURE_TRAMPOLINE, StubFailureTrampolineFrame) \
  V(INTERNAL, InternalFrame)                             \
  V(CONSTRUCT, ConstructFrame)                           \
  V(ARGUMENTS_ADAPTOR, ArgumentsAdaptorFrame)


class StandardFrameConstants : public AllStatic {
 public:
  // Fixed part of the frame consists of return address, caller fp,
  // constant pool (if FLAG_enable_embedded_constant_pool), context, and
  // function. StandardFrame::IterateExpressions assumes that kLastObjectOffset
  // is the last object pointer.
  static const int kCPSlotSize =
      FLAG_enable_embedded_constant_pool ? kPointerSize : 0;
  static const int kFixedFrameSizeFromFp =  2 * kPointerSize + kCPSlotSize;
  static const int kFixedFrameSizeAboveFp = kPCOnStackSize + kFPOnStackSize;
  static const int kFixedFrameSize =
      kFixedFrameSizeAboveFp + kFixedFrameSizeFromFp;
  static const int kFixedSlotCountAboveFp =
      kFixedFrameSizeAboveFp / kPointerSize;
  static const int kFixedSlotCount = kFixedFrameSize / kPointerSize;
  static const int kCPSlotCount = kCPSlotSize / kPointerSize;
  static const int kExpressionsOffset = -3 * kPointerSize - kCPSlotSize;
  static const int kMarkerOffset = -2 * kPointerSize - kCPSlotSize;
  static const int kContextOffset = -1 * kPointerSize - kCPSlotSize;
  static const int kConstantPoolOffset = kCPSlotSize ? -1 * kPointerSize : 0;
  static const int kCallerFPOffset = 0 * kPointerSize;
  static const int kCallerPCOffset = +1 * kFPOnStackSize;
  static const int kCallerSPOffset = kCallerPCOffset + 1 * kPCOnStackSize;

  static const int kLastObjectOffset = kContextOffset;
};


class ArgumentsAdaptorFrameConstants : public AllStatic {
 public:
  // FP-relative.
  static const int kLengthOffset = StandardFrameConstants::kExpressionsOffset;

  static const int kFrameSize =
      StandardFrameConstants::kFixedFrameSize + kPointerSize;
};


class InternalFrameConstants : public AllStatic {
 public:
  // FP-relative.
  static const int kCodeOffset = StandardFrameConstants::kExpressionsOffset;
};


class ConstructFrameConstants : public AllStatic {
 public:
  // FP-relative.
  static const int kImplicitReceiverOffset =
      StandardFrameConstants::kExpressionsOffset - 3 * kPointerSize;
  static const int kLengthOffset =
      StandardFrameConstants::kExpressionsOffset - 2 * kPointerSize;
  static const int kAllocationSiteOffset =
      StandardFrameConstants::kExpressionsOffset - 1 * kPointerSize;
  static const int kCodeOffset =
      StandardFrameConstants::kExpressionsOffset - 0 * kPointerSize;

  static const int kFrameSize =
      StandardFrameConstants::kFixedFrameSize + 4 * kPointerSize;
};


class InterpreterFrameConstants : public AllStatic {
 public:
  // Fixed frame includes new.target and bytecode offset.
  static const int kFixedFrameSize =
      StandardFrameConstants::kFixedFrameSize + 2 * kPointerSize;
  static const int kFixedFrameSizeFromFp =
      StandardFrameConstants::kFixedFrameSizeFromFp + 2 * kPointerSize;

  // FP-relative.
  static const int kBytecodeOffsetFromFp =
      -StandardFrameConstants::kFixedFrameSizeFromFp - 2 * kPointerSize;
  static const int kRegisterFilePointerFromFp =
      -StandardFrameConstants::kFixedFrameSizeFromFp - 3 * kPointerSize;

  // Expression index for {StandardFrame::GetExpressionAddress}.
  static const int kBytecodeOffsetExpressionIndex = 1;
  static const int kRegisterFileExpressionIndex = 2;

  // Register file pointer relative.
  static const int kLastParamFromRegisterPointer =
      StandardFrameConstants::kFixedFrameSize + 3 * kPointerSize;

  static const int kBytecodeOffsetFromRegisterPointer = 1 * kPointerSize;
  static const int kNewTargetFromRegisterPointer = 2 * kPointerSize;
  static const int kFunctionFromRegisterPointer = 3 * kPointerSize;
  static const int kContextFromRegisterPointer = 4 * kPointerSize;
};


// Abstract base class for all stack frames.
class StackFrame BASE_EMBEDDED {
 public:
#define DECLARE_TYPE(type, ignore) type,
  enum Type {
    NONE = 0,
    STACK_FRAME_TYPE_LIST(DECLARE_TYPE)
    NUMBER_OF_TYPES,
    // Used by FrameScope to indicate that the stack frame is constructed
    // manually and the FrameScope does not need to emit code.
    MANUAL
  };
#undef DECLARE_TYPE

  // Opaque data type for identifying stack frames. Used extensively
  // by the debugger.
  // ID_MIN_VALUE and ID_MAX_VALUE are specified to ensure that enumeration type
  // has correct value range (see Issue 830 for more details).
  enum Id {
    ID_MIN_VALUE = kMinInt,
    ID_MAX_VALUE = kMaxInt,
    NO_ID = 0
  };

  // Used to mark the outermost JS entry frame.
  enum JsFrameMarker {
    INNER_JSENTRY_FRAME = 0,
    OUTERMOST_JSENTRY_FRAME = 1
  };

  struct State {
    State() : sp(NULL), fp(NULL), pc_address(NULL),
              constant_pool_address(NULL) { }
    Address sp;
    Address fp;
    Address* pc_address;
    Address* constant_pool_address;
  };

  // Copy constructor; it breaks the connection to host iterator
  // (as an iterator usually lives on stack).
  StackFrame(const StackFrame& original) {
    this->state_ = original.state_;
    this->iterator_ = NULL;
    this->isolate_ = original.isolate_;
  }

  // Type testers.
  bool is_entry() const { return type() == ENTRY; }
  bool is_entry_construct() const { return type() == ENTRY_CONSTRUCT; }
  bool is_exit() const { return type() == EXIT; }
  bool is_optimized() const { return type() == OPTIMIZED; }
  bool is_interpreted() const { return type() == INTERPRETED; }
  bool is_arguments_adaptor() const { return type() == ARGUMENTS_ADAPTOR; }
  bool is_internal() const { return type() == INTERNAL; }
  bool is_stub_failure_trampoline() const {
    return type() == STUB_FAILURE_TRAMPOLINE;
  }
  bool is_construct() const { return type() == CONSTRUCT; }
  virtual bool is_standard() const { return false; }

  bool is_java_script() const {
    Type type = this->type();
    return (type == JAVA_SCRIPT) || (type == OPTIMIZED) ||
           (type == INTERPRETED);
  }

  // Accessors.
  Address sp() const { return state_.sp; }
  Address fp() const { return state_.fp; }
  Address caller_sp() const { return GetCallerStackPointer(); }

  // If this frame is optimized and was dynamically aligned return its old
  // unaligned frame pointer.  When the frame is deoptimized its FP will shift
  // up one word and become unaligned.
  Address UnpaddedFP() const;

  Address pc() const { return *pc_address(); }
  void set_pc(Address pc) { *pc_address() = pc; }

  Address constant_pool() const { return *constant_pool_address(); }
  void set_constant_pool(Address constant_pool) {
    *constant_pool_address() = constant_pool;
  }

  virtual void SetCallerFp(Address caller_fp) = 0;

  // Manually changes value of fp in this object.
  void UpdateFp(Address fp) { state_.fp = fp; }

  Address* pc_address() const { return state_.pc_address; }

  Address* constant_pool_address() const {
    return state_.constant_pool_address;
  }

  // Get the id of this stack frame.
  Id id() const { return static_cast<Id>(OffsetFrom(caller_sp())); }

  // Get the top handler from the current stack iterator.
  inline StackHandler* top_handler() const;

  // Get the type of this frame.
  virtual Type type() const = 0;

  // Get the code associated with this frame.
  // This method could be called during marking phase of GC.
  virtual Code* unchecked_code() const = 0;

  // Get the code associated with this frame.
  inline Code* LookupCode() const;

  // Get the code object that contains the given pc.
  static inline Code* GetContainingCode(Isolate* isolate, Address pc);

  // Get the code object containing the given pc and fill in the
  // safepoint entry and the number of stack slots. The pc must be at
  // a safepoint.
  static Code* GetSafepointData(Isolate* isolate,
                                Address pc,
                                SafepointEntry* safepoint_entry,
                                unsigned* stack_slots);

  virtual void Iterate(ObjectVisitor* v) const = 0;
  static void IteratePc(ObjectVisitor* v, Address* pc_address,
                        Address* constant_pool_address, Code* holder);

  // Sets a callback function for return-address rewriting profilers
  // to resolve the location of a return address to the location of the
  // profiler's stashed return address.
  static void SetReturnAddressLocationResolver(
      ReturnAddressLocationResolver resolver);

  // Resolves pc_address through the resolution address function if one is set.
  static inline Address* ResolveReturnAddressLocation(Address* pc_address);

  // Printing support.
  enum PrintMode { OVERVIEW, DETAILS };
  virtual void Print(StringStream* accumulator,
                     PrintMode mode,
                     int index) const { }

  Isolate* isolate() const { return isolate_; }

 protected:
  inline explicit StackFrame(StackFrameIteratorBase* iterator);
  virtual ~StackFrame() { }

  // Compute the stack pointer for the calling frame.
  virtual Address GetCallerStackPointer() const = 0;

  // Printing support.
  static void PrintIndex(StringStream* accumulator,
                         PrintMode mode,
                         int index);

  // Compute the stack frame type for the given state.
  static Type ComputeType(const StackFrameIteratorBase* iterator, State* state);

#ifdef DEBUG
  bool can_access_heap_objects() const;
#endif

 private:
  const StackFrameIteratorBase* iterator_;
  Isolate* isolate_;
  State state_;

  static ReturnAddressLocationResolver return_address_location_resolver_;

  // Fill in the state of the calling frame.
  virtual void ComputeCallerState(State* state) const = 0;

  // Get the type and the state of the calling frame.
  virtual Type GetCallerState(State* state) const;

  static const intptr_t kIsolateTag = 1;

  friend class StackFrameIterator;
  friend class StackFrameIteratorBase;
  friend class StackHandlerIterator;
  friend class SafeStackFrameIterator;

 private:
  void operator=(const StackFrame& original);
};


// Entry frames are used to enter JavaScript execution from C.
class EntryFrame: public StackFrame {
 public:
  Type type() const override { return ENTRY; }

  Code* unchecked_code() const override;

  // Garbage collection support.
  void Iterate(ObjectVisitor* v) const override;

  static EntryFrame* cast(StackFrame* frame) {
    DCHECK(frame->is_entry());
    return static_cast<EntryFrame*>(frame);
  }
  void SetCallerFp(Address caller_fp) override;

 protected:
  inline explicit EntryFrame(StackFrameIteratorBase* iterator);

  // The caller stack pointer for entry frames is always zero. The
  // real information about the caller frame is available through the
  // link to the top exit frame.
  Address GetCallerStackPointer() const override { return 0; }

 private:
  void ComputeCallerState(State* state) const override;
  Type GetCallerState(State* state) const override;

  friend class StackFrameIteratorBase;
};


class EntryConstructFrame: public EntryFrame {
 public:
  Type type() const override { return ENTRY_CONSTRUCT; }

  Code* unchecked_code() const override;

  static EntryConstructFrame* cast(StackFrame* frame) {
    DCHECK(frame->is_entry_construct());
    return static_cast<EntryConstructFrame*>(frame);
  }

 protected:
  inline explicit EntryConstructFrame(StackFrameIteratorBase* iterator);

 private:
  friend class StackFrameIteratorBase;
};


// Exit frames are used to exit JavaScript execution and go to C.
class ExitFrame: public StackFrame {
 public:
  Type type() const override { return EXIT; }

  Code* unchecked_code() const override;

  Object*& code_slot() const;

  // Garbage collection support.
  void Iterate(ObjectVisitor* v) const override;

  void SetCallerFp(Address caller_fp) override;

  static ExitFrame* cast(StackFrame* frame) {
    DCHECK(frame->is_exit());
    return static_cast<ExitFrame*>(frame);
  }

  // Compute the state and type of an exit frame given a frame
  // pointer. Used when constructing the first stack frame seen by an
  // iterator and the frames following entry frames.
  static Type GetStateForFramePointer(Address fp, State* state);
  static Address ComputeStackPointer(Address fp);
  static void FillState(Address fp, Address sp, State* state);

 protected:
  inline explicit ExitFrame(StackFrameIteratorBase* iterator);

  Address GetCallerStackPointer() const override;

 private:
  void ComputeCallerState(State* state) const override;

  friend class StackFrameIteratorBase;
};


class StandardFrame: public StackFrame {
 public:
  // Testers.
  bool is_standard() const override { return true; }

  // Accessors.
  inline Object* context() const;

  // Access the expressions in the stack frame including locals.
  inline Object* GetExpression(int index) const;
  inline void SetExpression(int index, Object* value);
  int ComputeExpressionsCount() const;
  static Object* GetExpression(Address fp, int index);

  void SetCallerFp(Address caller_fp) override;

  static StandardFrame* cast(StackFrame* frame) {
    DCHECK(frame->is_standard());
    return static_cast<StandardFrame*>(frame);
  }

 protected:
  inline explicit StandardFrame(StackFrameIteratorBase* iterator);

  void ComputeCallerState(State* state) const override;

  // Accessors.
  inline Address caller_fp() const;
  inline Address caller_pc() const;

  // Computes the address of the PC field in the standard frame given
  // by the provided frame pointer.
  static inline Address ComputePCAddress(Address fp);

  // Computes the address of the constant pool  field in the standard
  // frame given by the provided frame pointer.
  static inline Address ComputeConstantPoolAddress(Address fp);

  // Iterate over expression stack including stack handlers, locals,
  // and parts of the fixed part including context and code fields.
  void IterateExpressions(ObjectVisitor* v) const;

  // Returns the address of the n'th expression stack element.
  Address GetExpressionAddress(int n) const;
  static Address GetExpressionAddress(Address fp, int n);

  // Determines if the standard frame for the given frame pointer is
  // an arguments adaptor frame.
  static inline bool IsArgumentsAdaptorFrame(Address fp);

  // Determines if the standard frame for the given frame pointer is a
  // construct frame.
  static inline bool IsConstructFrame(Address fp);

  // Used by OptimizedFrames and StubFrames.
  void IterateCompiledFrame(ObjectVisitor* v) const;

 private:
  friend class StackFrame;
  friend class SafeStackFrameIterator;
};


class FrameSummary BASE_EMBEDDED {
 public:
  FrameSummary(Object* receiver, JSFunction* function,
               AbstractCode* abstract_code, int code_offset,
               bool is_constructor);

  Handle<Object> receiver() { return receiver_; }
  Handle<JSFunction> function() { return function_; }
  Handle<AbstractCode> abstract_code() { return abstract_code_; }
  int code_offset() { return code_offset_; }
  bool is_constructor() { return is_constructor_; }

  void Print();

 private:
  Handle<Object> receiver_;
  Handle<JSFunction> function_;
  Handle<AbstractCode> abstract_code_;
  int code_offset_;
  bool is_constructor_;
};


class JavaScriptFrame: public StandardFrame {
 public:
  Type type() const override { return JAVA_SCRIPT; }

  // Accessors.
  inline JSFunction* function() const;
  inline Object* receiver() const;
  inline void set_receiver(Object* value);

  // Access the parameters.
  inline Address GetParameterSlot(int index) const;
  inline Object* GetParameter(int index) const;
  inline int ComputeParametersCount() const {
    return GetNumberOfIncomingArguments();
  }

  // Access the operand stack.
  inline Address GetOperandSlot(int index) const;
  inline Object* GetOperand(int index) const;
  inline int ComputeOperandsCount() const;

  // Generator support to preserve operand stack.
  void SaveOperandStack(FixedArray* store) const;
  void RestoreOperandStack(FixedArray* store);

  // Debugger access.
  void SetParameterValue(int index, Object* value) const;

  // Check if this frame is a constructor frame invoked through 'new'.
  bool IsConstructor() const;

  // Determines whether this frame includes inlined activations. To get details
  // about the inlined frames use {GetFunctions} and {Summarize}.
  bool HasInlinedFrames() const;

  // Check if this frame has "adapted" arguments in the sense that the
  // actual passed arguments are available in an arguments adaptor
  // frame below it on the stack.
  inline bool has_adapted_arguments() const;
  int GetArgumentsLength() const;

  // Garbage collection support.
  void Iterate(ObjectVisitor* v) const override;

  // Printing support.
  void Print(StringStream* accumulator, PrintMode mode,
             int index) const override;

  // Determine the code for the frame.
  Code* unchecked_code() const override;

  // Return a list with JSFunctions of this frame.
  virtual void GetFunctions(List<JSFunction*>* functions) const;

  // Build a list with summaries for this frame including all inlined frames.
  virtual void Summarize(List<FrameSummary>* frames);

  // Lookup exception handler for current {pc}, returns -1 if none found. Also
  // returns the expected number of stack slots at the handler site.
  virtual int LookupExceptionHandlerInTable(
      int* stack_slots, HandlerTable::CatchPrediction* prediction);

  // Architecture-specific register description.
  static Register fp_register();
  static Register context_register();
  static Register constant_pool_pointer_register();

  static JavaScriptFrame* cast(StackFrame* frame) {
    DCHECK(frame->is_java_script());
    return static_cast<JavaScriptFrame*>(frame);
  }

  static void PrintFunctionAndOffset(JSFunction* function, Code* code,
                                     Address pc, FILE* file,
                                     bool print_line_number);

  static void PrintTop(Isolate* isolate, FILE* file, bool print_args,
                       bool print_line_number);

 protected:
  inline explicit JavaScriptFrame(StackFrameIteratorBase* iterator);

  Address GetCallerStackPointer() const override;

  virtual int GetNumberOfIncomingArguments() const;

  // Garbage collection support. Iterates over incoming arguments,
  // receiver, and any callee-saved registers.
  void IterateArguments(ObjectVisitor* v) const;

 private:
  inline Object* function_slot_object() const;

  friend class StackFrameIteratorBase;
};


class StubFrame : public StandardFrame {
 public:
  Type type() const override { return STUB; }

  // GC support.
  void Iterate(ObjectVisitor* v) const override;

  // Determine the code for the frame.
  Code* unchecked_code() const override;

 protected:
  inline explicit StubFrame(StackFrameIteratorBase* iterator);

  Address GetCallerStackPointer() const override;

  virtual int GetNumberOfIncomingArguments() const;

  friend class StackFrameIteratorBase;
};


class OptimizedFrame : public JavaScriptFrame {
 public:
  Type type() const override { return OPTIMIZED; }

  // GC support.
  void Iterate(ObjectVisitor* v) const override;

  // Return a list with JSFunctions of this frame.
  // The functions are ordered bottom-to-top (i.e. functions.last()
  // is the top-most activation)
  void GetFunctions(List<JSFunction*>* functions) const override;

  void Summarize(List<FrameSummary>* frames) override;

  // Lookup exception handler for current {pc}, returns -1 if none found. Also
  // returns the expected number of stack slots at the handler site.
  int LookupExceptionHandlerInTable(
      int* stack_slots, HandlerTable::CatchPrediction* prediction) override;

  DeoptimizationInputData* GetDeoptimizationData(int* deopt_index) const;

  static int StackSlotOffsetRelativeToFp(int slot_index);

 protected:
  inline explicit OptimizedFrame(StackFrameIteratorBase* iterator);

 private:
  friend class StackFrameIteratorBase;

  Object* StackSlotAt(int index) const;
};


class InterpretedFrame : public JavaScriptFrame {
 public:
  Type type() const override { return INTERPRETED; }

  // Lookup exception handler for current {pc}, returns -1 if none found. Also
  // returns the expected number of stack slots at the handler site.
  int LookupExceptionHandlerInTable(
      int* stack_slots, HandlerTable::CatchPrediction* prediction) override;

  // Returns the current offset into the bytecode stream.
  int GetBytecodeOffset() const;

  // Updates the current offset into the bytecode stream, mainly used for stack
  // unwinding to continue execution at a different bytecode offset.
  void PatchBytecodeOffset(int new_offset);

  // Access to the interpreter register file for this frame.
  Object* GetInterpreterRegister(int register_index) const;

  // Build a list with summaries for this frame including all inlined frames.
  void Summarize(List<FrameSummary>* frames) override;

 protected:
  inline explicit InterpretedFrame(StackFrameIteratorBase* iterator);

 private:
  friend class StackFrameIteratorBase;
};


// Arguments adaptor frames are automatically inserted below
// JavaScript frames when the actual number of parameters does not
// match the formal number of parameters.
class ArgumentsAdaptorFrame: public JavaScriptFrame {
 public:
  Type type() const override { return ARGUMENTS_ADAPTOR; }

  // Determine the code for the frame.
  Code* unchecked_code() const override;

  static ArgumentsAdaptorFrame* cast(StackFrame* frame) {
    DCHECK(frame->is_arguments_adaptor());
    return static_cast<ArgumentsAdaptorFrame*>(frame);
  }

  // Printing support.
  void Print(StringStream* accumulator, PrintMode mode,
             int index) const override;

 protected:
  inline explicit ArgumentsAdaptorFrame(StackFrameIteratorBase* iterator);

  int GetNumberOfIncomingArguments() const override;

  Address GetCallerStackPointer() const override;

 private:
  friend class StackFrameIteratorBase;
};


class InternalFrame: public StandardFrame {
 public:
  Type type() const override { return INTERNAL; }

  // Garbage collection support.
  void Iterate(ObjectVisitor* v) const override;

  // Determine the code for the frame.
  Code* unchecked_code() const override;

  static InternalFrame* cast(StackFrame* frame) {
    DCHECK(frame->is_internal());
    return static_cast<InternalFrame*>(frame);
  }

 protected:
  inline explicit InternalFrame(StackFrameIteratorBase* iterator);

  Address GetCallerStackPointer() const override;

 private:
  friend class StackFrameIteratorBase;
};


class StubFailureTrampolineFrame: public StandardFrame {
 public:
  // sizeof(Arguments) - sizeof(Arguments*) is 3 * kPointerSize), but the
  // presubmit script complains about using sizeof() on a type.
  static const int kFirstRegisterParameterFrameOffset =
      StandardFrameConstants::kMarkerOffset - 3 * kPointerSize;

  static const int kCallerStackParameterCountFrameOffset =
      StandardFrameConstants::kMarkerOffset - 2 * kPointerSize;

  Type type() const override { return STUB_FAILURE_TRAMPOLINE; }

  // Get the code associated with this frame.
  // This method could be called during marking phase of GC.
  Code* unchecked_code() const override;

  void Iterate(ObjectVisitor* v) const override;

  // Architecture-specific register description.
  static Register fp_register();
  static Register context_register();
  static Register constant_pool_pointer_register();

 protected:
  inline explicit StubFailureTrampolineFrame(
      StackFrameIteratorBase* iterator);

  Address GetCallerStackPointer() const override;

 private:
  friend class StackFrameIteratorBase;
};


// Construct frames are special trampoline frames introduced to handle
// function invocations through 'new'.
class ConstructFrame: public InternalFrame {
 public:
  Type type() const override { return CONSTRUCT; }

  static ConstructFrame* cast(StackFrame* frame) {
    DCHECK(frame->is_construct());
    return static_cast<ConstructFrame*>(frame);
  }

 protected:
  inline explicit ConstructFrame(StackFrameIteratorBase* iterator);

 private:
  friend class StackFrameIteratorBase;
};


class StackFrameIteratorBase BASE_EMBEDDED {
 public:
  Isolate* isolate() const { return isolate_; }

  bool done() const { return frame_ == NULL; }

 protected:
  // An iterator that iterates over a given thread's stack.
  StackFrameIteratorBase(Isolate* isolate, bool can_access_heap_objects);

  Isolate* isolate_;
#define DECLARE_SINGLETON(ignore, type) type type##_;
  STACK_FRAME_TYPE_LIST(DECLARE_SINGLETON)
#undef DECLARE_SINGLETON
  StackFrame* frame_;
  StackHandler* handler_;
  const bool can_access_heap_objects_;

  StackHandler* handler() const {
    DCHECK(!done());
    return handler_;
  }

  // Get the type-specific frame singleton in a given state.
  StackFrame* SingletonFor(StackFrame::Type type, StackFrame::State* state);
  // A helper function, can return a NULL pointer.
  StackFrame* SingletonFor(StackFrame::Type type);

 private:
  friend class StackFrame;
  DISALLOW_COPY_AND_ASSIGN(StackFrameIteratorBase);
};


class StackFrameIterator: public StackFrameIteratorBase {
 public:
  // An iterator that iterates over the isolate's current thread's stack,
  explicit StackFrameIterator(Isolate* isolate);
  // An iterator that iterates over a given thread's stack.
  StackFrameIterator(Isolate* isolate, ThreadLocalTop* t);

  StackFrame* frame() const {
    DCHECK(!done());
    return frame_;
  }
  void Advance();

 private:
  // Go back to the first frame.
  void Reset(ThreadLocalTop* top);

  DISALLOW_COPY_AND_ASSIGN(StackFrameIterator);
};


// Iterator that supports iterating through all JavaScript frames.
class JavaScriptFrameIterator BASE_EMBEDDED {
 public:
  inline explicit JavaScriptFrameIterator(Isolate* isolate);
  inline JavaScriptFrameIterator(Isolate* isolate, ThreadLocalTop* top);
  // Skip frames until the frame with the given id is reached.
  JavaScriptFrameIterator(Isolate* isolate, StackFrame::Id id);

  inline JavaScriptFrame* frame() const;

  bool done() const { return iterator_.done(); }
  void Advance();

  // Advance to the frame holding the arguments for the current
  // frame. This only affects the current frame if it has adapted
  // arguments.
  void AdvanceToArgumentsFrame();

 private:
  StackFrameIterator iterator_;
};


// NOTE: The stack trace frame iterator is an iterator that only
// traverse proper JavaScript frames; that is JavaScript frames that
// have proper JavaScript functions. This excludes the problematic
// functions in runtime.js.
class StackTraceFrameIterator: public JavaScriptFrameIterator {
 public:
  explicit StackTraceFrameIterator(Isolate* isolate);
  void Advance();

 private:
  bool IsValidFrame();
};


class SafeStackFrameIterator: public StackFrameIteratorBase {
 public:
  SafeStackFrameIterator(Isolate* isolate,
                         Address fp, Address sp,
                         Address js_entry_sp);

  inline StackFrame* frame() const;
  void Advance();

  StackFrame::Type top_frame_type() const { return top_frame_type_; }

 private:
  void AdvanceOneFrame();

  bool IsValidStackAddress(Address addr) const {
    return low_bound_ <= addr && addr <= high_bound_;
  }
  bool IsValidFrame(StackFrame* frame) const;
  bool IsValidCaller(StackFrame* frame);
  bool IsValidExitFrame(Address fp) const;
  bool IsValidTop(ThreadLocalTop* top) const;

  const Address low_bound_;
  const Address high_bound_;
  StackFrame::Type top_frame_type_;
  ExternalCallbackScope* external_callback_scope_;
};


class StackFrameLocator BASE_EMBEDDED {
 public:
  explicit StackFrameLocator(Isolate* isolate) : iterator_(isolate) {}

  // Find the nth JavaScript frame on the stack. The caller must
  // guarantee that such a frame exists.
  JavaScriptFrame* FindJavaScriptFrame(int n);

 private:
  StackFrameIterator iterator_;
};


// Reads all frames on the current stack and copies them into the current
// zone memory.
Vector<StackFrame*> CreateStackMap(Isolate* isolate, Zone* zone);

}  // namespace internal
}  // namespace v8

#endif  // V8_FRAMES_H_
