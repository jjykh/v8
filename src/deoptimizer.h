// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_DEOPTIMIZER_H_
#define V8_DEOPTIMIZER_H_

#include "src/allocation.h"
#include "src/macro-assembler.h"


namespace v8 {
namespace internal {

class FrameDescription;
class TranslationIterator;
class DeoptimizedFrameInfo;
class TranslatedState;
class RegisterValues;

class TranslatedValue {
 public:
  // Allocation-less getter of the value.
  // Returns heap()->arguments_marker() if allocation would be
  // necessary to get the value.
  Object* GetRawValue() const;
  Handle<Object> GetValue();

  bool IsMaterializedObject() const;

 private:
  friend class TranslatedState;
  friend class TranslatedFrame;

  enum Kind {
    kInvalid,
    kTagged,
    kInt32,
    kUInt32,
    kBoolBit,
    kDouble,
    kCapturedObject,    // Object captured by the escape analysis.
                        // The number of nested objects can be obtained
                        // with the DeferredObjectLength() method
                        // (the values of the nested objects follow
                        // this value in the depth-first order.)
    kDuplicatedObject,  // Duplicated object of a deferred object.
    kArgumentsObject    // Arguments object - only used to keep indexing
                        // in sync, it should not be materialized.
  };

  TranslatedValue(TranslatedState* container, Kind kind)
      : kind_(kind), container_(container) {}
  Kind kind() const { return kind_; }
  void Handlify();
  int GetChildrenCount() const;

  static TranslatedValue NewArgumentsObject(TranslatedState* container,
                                            int length, int object_index);
  static TranslatedValue NewDeferredObject(TranslatedState* container,
                                           int length, int object_index);
  static TranslatedValue NewDuplicateObject(TranslatedState* container, int id);
  static TranslatedValue NewDouble(TranslatedState* container, double value);
  static TranslatedValue NewInt32(TranslatedState* container, int32_t value);
  static TranslatedValue NewUInt32(TranslatedState* container, uint32_t value);
  static TranslatedValue NewBool(TranslatedState* container, uint32_t value);
  static TranslatedValue NewTagged(TranslatedState* container, Object* literal);
  static TranslatedValue NewInvalid(TranslatedState* container);

  Isolate* isolate() const;
  void MaterializeSimple();

  Kind kind_;
  TranslatedState* container_;  // This is only needed for materialization of
                                // objects and constructing handles (to get
                                // to the isolate).

  MaybeHandle<Object> value_;  // Before handlification, this is always null,
                               // after materialization it is never null,
                               // in between it is only null if the value needs
                               // to be materialized.

  struct MaterializedObjectInfo {
    int id_;
    int length_;  // Applies only to kArgumentsObject or kCapturedObject kinds.
  };

  union {
    // kind kTagged. After handlification it is always nullptr.
    Object* raw_literal_;
    // kind is kUInt32 or kBoolBit.
    uint32_t uint32_value_;
    // kind is kInt32.
    int32_t int32_value_;
    // kind is kDouble
    double double_value_;
    // kind is kDuplicatedObject or kArgumentsObject or kCapturedObject.
    MaterializedObjectInfo materialization_info_;
  };

  // Checked accessors for the union members.
  Object* raw_literal() const;
  int32_t int32_value() const;
  uint32_t uint32_value() const;
  double double_value() const;
  int object_length() const;
  int object_index() const;
};


class TranslatedFrame {
 public:
  enum Kind {
    kFunction,
    kInterpretedFunction,
    kGetter,
    kSetter,
    kArgumentsAdaptor,
    kConstructStub,
    kCompiledStub,
    kInvalid
  };

  int GetValueCount();

  Kind kind() const { return kind_; }
  BailoutId node_id() const { return node_id_; }
  Handle<SharedFunctionInfo> shared_info() const { return shared_info_; }
  int height() const { return height_; }

  class iterator {
   public:
    iterator& operator++() {
      AdvanceIterator(&position_);
      return *this;
    }

    iterator operator++(int) {
      iterator original(position_);
      AdvanceIterator(&position_);
      return original;
    }

    bool operator==(const iterator& other) const {
      return position_ == other.position_;
    }
    bool operator!=(const iterator& other) const { return !(*this == other); }

    TranslatedValue& operator*() { return (*position_); }
    TranslatedValue* operator->() { return &(*position_); }

   private:
    friend TranslatedFrame;

    explicit iterator(std::deque<TranslatedValue>::iterator position)
        : position_(position) {}

    std::deque<TranslatedValue>::iterator position_;
  };

  typedef TranslatedValue& reference;
  typedef TranslatedValue const& const_reference;

  iterator begin() { return iterator(values_.begin()); }
  iterator end() { return iterator(values_.end()); }

  reference front() { return values_.front(); }
  const_reference front() const { return values_.front(); }

 private:
  friend class TranslatedState;

  // Constructor static methods.
  static TranslatedFrame JSFrame(BailoutId node_id,
                                 SharedFunctionInfo* shared_info, int height);
  static TranslatedFrame InterpretedFrame(BailoutId bytecode_offset,
                                          SharedFunctionInfo* shared_info,
                                          int height);
  static TranslatedFrame AccessorFrame(Kind kind,
                                       SharedFunctionInfo* shared_info);
  static TranslatedFrame ArgumentsAdaptorFrame(SharedFunctionInfo* shared_info,
                                               int height);
  static TranslatedFrame ConstructStubFrame(SharedFunctionInfo* shared_info,
                                            int height);
  static TranslatedFrame CompiledStubFrame(int height, Isolate* isolate) {
    return TranslatedFrame(kCompiledStub, isolate, nullptr, height);
  }
  static TranslatedFrame InvalidFrame() {
    return TranslatedFrame(kInvalid, nullptr);
  }

  static void AdvanceIterator(std::deque<TranslatedValue>::iterator* iter);

  TranslatedFrame(Kind kind, Isolate* isolate,
                  SharedFunctionInfo* shared_info = nullptr, int height = 0)
      : kind_(kind),
        node_id_(BailoutId::None()),
        raw_shared_info_(shared_info),
        height_(height),
        isolate_(isolate) {}


  void Add(const TranslatedValue& value) { values_.push_back(value); }
  void Handlify();

  Kind kind_;
  BailoutId node_id_;
  SharedFunctionInfo* raw_shared_info_;
  Handle<SharedFunctionInfo> shared_info_;
  int height_;
  Isolate* isolate_;

  typedef std::deque<TranslatedValue> ValuesContainer;

  ValuesContainer values_;
};


// Auxiliary class for translating deoptimization values.
// Typical usage sequence:
//
// 1. Construct the instance. This will involve reading out the translations
//    and resolving them to values using the supplied frame pointer and
//    machine state (registers). This phase is guaranteed not to allocate
//    and not to use any HandleScope. Any object pointers will be stored raw.
//
// 2. Handlify pointers. This will convert all the raw pointers to handles.
//
// 3. Reading out the frame values.
//
// Note: After the instance is constructed, it is possible to iterate over
// the values eagerly.

class TranslatedState {
 public:
  TranslatedState();
  explicit TranslatedState(JavaScriptFrame* frame);

  void Prepare(bool has_adapted_arguments, Address stack_frame_pointer);

  // Store newly materialized values into the isolate.
  void StoreMaterializedValuesAndDeopt();

  typedef std::vector<TranslatedFrame>::iterator iterator;
  iterator begin() { return frames_.begin(); }
  iterator end() { return frames_.end(); }

  typedef std::vector<TranslatedFrame>::const_iterator const_iterator;
  const_iterator begin() const { return frames_.begin(); }
  const_iterator end() const { return frames_.end(); }

  std::vector<TranslatedFrame>& frames() { return frames_; }

  TranslatedFrame* GetArgumentsInfoFromJSFrameIndex(int jsframe_index,
                                                    int* arguments_count);

  Isolate* isolate() { return isolate_; }

  void Init(Address input_frame_pointer, TranslationIterator* iterator,
            FixedArray* literal_array, RegisterValues* registers,
            FILE* trace_file);

 private:
  friend TranslatedValue;

  TranslatedFrame CreateNextTranslatedFrame(TranslationIterator* iterator,
                                            FixedArray* literal_array,
                                            Address fp,
                                            FILE* trace_file);
  TranslatedValue CreateNextTranslatedValue(int frame_index, int value_index,
                                            TranslationIterator* iterator,
                                            FixedArray* literal_array,
                                            Address fp,
                                            RegisterValues* registers,
                                            FILE* trace_file);

  void UpdateFromPreviouslyMaterializedObjects();
  Handle<Object> MaterializeAt(int frame_index, int* value_index);
  Handle<Object> MaterializeObjectAt(int object_index);
  bool GetAdaptedArguments(Handle<JSObject>* result, int frame_index);

  static uint32_t GetUInt32Slot(Address fp, int slot_index);

  std::vector<TranslatedFrame> frames_;
  Isolate* isolate_;
  Address stack_frame_pointer_;
  bool has_adapted_arguments_;

  struct ObjectPosition {
    int frame_index_;
    int value_index_;
  };
  std::deque<ObjectPosition> object_positions_;
};


class OptimizedFunctionVisitor BASE_EMBEDDED {
 public:
  virtual ~OptimizedFunctionVisitor() {}

  // Function which is called before iteration of any optimized functions
  // from given native context.
  virtual void EnterContext(Context* context) = 0;

  virtual void VisitFunction(JSFunction* function) = 0;

  // Function which is called after iteration of all optimized functions
  // from given native context.
  virtual void LeaveContext(Context* context) = 0;
};


#define DEOPT_MESSAGES_LIST(V)                                                 \
  V(kAccessCheck, "Access check needed")                                       \
  V(kNoReason, "no reason")                                                    \
  V(kConstantGlobalVariableAssignment, "Constant global variable assignment")  \
  V(kConversionOverflow, "conversion overflow")                                \
  V(kDivisionByZero, "division by zero")                                       \
  V(kElementsKindUnhandledInKeyedLoadGenericStub,                              \
    "ElementsKind unhandled in KeyedLoadGenericStub")                          \
  V(kExpectedHeapNumber, "Expected heap number")                               \
  V(kExpectedSmi, "Expected smi")                                              \
  V(kForcedDeoptToRuntime, "Forced deopt to runtime")                          \
  V(kHole, "hole")                                                             \
  V(kHoleyArrayDespitePackedElements_kindFeedback,                             \
    "Holey array despite packed elements_kind feedback")                       \
  V(kInstanceMigrationFailed, "instance migration failed")                     \
  V(kInsufficientTypeFeedbackForCallWithArguments,                             \
    "Insufficient type feedback for call with arguments")                      \
  V(kInsufficientTypeFeedbackForCombinedTypeOfBinaryOperation,                 \
    "Insufficient type feedback for combined type of binary operation")        \
  V(kInsufficientTypeFeedbackForGenericNamedAccess,                            \
    "Insufficient type feedback for generic named access")                     \
  V(kInsufficientTypeFeedbackForKeyedLoad,                                     \
    "Insufficient type feedback for keyed load")                               \
  V(kInsufficientTypeFeedbackForKeyedStore,                                    \
    "Insufficient type feedback for keyed store")                              \
  V(kInsufficientTypeFeedbackForLHSOfBinaryOperation,                          \
    "Insufficient type feedback for LHS of binary operation")                  \
  V(kInsufficientTypeFeedbackForRHSOfBinaryOperation,                          \
    "Insufficient type feedback for RHS of binary operation")                  \
  V(kKeyIsNegative, "key is negative")                                         \
  V(kLostPrecision, "lost precision")                                          \
  V(kLostPrecisionOrNaN, "lost precision or NaN")                              \
  V(kMementoFound, "memento found")                                            \
  V(kMinusZero, "minus zero")                                                  \
  V(kNaN, "NaN")                                                               \
  V(kNegativeKeyEncountered, "Negative key encountered")                       \
  V(kNegativeValue, "negative value")                                          \
  V(kNoCache, "no cache")                                                      \
  V(kNonStrictElementsInKeyedLoadGenericStub,                                  \
    "non-strict elements in KeyedLoadGenericStub")                             \
  V(kNotADateObject, "not a date object")                                      \
  V(kNotAHeapNumber, "not a heap number")                                      \
  V(kNotAHeapNumberUndefinedBoolean, "not a heap number/undefined/true/false") \
  V(kNotAHeapNumberUndefined, "not a heap number/undefined")                   \
  V(kNotAJavaScriptObject, "not a JavaScript object")                          \
  V(kNotASmi, "not a Smi")                                                     \
  V(kNull, "null")                                                             \
  V(kOutOfBounds, "out of bounds")                                             \
  V(kOutsideOfRange, "Outside of range")                                       \
  V(kOverflow, "overflow")                                                     \
  V(kProxy, "proxy")                                                           \
  V(kReceiverWasAGlobalObject, "receiver was a global object")                 \
  V(kSmi, "Smi")                                                               \
  V(kTooManyArguments, "too many arguments")                                   \
  V(kTooManyUndetectableTypes, "Too many undetectable types")                  \
  V(kTracingElementsTransitions, "Tracing elements transitions")               \
  V(kTypeMismatchBetweenFeedbackAndConstant,                                   \
    "Type mismatch between feedback and constant")                             \
  V(kUndefined, "undefined")                                                   \
  V(kUnexpectedCellContentsInConstantGlobalStore,                              \
    "Unexpected cell contents in constant global store")                       \
  V(kUnexpectedCellContentsInGlobalStore,                                      \
    "Unexpected cell contents in global store")                                \
  V(kUnexpectedObject, "unexpected object")                                    \
  V(kUnexpectedRHSOfBinaryOperation, "Unexpected RHS of binary operation")     \
  V(kUninitializedBoilerplateInFastClone,                                      \
    "Uninitialized boilerplate in fast clone")                                 \
  V(kUninitializedBoilerplateLiterals, "Uninitialized boilerplate literals")   \
  V(kUnknownMapInPolymorphicAccess, "Unknown map in polymorphic access")       \
  V(kUnknownMapInPolymorphicCall, "Unknown map in polymorphic call")           \
  V(kUnknownMapInPolymorphicElementAccess,                                     \
    "Unknown map in polymorphic element access")                               \
  V(kUnknownMap, "Unknown map")                                                \
  V(kValueMismatch, "value mismatch")                                          \
  V(kWrongInstanceType, "wrong instance type")                                 \
  V(kWrongMap, "wrong map")                                                    \
  V(kUndefinedOrNullInForIn, "null or undefined in for-in")                    \
  V(kUndefinedOrNullInToObject, "null or undefined in ToObject")


class Deoptimizer : public Malloced {
 public:
  enum BailoutType {
    EAGER,
    LAZY,
    SOFT,
    // This last bailout type is not really a bailout, but used by the
    // debugger to deoptimize stack frames to allow inspection.
    DEBUGGER,
    kBailoutTypesWithCodeEntry = SOFT + 1
  };

#define DEOPT_MESSAGES_CONSTANTS(C, T) C,
  enum DeoptReason {
    DEOPT_MESSAGES_LIST(DEOPT_MESSAGES_CONSTANTS) kLastDeoptReason
  };
#undef DEOPT_MESSAGES_CONSTANTS
  static const char* GetDeoptReason(DeoptReason deopt_reason);

  struct DeoptInfo {
    DeoptInfo(SourcePosition position, const char* m, DeoptReason d)
        : position(position), mnemonic(m), deopt_reason(d), inlining_id(0) {}

    SourcePosition position;
    const char* mnemonic;
    DeoptReason deopt_reason;
    int inlining_id;
  };

  static DeoptInfo GetDeoptInfo(Code* code, byte* from);

  struct JumpTableEntry : public ZoneObject {
    inline JumpTableEntry(Address entry, const DeoptInfo& deopt_info,
                          Deoptimizer::BailoutType type, bool frame)
        : label(),
          address(entry),
          deopt_info(deopt_info),
          bailout_type(type),
          needs_frame(frame) {}

    bool IsEquivalentTo(const JumpTableEntry& other) const {
      return address == other.address && bailout_type == other.bailout_type &&
             needs_frame == other.needs_frame;
    }

    Label label;
    Address address;
    DeoptInfo deopt_info;
    Deoptimizer::BailoutType bailout_type;
    bool needs_frame;
  };

  static bool TraceEnabledFor(BailoutType deopt_type,
                              StackFrame::Type frame_type);
  static const char* MessageFor(BailoutType type);

  int output_count() const { return output_count_; }

  Handle<JSFunction> function() const { return Handle<JSFunction>(function_); }
  Handle<Code> compiled_code() const { return Handle<Code>(compiled_code_); }
  BailoutType bailout_type() const { return bailout_type_; }

  // Number of created JS frames. Not all created frames are necessarily JS.
  int jsframe_count() const { return jsframe_count_; }

  static Deoptimizer* New(JSFunction* function,
                          BailoutType type,
                          unsigned bailout_id,
                          Address from,
                          int fp_to_sp_delta,
                          Isolate* isolate);
  static Deoptimizer* Grab(Isolate* isolate);

  // The returned object with information on the optimized frame needs to be
  // freed before another one can be generated.
  static DeoptimizedFrameInfo* DebuggerInspectableFrame(JavaScriptFrame* frame,
                                                        int jsframe_index,
                                                        Isolate* isolate);
  static void DeleteDebuggerInspectableFrame(DeoptimizedFrameInfo* info,
                                             Isolate* isolate);

  // Makes sure that there is enough room in the relocation
  // information of a code object to perform lazy deoptimization
  // patching. If there is not enough room a new relocation
  // information object is allocated and comments are added until it
  // is big enough.
  static void EnsureRelocSpaceForLazyDeoptimization(Handle<Code> code);

  // Deoptimize the function now. Its current optimized code will never be run
  // again and any activations of the optimized code will get deoptimized when
  // execution returns.
  static void DeoptimizeFunction(JSFunction* function);

  // Deoptimize all code in the given isolate.
  static void DeoptimizeAll(Isolate* isolate);

  // Deoptimizes all optimized code that has been previously marked
  // (via code->set_marked_for_deoptimization) and unlinks all functions that
  // refer to that code.
  static void DeoptimizeMarkedCode(Isolate* isolate);

  // Visit all the known optimized functions in a given isolate.
  static void VisitAllOptimizedFunctions(
      Isolate* isolate, OptimizedFunctionVisitor* visitor);

  // The size in bytes of the code required at a lazy deopt patch site.
  static int patch_size();

  ~Deoptimizer();

  void MaterializeHeapObjects(JavaScriptFrameIterator* it);

  void MaterializeHeapNumbersForDebuggerInspectableFrame(
      int frame_index, int parameter_count, int expression_count,
      DeoptimizedFrameInfo* info);

  static void ComputeOutputFrames(Deoptimizer* deoptimizer);


  enum GetEntryMode {
    CALCULATE_ENTRY_ADDRESS,
    ENSURE_ENTRY_CODE
  };


  static Address GetDeoptimizationEntry(
      Isolate* isolate,
      int id,
      BailoutType type,
      GetEntryMode mode = ENSURE_ENTRY_CODE);
  static int GetDeoptimizationId(Isolate* isolate,
                                 Address addr,
                                 BailoutType type);
  static int GetOutputInfo(DeoptimizationOutputData* data,
                           BailoutId node_id,
                           SharedFunctionInfo* shared);

  // Code generation support.
  static int input_offset() { return OFFSET_OF(Deoptimizer, input_); }
  static int output_count_offset() {
    return OFFSET_OF(Deoptimizer, output_count_);
  }
  static int output_offset() { return OFFSET_OF(Deoptimizer, output_); }

  static int has_alignment_padding_offset() {
    return OFFSET_OF(Deoptimizer, has_alignment_padding_);
  }

  static int GetDeoptimizedCodeCount(Isolate* isolate);

  static const int kNotDeoptimizationEntry = -1;

  // Generators for the deoptimization entry code.
  class TableEntryGenerator BASE_EMBEDDED {
   public:
    TableEntryGenerator(MacroAssembler* masm, BailoutType type, int count)
        : masm_(masm), type_(type), count_(count) {}

    void Generate();

   protected:
    MacroAssembler* masm() const { return masm_; }
    BailoutType type() const { return type_; }
    Isolate* isolate() const { return masm_->isolate(); }

    void GeneratePrologue();

   private:
    int count() const { return count_; }

    MacroAssembler* masm_;
    Deoptimizer::BailoutType type_;
    int count_;
  };

  int ConvertJSFrameIndexToFrameIndex(int jsframe_index);

  static size_t GetMaxDeoptTableSize();

  static void EnsureCodeForDeoptimizationEntry(Isolate* isolate,
                                               BailoutType type,
                                               int max_entry_id);

  Isolate* isolate() const { return isolate_; }

 private:
  static const int kMinNumberOfEntries = 64;
  static const int kMaxNumberOfEntries = 16384;

  Deoptimizer(Isolate* isolate,
              JSFunction* function,
              BailoutType type,
              unsigned bailout_id,
              Address from,
              int fp_to_sp_delta,
              Code* optimized_code);
  Code* FindOptimizedCode(JSFunction* function, Code* optimized_code);
  void PrintFunctionName();
  void DeleteFrameDescriptions();

  void DoComputeOutputFrames();
  void DoComputeJSFrame(int frame_index);
  void DoComputeInterpretedFrame(int frame_index);
  void DoComputeArgumentsAdaptorFrame(int frame_index);
  void DoComputeConstructStubFrame(int frame_index);
  void DoComputeAccessorStubFrame(int frame_index, bool is_setter_stub_frame);
  void DoComputeCompiledStubFrame(int frame_index);

  void WriteTranslatedValueToOutput(
      TranslatedFrame::iterator* iterator, int* input_index, int frame_index,
      unsigned output_offset, const char* debug_hint_string = nullptr,
      Address output_address_for_materialization = nullptr);
  void WriteValueToOutput(Object* value, int input_index, int frame_index,
                          unsigned output_offset,
                          const char* debug_hint_string);
  void DebugPrintOutputSlot(intptr_t value, int frame_index,
                            unsigned output_offset,
                            const char* debug_hint_string);

  unsigned ComputeInputFrameSize() const;
  unsigned ComputeJavascriptFixedSize(JSFunction* function) const;
  unsigned ComputeInterpretedFixedSize(JSFunction* function) const;

  unsigned ComputeIncomingArgumentSize(JSFunction* function) const;
  static unsigned ComputeOutgoingArgumentSize(Code* code, unsigned bailout_id);

  Object* ComputeLiteral(int index) const;

  static void GenerateDeoptimizationEntries(
      MacroAssembler* masm, int count, BailoutType type);

  // Marks all the code in the given context for deoptimization.
  static void MarkAllCodeForContext(Context* native_context);

  // Visit all the known optimized functions in a given context.
  static void VisitAllOptimizedFunctionsForContext(
      Context* context, OptimizedFunctionVisitor* visitor);

  // Deoptimizes all code marked in the given context.
  static void DeoptimizeMarkedCodeForContext(Context* native_context);

  // Patch the given code so that it will deoptimize itself.
  static void PatchCodeForDeoptimization(Isolate* isolate, Code* code);

  // Searches the list of known deoptimizing code for a Code object
  // containing the given address (which is supposedly faster than
  // searching all code objects).
  Code* FindDeoptimizingCode(Address addr);

  // Fill the input from from a JavaScript frame. This is used when
  // the debugger needs to inspect an optimized frame. For normal
  // deoptimizations the input frame is filled in generated code.
  void FillInputFrame(Address tos, JavaScriptFrame* frame);

  // Fill the given output frame's registers to contain the failure handler
  // address and the number of parameters for a stub failure trampoline.
  void SetPlatformCompiledStubRegisters(FrameDescription* output_frame,
                                        CodeStubDescriptor* desc);

  // Fill the given output frame's double registers with the original values
  // from the input frame's double registers.
  void CopyDoubleRegisters(FrameDescription* output_frame);

  // Determines whether the input frame contains alignment padding by looking
  // at the dynamic alignment state slot inside the frame.
  bool HasAlignmentPadding(JSFunction* function);

  Isolate* isolate_;
  JSFunction* function_;
  Code* compiled_code_;
  unsigned bailout_id_;
  BailoutType bailout_type_;
  Address from_;
  int fp_to_sp_delta_;
  int has_alignment_padding_;

  // Input frame description.
  FrameDescription* input_;
  // Number of output frames.
  int output_count_;
  // Number of output js frames.
  int jsframe_count_;
  // Array of output frame descriptions.
  FrameDescription** output_;

  // Key for lookup of previously materialized objects
  Address stack_fp_;

  TranslatedState translated_state_;
  struct ValueToMaterialize {
    Address output_slot_address_;
    TranslatedFrame::iterator value_;
  };
  std::vector<ValueToMaterialize> values_to_materialize_;

#ifdef DEBUG
  DisallowHeapAllocation* disallow_heap_allocation_;
#endif  // DEBUG

  CodeTracer::Scope* trace_scope_;

  static const int table_entry_size_;

  friend class FrameDescription;
  friend class DeoptimizedFrameInfo;
};


class RegisterValues {
 public:
  intptr_t GetRegister(unsigned n) const {
#if DEBUG
    // This convoluted DCHECK is needed to work around a gcc problem that
    // improperly detects an array bounds overflow in optimized debug builds
    // when using a plain DCHECK.
    if (n >= arraysize(registers_)) {
      DCHECK(false);
      return 0;
    }
#endif
    return registers_[n];
  }

  double GetDoubleRegister(unsigned n) const {
    DCHECK(n < arraysize(double_registers_));
    return double_registers_[n];
  }

  void SetRegister(unsigned n, intptr_t value) {
    DCHECK(n < arraysize(registers_));
    registers_[n] = value;
  }

  void SetDoubleRegister(unsigned n, double value) {
    DCHECK(n < arraysize(double_registers_));
    double_registers_[n] = value;
  }

  intptr_t registers_[Register::kNumRegisters];
  double double_registers_[DoubleRegister::kMaxNumRegisters];
};


class FrameDescription {
 public:
  FrameDescription(uint32_t frame_size,
                   JSFunction* function);

  void* operator new(size_t size, uint32_t frame_size) {
    // Subtracts kPointerSize, as the member frame_content_ already supplies
    // the first element of the area to store the frame.
    return malloc(size + frame_size - kPointerSize);
  }

  void operator delete(void* pointer, uint32_t frame_size) {
    free(pointer);
  }

  void operator delete(void* description) {
    free(description);
  }

  uint32_t GetFrameSize() const {
    DCHECK(static_cast<uint32_t>(frame_size_) == frame_size_);
    return static_cast<uint32_t>(frame_size_);
  }

  JSFunction* GetFunction() const { return function_; }

  unsigned GetOffsetFromSlotIndex(int slot_index);

  intptr_t GetFrameSlot(unsigned offset) {
    return *GetFrameSlotPointer(offset);
  }

  Address GetFramePointerAddress() {
    int fp_offset = GetFrameSize() -
                    (ComputeParametersCount() + 1) * kPointerSize -
                    StandardFrameConstants::kCallerSPOffset;
    return reinterpret_cast<Address>(GetFrameSlotPointer(fp_offset));
  }

  RegisterValues* GetRegisterValues() { return &register_values_; }

  void SetFrameSlot(unsigned offset, intptr_t value) {
    *GetFrameSlotPointer(offset) = value;
  }

  void SetCallerPc(unsigned offset, intptr_t value);

  void SetCallerFp(unsigned offset, intptr_t value);

  void SetCallerConstantPool(unsigned offset, intptr_t value);

  intptr_t GetRegister(unsigned n) const {
    return register_values_.GetRegister(n);
  }

  double GetDoubleRegister(unsigned n) const {
    return register_values_.GetDoubleRegister(n);
  }

  void SetRegister(unsigned n, intptr_t value) {
    register_values_.SetRegister(n, value);
  }

  void SetDoubleRegister(unsigned n, double value) {
    register_values_.SetDoubleRegister(n, value);
  }

  intptr_t GetTop() const { return top_; }
  void SetTop(intptr_t top) { top_ = top; }

  intptr_t GetPc() const { return pc_; }
  void SetPc(intptr_t pc) { pc_ = pc; }

  intptr_t GetFp() const { return fp_; }
  void SetFp(intptr_t fp) { fp_ = fp; }

  intptr_t GetContext() const { return context_; }
  void SetContext(intptr_t context) { context_ = context; }

  intptr_t GetConstantPool() const { return constant_pool_; }
  void SetConstantPool(intptr_t constant_pool) {
    constant_pool_ = constant_pool;
  }

  Smi* GetState() const { return state_; }
  void SetState(Smi* state) { state_ = state; }

  void SetContinuation(intptr_t pc) { continuation_ = pc; }

  StackFrame::Type GetFrameType() const { return type_; }
  void SetFrameType(StackFrame::Type type) { type_ = type; }

  // Get the incoming arguments count.
  int ComputeParametersCount();

  // Get a parameter value for an unoptimized frame.
  Object* GetParameter(int index);

  // Get the expression stack height for a unoptimized frame.
  unsigned GetExpressionCount();

  // Get the expression stack value for an unoptimized frame.
  Object* GetExpression(int index);

  static int registers_offset() {
    return OFFSET_OF(FrameDescription, register_values_.registers_);
  }

  static int double_registers_offset() {
    return OFFSET_OF(FrameDescription, register_values_.double_registers_);
  }

  static int frame_size_offset() {
    return offsetof(FrameDescription, frame_size_);
  }

  static int pc_offset() { return offsetof(FrameDescription, pc_); }

  static int state_offset() { return offsetof(FrameDescription, state_); }

  static int continuation_offset() {
    return offsetof(FrameDescription, continuation_);
  }

  static int frame_content_offset() {
    return offsetof(FrameDescription, frame_content_);
  }

 private:
  static const uint32_t kZapUint32 = 0xbeeddead;

  // Frame_size_ must hold a uint32_t value.  It is only a uintptr_t to
  // keep the variable-size array frame_content_ of type intptr_t at
  // the end of the structure aligned.
  uintptr_t frame_size_;  // Number of bytes.
  JSFunction* function_;
  RegisterValues register_values_;
  intptr_t top_;
  intptr_t pc_;
  intptr_t fp_;
  intptr_t context_;
  intptr_t constant_pool_;
  StackFrame::Type type_;
  Smi* state_;

  // Continuation is the PC where the execution continues after
  // deoptimizing.
  intptr_t continuation_;

  // This must be at the end of the object as the object is allocated larger
  // than it's definition indicate to extend this array.
  intptr_t frame_content_[1];

  intptr_t* GetFrameSlotPointer(unsigned offset) {
    DCHECK(offset < frame_size_);
    return reinterpret_cast<intptr_t*>(
        reinterpret_cast<Address>(this) + frame_content_offset() + offset);
  }

  int ComputeFixedSize();
};


class DeoptimizerData {
 public:
  explicit DeoptimizerData(MemoryAllocator* allocator);
  ~DeoptimizerData();

  void Iterate(ObjectVisitor* v);

 private:
  MemoryAllocator* allocator_;
  int deopt_entry_code_entries_[Deoptimizer::kBailoutTypesWithCodeEntry];
  MemoryChunk* deopt_entry_code_[Deoptimizer::kBailoutTypesWithCodeEntry];

  DeoptimizedFrameInfo* deoptimized_frame_info_;

  Deoptimizer* current_;

  friend class Deoptimizer;

  DISALLOW_COPY_AND_ASSIGN(DeoptimizerData);
};


class TranslationBuffer BASE_EMBEDDED {
 public:
  explicit TranslationBuffer(Zone* zone) : contents_(256, zone) { }

  int CurrentIndex() const { return contents_.length(); }
  void Add(int32_t value, Zone* zone);

  Handle<ByteArray> CreateByteArray(Factory* factory);

 private:
  ZoneList<uint8_t> contents_;
};


class TranslationIterator BASE_EMBEDDED {
 public:
  TranslationIterator(ByteArray* buffer, int index)
      : buffer_(buffer), index_(index) {
    DCHECK(index >= 0 && index < buffer->length());
  }

  int32_t Next();

  bool HasNext() const { return index_ < buffer_->length(); }

  void Skip(int n) {
    for (int i = 0; i < n; i++) Next();
  }

 private:
  ByteArray* buffer_;
  int index_;
};


#define TRANSLATION_OPCODE_LIST(V) \
  V(BEGIN)                         \
  V(JS_FRAME)                      \
  V(INTERPRETED_FRAME)             \
  V(CONSTRUCT_STUB_FRAME)          \
  V(GETTER_STUB_FRAME)             \
  V(SETTER_STUB_FRAME)             \
  V(ARGUMENTS_ADAPTOR_FRAME)       \
  V(COMPILED_STUB_FRAME)           \
  V(DUPLICATED_OBJECT)             \
  V(ARGUMENTS_OBJECT)              \
  V(CAPTURED_OBJECT)               \
  V(REGISTER)                      \
  V(INT32_REGISTER)                \
  V(UINT32_REGISTER)               \
  V(BOOL_REGISTER)                 \
  V(DOUBLE_REGISTER)               \
  V(STACK_SLOT)                    \
  V(INT32_STACK_SLOT)              \
  V(UINT32_STACK_SLOT)             \
  V(BOOL_STACK_SLOT)               \
  V(DOUBLE_STACK_SLOT)             \
  V(LITERAL)                       \
  V(JS_FRAME_FUNCTION)


class Translation BASE_EMBEDDED {
 public:
#define DECLARE_TRANSLATION_OPCODE_ENUM(item) item,
  enum Opcode {
    TRANSLATION_OPCODE_LIST(DECLARE_TRANSLATION_OPCODE_ENUM)
    LAST = LITERAL
  };
#undef DECLARE_TRANSLATION_OPCODE_ENUM

  Translation(TranslationBuffer* buffer, int frame_count, int jsframe_count,
              Zone* zone)
      : buffer_(buffer),
        index_(buffer->CurrentIndex()),
        zone_(zone) {
    buffer_->Add(BEGIN, zone);
    buffer_->Add(frame_count, zone);
    buffer_->Add(jsframe_count, zone);
  }

  int index() const { return index_; }

  // Commands.
  void BeginJSFrame(BailoutId node_id, int literal_id, unsigned height);
  void BeginInterpretedFrame(BailoutId bytecode_offset, int literal_id,
                             unsigned height);
  void BeginCompiledStubFrame(int height);
  void BeginArgumentsAdaptorFrame(int literal_id, unsigned height);
  void BeginConstructStubFrame(int literal_id, unsigned height);
  void BeginGetterStubFrame(int literal_id);
  void BeginSetterStubFrame(int literal_id);
  void BeginArgumentsObject(int args_length);
  void BeginCapturedObject(int length);
  void DuplicateObject(int object_index);
  void StoreRegister(Register reg);
  void StoreInt32Register(Register reg);
  void StoreUint32Register(Register reg);
  void StoreBoolRegister(Register reg);
  void StoreDoubleRegister(DoubleRegister reg);
  void StoreStackSlot(int index);
  void StoreInt32StackSlot(int index);
  void StoreUint32StackSlot(int index);
  void StoreBoolStackSlot(int index);
  void StoreDoubleStackSlot(int index);
  void StoreLiteral(int literal_id);
  void StoreArgumentsObject(bool args_known, int args_index, int args_length);
  void StoreJSFrameFunction();

  Zone* zone() const { return zone_; }

  static int NumberOfOperandsFor(Opcode opcode);

#if defined(OBJECT_PRINT) || defined(ENABLE_DISASSEMBLER)
  static const char* StringFor(Opcode opcode);
#endif

 private:
  TranslationBuffer* buffer_;
  int index_;
  Zone* zone_;
};


class MaterializedObjectStore {
 public:
  explicit MaterializedObjectStore(Isolate* isolate) : isolate_(isolate) {
  }

  Handle<FixedArray> Get(Address fp);
  void Set(Address fp, Handle<FixedArray> materialized_objects);
  bool Remove(Address fp);

 private:
  Isolate* isolate() { return isolate_; }
  Handle<FixedArray> GetStackEntries();
  Handle<FixedArray> EnsureStackEntries(int size);

  int StackIdToIndex(Address fp);

  Isolate* isolate_;
  List<Address> frame_fps_;
};


// Class used to represent an unoptimized frame when the debugger
// needs to inspect a frame that is part of an optimized frame. The
// internally used FrameDescription objects are not GC safe so for use
// by the debugger frame information is copied to an object of this type.
// Represents parameters in unadapted form so their number might mismatch
// formal parameter count.
class DeoptimizedFrameInfo : public Malloced {
 public:
  DeoptimizedFrameInfo(Deoptimizer* deoptimizer,
                       int frame_index,
                       bool has_arguments_adaptor,
                       bool has_construct_stub);
  virtual ~DeoptimizedFrameInfo();

  // GC support.
  void Iterate(ObjectVisitor* v);

  // Return the number of incoming arguments.
  int parameters_count() { return parameters_count_; }

  // Return the height of the expression stack.
  int expression_count() { return expression_count_; }

  // Get the frame function.
  JSFunction* GetFunction() {
    return function_;
  }

  // Get the frame context.
  Object* GetContext() { return context_; }

  // Check if this frame is preceded by construct stub frame.  The bottom-most
  // inlined frame might still be called by an uninlined construct stub.
  bool HasConstructStub() {
    return has_construct_stub_;
  }

  // Get an incoming argument.
  Object* GetParameter(int index) {
    DCHECK(0 <= index && index < parameters_count());
    return parameters_[index];
  }

  // Get an expression from the expression stack.
  Object* GetExpression(int index) {
    DCHECK(0 <= index && index < expression_count());
    return expression_stack_[index];
  }

  int GetSourcePosition() {
    return source_position_;
  }

 private:
  // Set an incoming argument.
  void SetParameter(int index, Object* obj) {
    DCHECK(0 <= index && index < parameters_count());
    parameters_[index] = obj;
  }

  // Set an expression on the expression stack.
  void SetExpression(int index, Object* obj) {
    DCHECK(0 <= index && index < expression_count());
    expression_stack_[index] = obj;
  }

  JSFunction* function_;
  Object* context_;
  bool has_construct_stub_;
  int parameters_count_;
  int expression_count_;
  Object** parameters_;
  Object** expression_stack_;
  int source_position_;

  friend class Deoptimizer;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_DEOPTIMIZER_H_
