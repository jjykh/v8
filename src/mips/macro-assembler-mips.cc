
// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <limits.h>  // For LONG_MIN, LONG_MAX.

#if V8_TARGET_ARCH_MIPS

#include "src/base/bits.h"
#include "src/base/division-by-constant.h"
#include "src/bootstrapper.h"
#include "src/codegen.h"
#include "src/debug/debug.h"
#include "src/mips/macro-assembler-mips.h"
#include "src/register-configuration.h"
#include "src/runtime/runtime.h"

namespace v8 {
namespace internal {

MacroAssembler::MacroAssembler(Isolate* arg_isolate, void* buffer, int size,
                               CodeObjectRequired create_code_object)
    : Assembler(arg_isolate, buffer, size),
      generating_stub_(false),
      has_frame_(false),
      has_double_zero_reg_set_(false) {
  if (create_code_object == CodeObjectRequired::kYes) {
    code_object_ =
        Handle<Object>::New(isolate()->heap()->undefined_value(), isolate());
  }
}


void MacroAssembler::Load(Register dst,
                          const MemOperand& src,
                          Representation r) {
  DCHECK(!r.IsDouble());
  if (r.IsInteger8()) {
    lb(dst, src);
  } else if (r.IsUInteger8()) {
    lbu(dst, src);
  } else if (r.IsInteger16()) {
    lh(dst, src);
  } else if (r.IsUInteger16()) {
    lhu(dst, src);
  } else {
    lw(dst, src);
  }
}


void MacroAssembler::Store(Register src,
                           const MemOperand& dst,
                           Representation r) {
  DCHECK(!r.IsDouble());
  if (r.IsInteger8() || r.IsUInteger8()) {
    sb(src, dst);
  } else if (r.IsInteger16() || r.IsUInteger16()) {
    sh(src, dst);
  } else {
    if (r.IsHeapObject()) {
      AssertNotSmi(src);
    } else if (r.IsSmi()) {
      AssertSmi(src);
    }
    sw(src, dst);
  }
}


void MacroAssembler::LoadRoot(Register destination,
                              Heap::RootListIndex index) {
  lw(destination, MemOperand(s6, index << kPointerSizeLog2));
}


void MacroAssembler::LoadRoot(Register destination,
                              Heap::RootListIndex index,
                              Condition cond,
                              Register src1, const Operand& src2) {
  Branch(2, NegateCondition(cond), src1, src2);
  lw(destination, MemOperand(s6, index << kPointerSizeLog2));
}


void MacroAssembler::StoreRoot(Register source,
                               Heap::RootListIndex index) {
  DCHECK(Heap::RootCanBeWrittenAfterInitialization(index));
  sw(source, MemOperand(s6, index << kPointerSizeLog2));
}


void MacroAssembler::StoreRoot(Register source,
                               Heap::RootListIndex index,
                               Condition cond,
                               Register src1, const Operand& src2) {
  DCHECK(Heap::RootCanBeWrittenAfterInitialization(index));
  Branch(2, NegateCondition(cond), src1, src2);
  sw(source, MemOperand(s6, index << kPointerSizeLog2));
}


// Push and pop all registers that can hold pointers.
void MacroAssembler::PushSafepointRegisters() {
  // Safepoints expect a block of kNumSafepointRegisters values on the
  // stack, so adjust the stack for unsaved registers.
  const int num_unsaved = kNumSafepointRegisters - kNumSafepointSavedRegisters;
  DCHECK(num_unsaved >= 0);
  if (num_unsaved > 0) {
    Subu(sp, sp, Operand(num_unsaved * kPointerSize));
  }
  MultiPush(kSafepointSavedRegisters);
}


void MacroAssembler::PopSafepointRegisters() {
  const int num_unsaved = kNumSafepointRegisters - kNumSafepointSavedRegisters;
  MultiPop(kSafepointSavedRegisters);
  if (num_unsaved > 0) {
    Addu(sp, sp, Operand(num_unsaved * kPointerSize));
  }
}


void MacroAssembler::StoreToSafepointRegisterSlot(Register src, Register dst) {
  sw(src, SafepointRegisterSlot(dst));
}


void MacroAssembler::LoadFromSafepointRegisterSlot(Register dst, Register src) {
  lw(dst, SafepointRegisterSlot(src));
}


int MacroAssembler::SafepointRegisterStackIndex(int reg_code) {
  // The registers are pushed starting with the highest encoding,
  // which means that lowest encodings are closest to the stack pointer.
  return kSafepointRegisterStackIndexMap[reg_code];
}


MemOperand MacroAssembler::SafepointRegisterSlot(Register reg) {
  return MemOperand(sp, SafepointRegisterStackIndex(reg.code()) * kPointerSize);
}


MemOperand MacroAssembler::SafepointRegistersAndDoublesSlot(Register reg) {
  UNIMPLEMENTED_MIPS();
  // General purpose registers are pushed last on the stack.
  int doubles_size = DoubleRegister::kMaxNumRegisters * kDoubleSize;
  int register_offset = SafepointRegisterStackIndex(reg.code()) * kPointerSize;
  return MemOperand(sp, doubles_size + register_offset);
}


void MacroAssembler::InNewSpace(Register object,
                                Register scratch,
                                Condition cc,
                                Label* branch) {
  DCHECK(cc == eq || cc == ne);
  And(scratch, object, Operand(ExternalReference::new_space_mask(isolate())));
  Branch(branch, cc, scratch,
         Operand(ExternalReference::new_space_start(isolate())));
}


// Clobbers object, dst, value, and ra, if (ra_status == kRAHasBeenSaved)
// The register 'object' contains a heap object pointer.  The heap object
// tag is shifted away.
void MacroAssembler::RecordWriteField(
    Register object,
    int offset,
    Register value,
    Register dst,
    RAStatus ra_status,
    SaveFPRegsMode save_fp,
    RememberedSetAction remembered_set_action,
    SmiCheck smi_check,
    PointersToHereCheck pointers_to_here_check_for_value) {
  DCHECK(!AreAliased(value, dst, t8, object));
  // First, check if a write barrier is even needed. The tests below
  // catch stores of Smis.
  Label done;

  // Skip barrier if writing a smi.
  if (smi_check == INLINE_SMI_CHECK) {
    JumpIfSmi(value, &done);
  }

  // Although the object register is tagged, the offset is relative to the start
  // of the object, so so offset must be a multiple of kPointerSize.
  DCHECK(IsAligned(offset, kPointerSize));

  Addu(dst, object, Operand(offset - kHeapObjectTag));
  if (emit_debug_code()) {
    Label ok;
    And(t8, dst, Operand((1 << kPointerSizeLog2) - 1));
    Branch(&ok, eq, t8, Operand(zero_reg));
    stop("Unaligned cell in write barrier");
    bind(&ok);
  }

  RecordWrite(object,
              dst,
              value,
              ra_status,
              save_fp,
              remembered_set_action,
              OMIT_SMI_CHECK,
              pointers_to_here_check_for_value);

  bind(&done);

  // Clobber clobbered input registers when running with the debug-code flag
  // turned on to provoke errors.
  if (emit_debug_code()) {
    li(value, Operand(bit_cast<int32_t>(kZapValue + 4)));
    li(dst, Operand(bit_cast<int32_t>(kZapValue + 8)));
  }
}


// Clobbers object, dst, map, and ra, if (ra_status == kRAHasBeenSaved)
void MacroAssembler::RecordWriteForMap(Register object,
                                       Register map,
                                       Register dst,
                                       RAStatus ra_status,
                                       SaveFPRegsMode fp_mode) {
  if (emit_debug_code()) {
    DCHECK(!dst.is(at));
    lw(dst, FieldMemOperand(map, HeapObject::kMapOffset));
    Check(eq,
          kWrongAddressOrValuePassedToRecordWrite,
          dst,
          Operand(isolate()->factory()->meta_map()));
  }

  if (!FLAG_incremental_marking) {
    return;
  }

  if (emit_debug_code()) {
    lw(at, FieldMemOperand(object, HeapObject::kMapOffset));
    Check(eq,
          kWrongAddressOrValuePassedToRecordWrite,
          map,
          Operand(at));
  }

  Label done;

  // A single check of the map's pages interesting flag suffices, since it is
  // only set during incremental collection, and then it's also guaranteed that
  // the from object's page's interesting flag is also set.  This optimization
  // relies on the fact that maps can never be in new space.
  CheckPageFlag(map,
                map,  // Used as scratch.
                MemoryChunk::kPointersToHereAreInterestingMask,
                eq,
                &done);

  Addu(dst, object, Operand(HeapObject::kMapOffset - kHeapObjectTag));
  if (emit_debug_code()) {
    Label ok;
    And(at, dst, Operand((1 << kPointerSizeLog2) - 1));
    Branch(&ok, eq, at, Operand(zero_reg));
    stop("Unaligned cell in write barrier");
    bind(&ok);
  }

  // Record the actual write.
  if (ra_status == kRAHasNotBeenSaved) {
    push(ra);
  }
  RecordWriteStub stub(isolate(), object, map, dst, OMIT_REMEMBERED_SET,
                       fp_mode);
  CallStub(&stub);
  if (ra_status == kRAHasNotBeenSaved) {
    pop(ra);
  }

  bind(&done);

  // Count number of write barriers in generated code.
  isolate()->counters()->write_barriers_static()->Increment();
  IncrementCounter(isolate()->counters()->write_barriers_dynamic(), 1, at, dst);

  // Clobber clobbered registers when running with the debug-code flag
  // turned on to provoke errors.
  if (emit_debug_code()) {
    li(dst, Operand(bit_cast<int32_t>(kZapValue + 12)));
    li(map, Operand(bit_cast<int32_t>(kZapValue + 16)));
  }
}


// Clobbers object, address, value, and ra, if (ra_status == kRAHasBeenSaved)
// The register 'object' contains a heap object pointer.  The heap object
// tag is shifted away.
void MacroAssembler::RecordWrite(
    Register object,
    Register address,
    Register value,
    RAStatus ra_status,
    SaveFPRegsMode fp_mode,
    RememberedSetAction remembered_set_action,
    SmiCheck smi_check,
    PointersToHereCheck pointers_to_here_check_for_value) {
  DCHECK(!AreAliased(object, address, value, t8));
  DCHECK(!AreAliased(object, address, value, t9));

  if (emit_debug_code()) {
    lw(at, MemOperand(address));
    Assert(
        eq, kWrongAddressOrValuePassedToRecordWrite, at, Operand(value));
  }

  if (remembered_set_action == OMIT_REMEMBERED_SET &&
      !FLAG_incremental_marking) {
    return;
  }

  // First, check if a write barrier is even needed. The tests below
  // catch stores of smis and stores into the young generation.
  Label done;

  if (smi_check == INLINE_SMI_CHECK) {
    DCHECK_EQ(0, kSmiTag);
    JumpIfSmi(value, &done);
  }

  if (pointers_to_here_check_for_value != kPointersToHereAreAlwaysInteresting) {
    CheckPageFlag(value,
                  value,  // Used as scratch.
                  MemoryChunk::kPointersToHereAreInterestingMask,
                  eq,
                  &done);
  }
  CheckPageFlag(object,
                value,  // Used as scratch.
                MemoryChunk::kPointersFromHereAreInterestingMask,
                eq,
                &done);

  // Record the actual write.
  if (ra_status == kRAHasNotBeenSaved) {
    push(ra);
  }
  RecordWriteStub stub(isolate(), object, value, address, remembered_set_action,
                       fp_mode);
  CallStub(&stub);
  if (ra_status == kRAHasNotBeenSaved) {
    pop(ra);
  }

  bind(&done);

  // Count number of write barriers in generated code.
  isolate()->counters()->write_barriers_static()->Increment();
  IncrementCounter(isolate()->counters()->write_barriers_dynamic(), 1, at,
                   value);

  // Clobber clobbered registers when running with the debug-code flag
  // turned on to provoke errors.
  if (emit_debug_code()) {
    li(address, Operand(bit_cast<int32_t>(kZapValue + 12)));
    li(value, Operand(bit_cast<int32_t>(kZapValue + 16)));
  }
}

void MacroAssembler::RecordWriteCodeEntryField(Register js_function,
                                               Register code_entry,
                                               Register scratch) {
  const int offset = JSFunction::kCodeEntryOffset;

  // Since a code entry (value) is always in old space, we don't need to update
  // remembered set. If incremental marking is off, there is nothing for us to
  // do.
  if (!FLAG_incremental_marking) return;

  DCHECK(js_function.is(a1));
  DCHECK(code_entry.is(t0));
  DCHECK(scratch.is(t1));
  AssertNotSmi(js_function);

  if (emit_debug_code()) {
    Addu(scratch, js_function, Operand(offset - kHeapObjectTag));
    lw(at, MemOperand(scratch));
    Assert(eq, kWrongAddressOrValuePassedToRecordWrite, at,
           Operand(code_entry));
  }

  // First, check if a write barrier is even needed. The tests below
  // catch stores of Smis and stores into young gen.
  Label done;

  CheckPageFlag(code_entry, scratch,
                MemoryChunk::kPointersToHereAreInterestingMask, eq, &done);
  CheckPageFlag(js_function, scratch,
                MemoryChunk::kPointersFromHereAreInterestingMask, eq, &done);

  const Register dst = scratch;
  Addu(dst, js_function, Operand(offset - kHeapObjectTag));

  // Save caller-saved registers. js_function and code_entry are in the
  // caller-saved register list.
  DCHECK(kJSCallerSaved & js_function.bit());
  DCHECK(kJSCallerSaved & code_entry.bit());
  MultiPush(kJSCallerSaved | ra.bit());

  int argument_count = 3;

  PrepareCallCFunction(argument_count, 0, code_entry);

  mov(a0, js_function);
  mov(a1, dst);
  li(a2, Operand(ExternalReference::isolate_address(isolate())));

  {
    AllowExternalCallThatCantCauseGC scope(this);
    CallCFunction(
        ExternalReference::incremental_marking_record_write_code_entry_function(
            isolate()),
        argument_count);
  }

  // Restore caller-saved registers.
  MultiPop(kJSCallerSaved | ra.bit());

  bind(&done);
}

void MacroAssembler::RememberedSetHelper(Register object,  // For debug tests.
                                         Register address,
                                         Register scratch,
                                         SaveFPRegsMode fp_mode,
                                         RememberedSetFinalAction and_then) {
  Label done;
  if (emit_debug_code()) {
    Label ok;
    JumpIfNotInNewSpace(object, scratch, &ok);
    stop("Remembered set pointer is in new space");
    bind(&ok);
  }
  // Load store buffer top.
  ExternalReference store_buffer =
      ExternalReference::store_buffer_top(isolate());
  li(t8, Operand(store_buffer));
  lw(scratch, MemOperand(t8));
  // Store pointer to buffer and increment buffer top.
  sw(address, MemOperand(scratch));
  Addu(scratch, scratch, kPointerSize);
  // Write back new top of buffer.
  sw(scratch, MemOperand(t8));
  // Call stub on end of buffer.
  // Check for end of buffer.
  And(t8, scratch, Operand(StoreBuffer::kStoreBufferOverflowBit));
  if (and_then == kFallThroughAtEnd) {
    Branch(&done, eq, t8, Operand(zero_reg));
  } else {
    DCHECK(and_then == kReturnAtEnd);
    Ret(eq, t8, Operand(zero_reg));
  }
  push(ra);
  StoreBufferOverflowStub store_buffer_overflow(isolate(), fp_mode);
  CallStub(&store_buffer_overflow);
  pop(ra);
  bind(&done);
  if (and_then == kReturnAtEnd) {
    Ret();
  }
}


// -----------------------------------------------------------------------------
// Allocation support.


void MacroAssembler::CheckAccessGlobalProxy(Register holder_reg,
                                            Register scratch,
                                            Label* miss) {
  Label same_contexts;

  DCHECK(!holder_reg.is(scratch));
  DCHECK(!holder_reg.is(at));
  DCHECK(!scratch.is(at));

  // Load current lexical context from the stack frame.
  lw(scratch, MemOperand(fp, StandardFrameConstants::kContextOffset));
  // In debug mode, make sure the lexical context is set.
#ifdef DEBUG
  Check(ne, kWeShouldNotHaveAnEmptyLexicalContext,
      scratch, Operand(zero_reg));
#endif

  // Load the native context of the current context.
  lw(scratch, ContextMemOperand(scratch, Context::NATIVE_CONTEXT_INDEX));

  // Check the context is a native context.
  if (emit_debug_code()) {
    push(holder_reg);  // Temporarily save holder on the stack.
    // Read the first word and compare to the native_context_map.
    lw(holder_reg, FieldMemOperand(scratch, HeapObject::kMapOffset));
    LoadRoot(at, Heap::kNativeContextMapRootIndex);
    Check(eq, kJSGlobalObjectNativeContextShouldBeANativeContext,
          holder_reg, Operand(at));
    pop(holder_reg);  // Restore holder.
  }

  // Check if both contexts are the same.
  lw(at, FieldMemOperand(holder_reg, JSGlobalProxy::kNativeContextOffset));
  Branch(&same_contexts, eq, scratch, Operand(at));

  // Check the context is a native context.
  if (emit_debug_code()) {
    push(holder_reg);  // Temporarily save holder on the stack.
    mov(holder_reg, at);  // Move at to its holding place.
    LoadRoot(at, Heap::kNullValueRootIndex);
    Check(ne, kJSGlobalProxyContextShouldNotBeNull,
          holder_reg, Operand(at));

    lw(holder_reg, FieldMemOperand(holder_reg, HeapObject::kMapOffset));
    LoadRoot(at, Heap::kNativeContextMapRootIndex);
    Check(eq, kJSGlobalObjectNativeContextShouldBeANativeContext,
          holder_reg, Operand(at));
    // Restore at is not needed. at is reloaded below.
    pop(holder_reg);  // Restore holder.
    // Restore at to holder's context.
    lw(at, FieldMemOperand(holder_reg, JSGlobalProxy::kNativeContextOffset));
  }

  // Check that the security token in the calling global object is
  // compatible with the security token in the receiving global
  // object.
  int token_offset = Context::kHeaderSize +
                     Context::SECURITY_TOKEN_INDEX * kPointerSize;

  lw(scratch, FieldMemOperand(scratch, token_offset));
  lw(at, FieldMemOperand(at, token_offset));
  Branch(miss, ne, scratch, Operand(at));

  bind(&same_contexts);
}


// Compute the hash code from the untagged key.  This must be kept in sync with
// ComputeIntegerHash in utils.h and KeyedLoadGenericStub in
// code-stub-hydrogen.cc
void MacroAssembler::GetNumberHash(Register reg0, Register scratch) {
  // First of all we assign the hash seed to scratch.
  LoadRoot(scratch, Heap::kHashSeedRootIndex);
  SmiUntag(scratch);

  // Xor original key with a seed.
  xor_(reg0, reg0, scratch);

  // Compute the hash code from the untagged key.  This must be kept in sync
  // with ComputeIntegerHash in utils.h.
  //
  // hash = ~hash + (hash << 15);
  nor(scratch, reg0, zero_reg);
  Lsa(reg0, scratch, reg0, 15);

  // hash = hash ^ (hash >> 12);
  srl(at, reg0, 12);
  xor_(reg0, reg0, at);

  // hash = hash + (hash << 2);
  Lsa(reg0, reg0, reg0, 2);

  // hash = hash ^ (hash >> 4);
  srl(at, reg0, 4);
  xor_(reg0, reg0, at);

  // hash = hash * 2057;
  sll(scratch, reg0, 11);
  Lsa(reg0, reg0, reg0, 3);
  addu(reg0, reg0, scratch);

  // hash = hash ^ (hash >> 16);
  srl(at, reg0, 16);
  xor_(reg0, reg0, at);
  And(reg0, reg0, Operand(0x3fffffff));
}


void MacroAssembler::LoadFromNumberDictionary(Label* miss,
                                              Register elements,
                                              Register key,
                                              Register result,
                                              Register reg0,
                                              Register reg1,
                                              Register reg2) {
  // Register use:
  //
  // elements - holds the slow-case elements of the receiver on entry.
  //            Unchanged unless 'result' is the same register.
  //
  // key      - holds the smi key on entry.
  //            Unchanged unless 'result' is the same register.
  //
  //
  // result   - holds the result on exit if the load succeeded.
  //            Allowed to be the same as 'key' or 'result'.
  //            Unchanged on bailout so 'key' or 'result' can be used
  //            in further computation.
  //
  // Scratch registers:
  //
  // reg0 - holds the untagged key on entry and holds the hash once computed.
  //
  // reg1 - Used to hold the capacity mask of the dictionary.
  //
  // reg2 - Used for the index into the dictionary.
  // at   - Temporary (avoid MacroAssembler instructions also using 'at').
  Label done;

  GetNumberHash(reg0, reg1);

  // Compute the capacity mask.
  lw(reg1, FieldMemOperand(elements, SeededNumberDictionary::kCapacityOffset));
  sra(reg1, reg1, kSmiTagSize);
  Subu(reg1, reg1, Operand(1));

  // Generate an unrolled loop that performs a few probes before giving up.
  for (int i = 0; i < kNumberDictionaryProbes; i++) {
    // Use reg2 for index calculations and keep the hash intact in reg0.
    mov(reg2, reg0);
    // Compute the masked index: (hash + i + i * i) & mask.
    if (i > 0) {
      Addu(reg2, reg2, Operand(SeededNumberDictionary::GetProbeOffset(i)));
    }
    and_(reg2, reg2, reg1);

    // Scale the index by multiplying by the element size.
    DCHECK(SeededNumberDictionary::kEntrySize == 3);
    Lsa(reg2, reg2, reg2, 1);  // reg2 = reg2 * 3.

    // Check if the key is identical to the name.
    Lsa(reg2, elements, reg2, kPointerSizeLog2);

    lw(at, FieldMemOperand(reg2, SeededNumberDictionary::kElementsStartOffset));
    if (i != kNumberDictionaryProbes - 1) {
      Branch(&done, eq, key, Operand(at));
    } else {
      Branch(miss, ne, key, Operand(at));
    }
  }

  bind(&done);
  // Check that the value is a field property.
  // reg2: elements + (index * kPointerSize).
  const int kDetailsOffset =
      SeededNumberDictionary::kElementsStartOffset + 2 * kPointerSize;
  lw(reg1, FieldMemOperand(reg2, kDetailsOffset));
  DCHECK_EQ(DATA, 0);
  And(at, reg1, Operand(Smi::FromInt(PropertyDetails::TypeField::kMask)));
  Branch(miss, ne, at, Operand(zero_reg));

  // Get the value at the masked, scaled index and return.
  const int kValueOffset =
      SeededNumberDictionary::kElementsStartOffset + kPointerSize;
  lw(result, FieldMemOperand(reg2, kValueOffset));
}


// ---------------------------------------------------------------------------
// Instruction macros.

void MacroAssembler::Addu(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    addu(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseReg(rt.rmode_)) {
      addiu(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      DCHECK(!rs.is(at));
      li(at, rt);
      addu(rd, rs, at);
    }
  }
}


void MacroAssembler::Subu(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    subu(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseReg(rt.rmode_)) {
      addiu(rd, rs, -rt.imm32_);  // No subiu instr, use addiu(x, y, -imm).
    } else {
      // li handles the relocation.
      DCHECK(!rs.is(at));
      li(at, rt);
      subu(rd, rs, at);
    }
  }
}


void MacroAssembler::Mul(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    if (IsMipsArchVariant(kLoongson)) {
      mult(rs, rt.rm());
      mflo(rd);
    } else {
      mul(rd, rs, rt.rm());
    }
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    if (IsMipsArchVariant(kLoongson)) {
      mult(rs, at);
      mflo(rd);
    } else {
      mul(rd, rs, at);
    }
  }
}


void MacroAssembler::Mul(Register rd_hi, Register rd_lo,
    Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    if (!IsMipsArchVariant(kMips32r6)) {
      mult(rs, rt.rm());
      mflo(rd_lo);
      mfhi(rd_hi);
    } else {
      if (rd_lo.is(rs)) {
        DCHECK(!rd_hi.is(rs));
        DCHECK(!rd_hi.is(rt.rm()) && !rd_lo.is(rt.rm()));
        muh(rd_hi, rs, rt.rm());
        mul(rd_lo, rs, rt.rm());
      } else {
        DCHECK(!rd_hi.is(rt.rm()) && !rd_lo.is(rt.rm()));
        mul(rd_lo, rs, rt.rm());
        muh(rd_hi, rs, rt.rm());
      }
    }
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    if (!IsMipsArchVariant(kMips32r6)) {
      mult(rs, at);
      mflo(rd_lo);
      mfhi(rd_hi);
    } else {
      if (rd_lo.is(rs)) {
        DCHECK(!rd_hi.is(rs));
        DCHECK(!rd_hi.is(at) && !rd_lo.is(at));
        muh(rd_hi, rs, at);
        mul(rd_lo, rs, at);
      } else {
        DCHECK(!rd_hi.is(at) && !rd_lo.is(at));
        mul(rd_lo, rs, at);
        muh(rd_hi, rs, at);
      }
    }
  }
}


void MacroAssembler::Mulh(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    if (!IsMipsArchVariant(kMips32r6)) {
      mult(rs, rt.rm());
      mfhi(rd);
    } else {
      muh(rd, rs, rt.rm());
    }
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    if (!IsMipsArchVariant(kMips32r6)) {
      mult(rs, at);
      mfhi(rd);
    } else {
      muh(rd, rs, at);
    }
  }
}


void MacroAssembler::Mult(Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    mult(rs, rt.rm());
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    mult(rs, at);
  }
}


void MacroAssembler::Mulhu(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    if (!IsMipsArchVariant(kMips32r6)) {
      multu(rs, rt.rm());
      mfhi(rd);
    } else {
      muhu(rd, rs, rt.rm());
    }
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    if (!IsMipsArchVariant(kMips32r6)) {
      multu(rs, at);
      mfhi(rd);
    } else {
      muhu(rd, rs, at);
    }
  }
}


void MacroAssembler::Multu(Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    multu(rs, rt.rm());
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    multu(rs, at);
  }
}


void MacroAssembler::Div(Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    div(rs, rt.rm());
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    div(rs, at);
  }
}


void MacroAssembler::Div(Register rem, Register res,
    Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    if (!IsMipsArchVariant(kMips32r6)) {
      div(rs, rt.rm());
      mflo(res);
      mfhi(rem);
    } else {
      div(res, rs, rt.rm());
      mod(rem, rs, rt.rm());
    }
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    if (!IsMipsArchVariant(kMips32r6)) {
      div(rs, at);
      mflo(res);
      mfhi(rem);
    } else {
      div(res, rs, at);
      mod(rem, rs, at);
    }
  }
}


void MacroAssembler::Div(Register res, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    if (!IsMipsArchVariant(kMips32r6)) {
      div(rs, rt.rm());
      mflo(res);
    } else {
      div(res, rs, rt.rm());
    }
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    if (!IsMipsArchVariant(kMips32r6)) {
      div(rs, at);
      mflo(res);
    } else {
      div(res, rs, at);
    }
  }
}


void MacroAssembler::Mod(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    if (!IsMipsArchVariant(kMips32r6)) {
      div(rs, rt.rm());
      mfhi(rd);
    } else {
      mod(rd, rs, rt.rm());
    }
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    if (!IsMipsArchVariant(kMips32r6)) {
      div(rs, at);
      mfhi(rd);
    } else {
      mod(rd, rs, at);
    }
  }
}


void MacroAssembler::Modu(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    if (!IsMipsArchVariant(kMips32r6)) {
      divu(rs, rt.rm());
      mfhi(rd);
    } else {
      modu(rd, rs, rt.rm());
    }
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    if (!IsMipsArchVariant(kMips32r6)) {
      divu(rs, at);
      mfhi(rd);
    } else {
      modu(rd, rs, at);
    }
  }
}


void MacroAssembler::Divu(Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    divu(rs, rt.rm());
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    divu(rs, at);
  }
}


void MacroAssembler::Divu(Register res, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    if (!IsMipsArchVariant(kMips32r6)) {
      divu(rs, rt.rm());
      mflo(res);
    } else {
      divu(res, rs, rt.rm());
    }
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    if (!IsMipsArchVariant(kMips32r6)) {
      divu(rs, at);
      mflo(res);
    } else {
      divu(res, rs, at);
    }
  }
}


void MacroAssembler::And(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    and_(rd, rs, rt.rm());
  } else {
    if (is_uint16(rt.imm32_) && !MustUseReg(rt.rmode_)) {
      andi(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      DCHECK(!rs.is(at));
      li(at, rt);
      and_(rd, rs, at);
    }
  }
}


void MacroAssembler::Or(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    or_(rd, rs, rt.rm());
  } else {
    if (is_uint16(rt.imm32_) && !MustUseReg(rt.rmode_)) {
      ori(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      DCHECK(!rs.is(at));
      li(at, rt);
      or_(rd, rs, at);
    }
  }
}


void MacroAssembler::Xor(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    xor_(rd, rs, rt.rm());
  } else {
    if (is_uint16(rt.imm32_) && !MustUseReg(rt.rmode_)) {
      xori(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      DCHECK(!rs.is(at));
      li(at, rt);
      xor_(rd, rs, at);
    }
  }
}


void MacroAssembler::Nor(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    nor(rd, rs, rt.rm());
  } else {
    // li handles the relocation.
    DCHECK(!rs.is(at));
    li(at, rt);
    nor(rd, rs, at);
  }
}


void MacroAssembler::Neg(Register rs, const Operand& rt) {
  DCHECK(rt.is_reg());
  DCHECK(!at.is(rs));
  DCHECK(!at.is(rt.rm()));
  li(at, -1);
  xor_(rs, rt.rm(), at);
}


void MacroAssembler::Slt(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    slt(rd, rs, rt.rm());
  } else {
    if (is_int16(rt.imm32_) && !MustUseReg(rt.rmode_)) {
      slti(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      DCHECK(!rs.is(at));
      li(at, rt);
      slt(rd, rs, at);
    }
  }
}


void MacroAssembler::Sltu(Register rd, Register rs, const Operand& rt) {
  if (rt.is_reg()) {
    sltu(rd, rs, rt.rm());
  } else {
    if (is_uint16(rt.imm32_) && !MustUseReg(rt.rmode_)) {
      sltiu(rd, rs, rt.imm32_);
    } else {
      // li handles the relocation.
      DCHECK(!rs.is(at));
      li(at, rt);
      sltu(rd, rs, at);
    }
  }
}


void MacroAssembler::Ror(Register rd, Register rs, const Operand& rt) {
  if (IsMipsArchVariant(kMips32r2) || IsMipsArchVariant(kMips32r6)) {
    if (rt.is_reg()) {
      rotrv(rd, rs, rt.rm());
    } else {
      rotr(rd, rs, rt.imm32_);
    }
  } else {
    if (rt.is_reg()) {
      subu(at, zero_reg, rt.rm());
      sllv(at, rs, at);
      srlv(rd, rs, rt.rm());
      or_(rd, rd, at);
    } else {
      if (rt.imm32_ == 0) {
        srl(rd, rs, 0);
      } else {
        srl(at, rs, rt.imm32_);
        sll(rd, rs, (0x20 - rt.imm32_) & 0x1f);
        or_(rd, rd, at);
      }
    }
  }
}


void MacroAssembler::Pref(int32_t hint, const MemOperand& rs) {
  if (IsMipsArchVariant(kLoongson)) {
    lw(zero_reg, rs);
  } else {
    pref(hint, rs);
  }
}


void MacroAssembler::Lsa(Register rd, Register rt, Register rs, uint8_t sa,
                         Register scratch) {
  if (IsMipsArchVariant(kMips32r6) && sa <= 4) {
    lsa(rd, rt, rs, sa);
  } else {
    Register tmp = rd.is(rt) ? scratch : rd;
    DCHECK(!tmp.is(rt));
    sll(tmp, rs, sa);
    Addu(rd, rt, tmp);
  }
}


// ------------Pseudo-instructions-------------

void MacroAssembler::Ulw(Register rd, const MemOperand& rs) {
  lwr(rd, rs);
  lwl(rd, MemOperand(rs.rm(), rs.offset() + 3));
}


void MacroAssembler::Usw(Register rd, const MemOperand& rs) {
  swr(rd, rs);
  swl(rd, MemOperand(rs.rm(), rs.offset() + 3));
}


void MacroAssembler::li(Register dst, Handle<Object> value, LiFlags mode) {
  AllowDeferredHandleDereference smi_check;
  if (value->IsSmi()) {
    li(dst, Operand(value), mode);
  } else {
    DCHECK(value->IsHeapObject());
    if (isolate()->heap()->InNewSpace(*value)) {
      Handle<Cell> cell = isolate()->factory()->NewCell(value);
      li(dst, Operand(cell));
      lw(dst, FieldMemOperand(dst, Cell::kValueOffset));
    } else {
      li(dst, Operand(value));
    }
  }
}


void MacroAssembler::li(Register rd, Operand j, LiFlags mode) {
  DCHECK(!j.is_reg());
  BlockTrampolinePoolScope block_trampoline_pool(this);
  if (!MustUseReg(j.rmode_) && mode == OPTIMIZE_SIZE) {
    // Normal load of an immediate value which does not need Relocation Info.
    if (is_int16(j.imm32_)) {
      addiu(rd, zero_reg, j.imm32_);
    } else if (!(j.imm32_ & kHiMask)) {
      ori(rd, zero_reg, j.imm32_);
    } else if (!(j.imm32_ & kImm16Mask)) {
      lui(rd, (j.imm32_ >> kLuiShift) & kImm16Mask);
    } else {
      lui(rd, (j.imm32_ >> kLuiShift) & kImm16Mask);
      ori(rd, rd, (j.imm32_ & kImm16Mask));
    }
  } else {
    if (MustUseReg(j.rmode_)) {
      RecordRelocInfo(j.rmode_, j.imm32_);
    }
    // We always need the same number of instructions as we may need to patch
    // this code to load another value which may need 2 instructions to load.
    lui(rd, (j.imm32_ >> kLuiShift) & kImm16Mask);
    ori(rd, rd, (j.imm32_ & kImm16Mask));
  }
}


void MacroAssembler::MultiPush(RegList regs) {
  int16_t num_to_push = NumberOfBitsSet(regs);
  int16_t stack_offset = num_to_push * kPointerSize;

  Subu(sp, sp, Operand(stack_offset));
  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs & (1 << i)) != 0) {
      stack_offset -= kPointerSize;
      sw(ToRegister(i), MemOperand(sp, stack_offset));
    }
  }
}


void MacroAssembler::MultiPushReversed(RegList regs) {
  int16_t num_to_push = NumberOfBitsSet(regs);
  int16_t stack_offset = num_to_push * kPointerSize;

  Subu(sp, sp, Operand(stack_offset));
  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs & (1 << i)) != 0) {
      stack_offset -= kPointerSize;
      sw(ToRegister(i), MemOperand(sp, stack_offset));
    }
  }
}


void MacroAssembler::MultiPop(RegList regs) {
  int16_t stack_offset = 0;

  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs & (1 << i)) != 0) {
      lw(ToRegister(i), MemOperand(sp, stack_offset));
      stack_offset += kPointerSize;
    }
  }
  addiu(sp, sp, stack_offset);
}


void MacroAssembler::MultiPopReversed(RegList regs) {
  int16_t stack_offset = 0;

  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs & (1 << i)) != 0) {
      lw(ToRegister(i), MemOperand(sp, stack_offset));
      stack_offset += kPointerSize;
    }
  }
  addiu(sp, sp, stack_offset);
}


void MacroAssembler::MultiPushFPU(RegList regs) {
  int16_t num_to_push = NumberOfBitsSet(regs);
  int16_t stack_offset = num_to_push * kDoubleSize;

  Subu(sp, sp, Operand(stack_offset));
  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs & (1 << i)) != 0) {
      stack_offset -= kDoubleSize;
      sdc1(FPURegister::from_code(i), MemOperand(sp, stack_offset));
    }
  }
}


void MacroAssembler::MultiPushReversedFPU(RegList regs) {
  int16_t num_to_push = NumberOfBitsSet(regs);
  int16_t stack_offset = num_to_push * kDoubleSize;

  Subu(sp, sp, Operand(stack_offset));
  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs & (1 << i)) != 0) {
      stack_offset -= kDoubleSize;
      sdc1(FPURegister::from_code(i), MemOperand(sp, stack_offset));
    }
  }
}


void MacroAssembler::MultiPopFPU(RegList regs) {
  int16_t stack_offset = 0;

  for (int16_t i = 0; i < kNumRegisters; i++) {
    if ((regs & (1 << i)) != 0) {
      ldc1(FPURegister::from_code(i), MemOperand(sp, stack_offset));
      stack_offset += kDoubleSize;
    }
  }
  addiu(sp, sp, stack_offset);
}


void MacroAssembler::MultiPopReversedFPU(RegList regs) {
  int16_t stack_offset = 0;

  for (int16_t i = kNumRegisters - 1; i >= 0; i--) {
    if ((regs & (1 << i)) != 0) {
      ldc1(FPURegister::from_code(i), MemOperand(sp, stack_offset));
      stack_offset += kDoubleSize;
    }
  }
  addiu(sp, sp, stack_offset);
}


void MacroAssembler::Ext(Register rt,
                         Register rs,
                         uint16_t pos,
                         uint16_t size) {
  DCHECK(pos < 32);
  DCHECK(pos + size < 33);

  if (IsMipsArchVariant(kMips32r2) || IsMipsArchVariant(kMips32r6)) {
    ext_(rt, rs, pos, size);
  } else {
    // Move rs to rt and shift it left then right to get the
    // desired bitfield on the right side and zeroes on the left.
    int shift_left = 32 - (pos + size);
    sll(rt, rs, shift_left);  // Acts as a move if shift_left == 0.

    int shift_right = 32 - size;
    if (shift_right > 0) {
      srl(rt, rt, shift_right);
    }
  }
}


void MacroAssembler::Ins(Register rt,
                         Register rs,
                         uint16_t pos,
                         uint16_t size) {
  DCHECK(pos < 32);
  DCHECK(pos + size <= 32);
  DCHECK(size != 0);

  if (IsMipsArchVariant(kMips32r2) || IsMipsArchVariant(kMips32r6)) {
    ins_(rt, rs, pos, size);
  } else {
    DCHECK(!rt.is(t8) && !rs.is(t8));
    Subu(at, zero_reg, Operand(1));
    srl(at, at, 32 - size);
    and_(t8, rs, at);
    sll(t8, t8, pos);
    sll(at, at, pos);
    nor(at, at, zero_reg);
    and_(at, rt, at);
    or_(rt, t8, at);
  }
}


void MacroAssembler::Cvt_d_uw(FPURegister fd, Register rs,
                              FPURegister scratch) {
  // In FP64Mode we do convertion from long.
  if (IsFp64Mode()) {
    mtc1(rs, scratch);
    Mthc1(zero_reg, scratch);
    cvt_d_l(fd, scratch);
  } else {
    // Convert rs to a FP value in fd.
    DCHECK(!fd.is(scratch));
    DCHECK(!rs.is(at));

    Label msb_clear, conversion_done;
    // For a value which is < 2^31, regard it as a signed positve word.
    Branch(&msb_clear, ge, rs, Operand(zero_reg), USE_DELAY_SLOT);
    mtc1(rs, fd);

    li(at, 0x41F00000);  // FP value: 2^32.

    // For unsigned inputs > 2^31, we convert to double as a signed int32,
    // then add 2^32 to move it back to unsigned value in range 2^31..2^31-1.
    mtc1(zero_reg, scratch);
    Mthc1(at, scratch);

    cvt_d_w(fd, fd);

    Branch(USE_DELAY_SLOT, &conversion_done);
    add_d(fd, fd, scratch);

    bind(&msb_clear);
    cvt_d_w(fd, fd);

    bind(&conversion_done);
  }
}


void MacroAssembler::Trunc_uw_d(FPURegister fd,
                                FPURegister fs,
                                FPURegister scratch) {
  Trunc_uw_d(fs, t8, scratch);
  mtc1(t8, fd);
}


void MacroAssembler::Trunc_w_d(FPURegister fd, FPURegister fs) {
  if (IsMipsArchVariant(kLoongson) && fd.is(fs)) {
    Mfhc1(t8, fs);
    trunc_w_d(fd, fs);
    Mthc1(t8, fs);
  } else {
    trunc_w_d(fd, fs);
  }
}


void MacroAssembler::Round_w_d(FPURegister fd, FPURegister fs) {
  if (IsMipsArchVariant(kLoongson) && fd.is(fs)) {
    Mfhc1(t8, fs);
    round_w_d(fd, fs);
    Mthc1(t8, fs);
  } else {
    round_w_d(fd, fs);
  }
}


void MacroAssembler::Floor_w_d(FPURegister fd, FPURegister fs) {
  if (IsMipsArchVariant(kLoongson) && fd.is(fs)) {
    Mfhc1(t8, fs);
    floor_w_d(fd, fs);
    Mthc1(t8, fs);
  } else {
    floor_w_d(fd, fs);
  }
}


void MacroAssembler::Ceil_w_d(FPURegister fd, FPURegister fs) {
  if (IsMipsArchVariant(kLoongson) && fd.is(fs)) {
    Mfhc1(t8, fs);
    ceil_w_d(fd, fs);
    Mthc1(t8, fs);
  } else {
    ceil_w_d(fd, fs);
  }
}


void MacroAssembler::Trunc_uw_d(FPURegister fd,
                                Register rs,
                                FPURegister scratch) {
  DCHECK(!fd.is(scratch));
  DCHECK(!rs.is(at));

  // Load 2^31 into scratch as its float representation.
  li(at, 0x41E00000);
  mtc1(zero_reg, scratch);
  Mthc1(at, scratch);
  // Test if scratch > fd.
  // If fd < 2^31 we can convert it normally.
  Label simple_convert;
  BranchF(&simple_convert, NULL, lt, fd, scratch);

  // First we subtract 2^31 from fd, then trunc it to rs
  // and add 2^31 to rs.
  sub_d(scratch, fd, scratch);
  trunc_w_d(scratch, scratch);
  mfc1(rs, scratch);
  Or(rs, rs, 1 << 31);

  Label done;
  Branch(&done);
  // Simple conversion.
  bind(&simple_convert);
  trunc_w_d(scratch, fd);
  mfc1(rs, scratch);

  bind(&done);
}


void MacroAssembler::Mthc1(Register rt, FPURegister fs) {
  if (IsFp64Mode()) {
    mthc1(rt, fs);
  } else {
    mtc1(rt, fs.high());
  }
}


void MacroAssembler::Mfhc1(Register rt, FPURegister fs) {
  if (IsFp64Mode()) {
    mfhc1(rt, fs);
  } else {
    mfc1(rt, fs.high());
  }
}


void MacroAssembler::BranchFCommon(SecondaryField sizeField, Label* target,
                                   Label* nan, Condition cond, FPURegister cmp1,
                                   FPURegister cmp2, BranchDelaySlot bd) {
  {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    if (cond == al) {
      Branch(bd, target);
      return;
    }

    if (IsMipsArchVariant(kMips32r6)) {
      sizeField = sizeField == D ? L : W;
    }
    DCHECK(nan || target);
    // Check for unordered (NaN) cases.
    if (nan) {
      bool long_branch =
          nan->is_bound() ? is_near(nan) : is_trampoline_emitted();
      if (!IsMipsArchVariant(kMips32r6)) {
        if (long_branch) {
          Label skip;
          c(UN, sizeField, cmp1, cmp2);
          bc1f(&skip);
          nop();
          BranchLong(nan, bd);
          bind(&skip);
        } else {
          c(UN, sizeField, cmp1, cmp2);
          bc1t(nan);
          if (bd == PROTECT) {
            nop();
          }
        }
      } else {
        // Use kDoubleCompareReg for comparison result. It has to be unavailable
        // to lithium register allocator.
        DCHECK(!cmp1.is(kDoubleCompareReg) && !cmp2.is(kDoubleCompareReg));
        if (long_branch) {
          Label skip;
          cmp(UN, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1eqz(&skip, kDoubleCompareReg);
          nop();
          BranchLong(nan, bd);
          bind(&skip);
        } else {
          cmp(UN, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1nez(nan, kDoubleCompareReg);
          if (bd == PROTECT) {
            nop();
          }
        }
      }
    }

    if (target) {
      bool long_branch =
          target->is_bound() ? is_near(target) : is_trampoline_emitted();
      if (long_branch) {
        Label skip;
        Condition neg_cond = NegateFpuCondition(cond);
        BranchShortF(sizeField, &skip, neg_cond, cmp1, cmp2, bd);
        BranchLong(target, bd);
        bind(&skip);
      } else {
        BranchShortF(sizeField, target, cond, cmp1, cmp2, bd);
      }
    }
  }
}

void MacroAssembler::BranchShortF(SecondaryField sizeField, Label* target,
                                  Condition cc, FPURegister cmp1,
                                  FPURegister cmp2, BranchDelaySlot bd) {
  if (!IsMipsArchVariant(kMips32r6)) {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    if (target) {
      // Here NaN cases were either handled by this function or are assumed to
      // have been handled by the caller.
      switch (cc) {
        case lt:
          c(OLT, sizeField, cmp1, cmp2);
          bc1t(target);
          break;
        case ult:
          c(ULT, sizeField, cmp1, cmp2);
          bc1t(target);
          break;
        case gt:
          c(ULE, sizeField, cmp1, cmp2);
          bc1f(target);
          break;
        case ugt:
          c(OLE, sizeField, cmp1, cmp2);
          bc1f(target);
          break;
        case ge:
          c(ULT, sizeField, cmp1, cmp2);
          bc1f(target);
          break;
        case uge:
          c(OLT, sizeField, cmp1, cmp2);
          bc1f(target);
          break;
        case le:
          c(OLE, sizeField, cmp1, cmp2);
          bc1t(target);
          break;
        case ule:
          c(ULE, sizeField, cmp1, cmp2);
          bc1t(target);
          break;
        case eq:
          c(EQ, sizeField, cmp1, cmp2);
          bc1t(target);
          break;
        case ueq:
          c(UEQ, sizeField, cmp1, cmp2);
          bc1t(target);
          break;
        case ne:  // Unordered or not equal.
          c(EQ, sizeField, cmp1, cmp2);
          bc1f(target);
          break;
        case ogl:
          c(UEQ, sizeField, cmp1, cmp2);
          bc1f(target);
          break;
        default:
          CHECK(0);
      }
    }
  } else {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    if (target) {
      // Here NaN cases were either handled by this function or are assumed to
      // have been handled by the caller.
      // Unsigned conditions are treated as their signed counterpart.
      // Use kDoubleCompareReg for comparison result, it is
      // valid in fp64 (FR = 1) mode which is implied for mips32r6.
      DCHECK(!cmp1.is(kDoubleCompareReg) && !cmp2.is(kDoubleCompareReg));
      switch (cc) {
        case lt:
          cmp(OLT, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1nez(target, kDoubleCompareReg);
          break;
        case ult:
          cmp(ULT, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1nez(target, kDoubleCompareReg);
          break;
        case gt:
          cmp(ULE, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1eqz(target, kDoubleCompareReg);
          break;
        case ugt:
          cmp(OLE, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1eqz(target, kDoubleCompareReg);
          break;
        case ge:
          cmp(ULT, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1eqz(target, kDoubleCompareReg);
          break;
        case uge:
          cmp(OLT, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1eqz(target, kDoubleCompareReg);
          break;
        case le:
          cmp(OLE, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1nez(target, kDoubleCompareReg);
          break;
        case ule:
          cmp(ULE, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1nez(target, kDoubleCompareReg);
          break;
        case eq:
          cmp(EQ, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1nez(target, kDoubleCompareReg);
          break;
        case ueq:
          cmp(UEQ, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1nez(target, kDoubleCompareReg);
          break;
        case ne:
          cmp(EQ, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1eqz(target, kDoubleCompareReg);
          break;
        case ogl:
          cmp(UEQ, sizeField, kDoubleCompareReg, cmp1, cmp2);
          bc1eqz(target, kDoubleCompareReg);
          break;
        default:
          CHECK(0);
      }
    }
  }
  if (bd == PROTECT) {
    nop();
  }
}


void MacroAssembler::FmoveLow(FPURegister dst, Register src_low) {
  if (IsFp64Mode()) {
    DCHECK(!src_low.is(at));
    mfhc1(at, dst);
    mtc1(src_low, dst);
    mthc1(at, dst);
  } else {
    mtc1(src_low, dst);
  }
}


void MacroAssembler::Move(FPURegister dst, float imm) {
  li(at, Operand(bit_cast<int32_t>(imm)));
  mtc1(at, dst);
}


void MacroAssembler::Move(FPURegister dst, double imm) {
  static const DoubleRepresentation minus_zero(-0.0);
  static const DoubleRepresentation zero(0.0);
  DoubleRepresentation value_rep(imm);
  // Handle special values first.
  if (value_rep == zero && has_double_zero_reg_set_) {
    mov_d(dst, kDoubleRegZero);
  } else if (value_rep == minus_zero && has_double_zero_reg_set_) {
    neg_d(dst, kDoubleRegZero);
  } else {
    uint32_t lo, hi;
    DoubleAsTwoUInt32(imm, &lo, &hi);
    // Move the low part of the double into the lower of the corresponding FPU
    // register of FPU register pair.
    if (lo != 0) {
      li(at, Operand(lo));
      mtc1(at, dst);
    } else {
      mtc1(zero_reg, dst);
    }
    // Move the high part of the double into the higher of the corresponding FPU
    // register of FPU register pair.
    if (hi != 0) {
      li(at, Operand(hi));
      Mthc1(at, dst);
    } else {
      Mthc1(zero_reg, dst);
    }
    if (dst.is(kDoubleRegZero)) has_double_zero_reg_set_ = true;
  }
}


void MacroAssembler::Movz(Register rd, Register rs, Register rt) {
  if (IsMipsArchVariant(kLoongson) || IsMipsArchVariant(kMips32r6)) {
    Label done;
    Branch(&done, ne, rt, Operand(zero_reg));
    mov(rd, rs);
    bind(&done);
  } else {
    movz(rd, rs, rt);
  }
}


void MacroAssembler::Movn(Register rd, Register rs, Register rt) {
  if (IsMipsArchVariant(kLoongson) || IsMipsArchVariant(kMips32r6)) {
    Label done;
    Branch(&done, eq, rt, Operand(zero_reg));
    mov(rd, rs);
    bind(&done);
  } else {
    movn(rd, rs, rt);
  }
}


void MacroAssembler::Movt(Register rd, Register rs, uint16_t cc) {
  if (IsMipsArchVariant(kLoongson)) {
    // Tests an FP condition code and then conditionally move rs to rd.
    // We do not currently use any FPU cc bit other than bit 0.
    DCHECK(cc == 0);
    DCHECK(!(rs.is(t8) || rd.is(t8)));
    Label done;
    Register scratch = t8;
    // For testing purposes we need to fetch content of the FCSR register and
    // than test its cc (floating point condition code) bit (for cc = 0, it is
    // 24. bit of the FCSR).
    cfc1(scratch, FCSR);
    // For the MIPS I, II and III architectures, the contents of scratch is
    // UNPREDICTABLE for the instruction immediately following CFC1.
    nop();
    srl(scratch, scratch, 16);
    andi(scratch, scratch, 0x0080);
    Branch(&done, eq, scratch, Operand(zero_reg));
    mov(rd, rs);
    bind(&done);
  } else {
    movt(rd, rs, cc);
  }
}


void MacroAssembler::Movf(Register rd, Register rs, uint16_t cc) {
  if (IsMipsArchVariant(kLoongson)) {
    // Tests an FP condition code and then conditionally move rs to rd.
    // We do not currently use any FPU cc bit other than bit 0.
    DCHECK(cc == 0);
    DCHECK(!(rs.is(t8) || rd.is(t8)));
    Label done;
    Register scratch = t8;
    // For testing purposes we need to fetch content of the FCSR register and
    // than test its cc (floating point condition code) bit (for cc = 0, it is
    // 24. bit of the FCSR).
    cfc1(scratch, FCSR);
    // For the MIPS I, II and III architectures, the contents of scratch is
    // UNPREDICTABLE for the instruction immediately following CFC1.
    nop();
    srl(scratch, scratch, 16);
    andi(scratch, scratch, 0x0080);
    Branch(&done, ne, scratch, Operand(zero_reg));
    mov(rd, rs);
    bind(&done);
  } else {
    movf(rd, rs, cc);
  }
}


void MacroAssembler::Clz(Register rd, Register rs) {
  if (IsMipsArchVariant(kLoongson)) {
    DCHECK(!(rd.is(t8) || rd.is(t9)) && !(rs.is(t8) || rs.is(t9)));
    Register mask = t8;
    Register scratch = t9;
    Label loop, end;
    mov(at, rs);
    mov(rd, zero_reg);
    lui(mask, 0x8000);
    bind(&loop);
    and_(scratch, at, mask);
    Branch(&end, ne, scratch, Operand(zero_reg));
    addiu(rd, rd, 1);
    Branch(&loop, ne, mask, Operand(zero_reg), USE_DELAY_SLOT);
    srl(mask, mask, 1);
    bind(&end);
  } else {
    clz(rd, rs);
  }
}


void MacroAssembler::EmitFPUTruncate(FPURoundingMode rounding_mode,
                                     Register result,
                                     DoubleRegister double_input,
                                     Register scratch,
                                     DoubleRegister double_scratch,
                                     Register except_flag,
                                     CheckForInexactConversion check_inexact) {
  DCHECK(!result.is(scratch));
  DCHECK(!double_input.is(double_scratch));
  DCHECK(!except_flag.is(scratch));

  Label done;

  // Clear the except flag (0 = no exception)
  mov(except_flag, zero_reg);

  // Test for values that can be exactly represented as a signed 32-bit integer.
  cvt_w_d(double_scratch, double_input);
  mfc1(result, double_scratch);
  cvt_d_w(double_scratch, double_scratch);
  BranchF(&done, NULL, eq, double_input, double_scratch);

  int32_t except_mask = kFCSRFlagMask;  // Assume interested in all exceptions.

  if (check_inexact == kDontCheckForInexactConversion) {
    // Ignore inexact exceptions.
    except_mask &= ~kFCSRInexactFlagMask;
  }

  // Save FCSR.
  cfc1(scratch, FCSR);
  // Disable FPU exceptions.
  ctc1(zero_reg, FCSR);

  // Do operation based on rounding mode.
  switch (rounding_mode) {
    case kRoundToNearest:
      Round_w_d(double_scratch, double_input);
      break;
    case kRoundToZero:
      Trunc_w_d(double_scratch, double_input);
      break;
    case kRoundToPlusInf:
      Ceil_w_d(double_scratch, double_input);
      break;
    case kRoundToMinusInf:
      Floor_w_d(double_scratch, double_input);
      break;
  }  // End of switch-statement.

  // Retrieve FCSR.
  cfc1(except_flag, FCSR);
  // Restore FCSR.
  ctc1(scratch, FCSR);
  // Move the converted value into the result register.
  mfc1(result, double_scratch);

  // Check for fpu exceptions.
  And(except_flag, except_flag, Operand(except_mask));

  bind(&done);
}


void MacroAssembler::TryInlineTruncateDoubleToI(Register result,
                                                DoubleRegister double_input,
                                                Label* done) {
  DoubleRegister single_scratch = kLithiumScratchDouble.low();
  Register scratch = at;
  Register scratch2 = t9;

  // Clear cumulative exception flags and save the FCSR.
  cfc1(scratch2, FCSR);
  ctc1(zero_reg, FCSR);
  // Try a conversion to a signed integer.
  trunc_w_d(single_scratch, double_input);
  mfc1(result, single_scratch);
  // Retrieve and restore the FCSR.
  cfc1(scratch, FCSR);
  ctc1(scratch2, FCSR);
  // Check for overflow and NaNs.
  And(scratch,
      scratch,
      kFCSROverflowFlagMask | kFCSRUnderflowFlagMask | kFCSRInvalidOpFlagMask);
  // If we had no exceptions we are done.
  Branch(done, eq, scratch, Operand(zero_reg));
}


void MacroAssembler::TruncateDoubleToI(Register result,
                                       DoubleRegister double_input) {
  Label done;

  TryInlineTruncateDoubleToI(result, double_input, &done);

  // If we fell through then inline version didn't succeed - call stub instead.
  push(ra);
  Subu(sp, sp, Operand(kDoubleSize));  // Put input on stack.
  sdc1(double_input, MemOperand(sp, 0));

  DoubleToIStub stub(isolate(), sp, result, 0, true, true);
  CallStub(&stub);

  Addu(sp, sp, Operand(kDoubleSize));
  pop(ra);

  bind(&done);
}


void MacroAssembler::TruncateHeapNumberToI(Register result, Register object) {
  Label done;
  DoubleRegister double_scratch = f12;
  DCHECK(!result.is(object));

  ldc1(double_scratch,
       MemOperand(object, HeapNumber::kValueOffset - kHeapObjectTag));
  TryInlineTruncateDoubleToI(result, double_scratch, &done);

  // If we fell through then inline version didn't succeed - call stub instead.
  push(ra);
  DoubleToIStub stub(isolate(),
                     object,
                     result,
                     HeapNumber::kValueOffset - kHeapObjectTag,
                     true,
                     true);
  CallStub(&stub);
  pop(ra);

  bind(&done);
}


void MacroAssembler::TruncateNumberToI(Register object,
                                       Register result,
                                       Register heap_number_map,
                                       Register scratch,
                                       Label* not_number) {
  Label done;
  DCHECK(!result.is(object));

  UntagAndJumpIfSmi(result, object, &done);
  JumpIfNotHeapNumber(object, heap_number_map, scratch, not_number);
  TruncateHeapNumberToI(result, object);

  bind(&done);
}


void MacroAssembler::GetLeastBitsFromSmi(Register dst,
                                         Register src,
                                         int num_least_bits) {
  Ext(dst, src, kSmiTagSize, num_least_bits);
}


void MacroAssembler::GetLeastBitsFromInt32(Register dst,
                                           Register src,
                                           int num_least_bits) {
  And(dst, src, Operand((1 << num_least_bits) - 1));
}


// Emulated condtional branches do not emit a nop in the branch delay slot.
//
// BRANCH_ARGS_CHECK checks that conditional jump arguments are correct.
#define BRANCH_ARGS_CHECK(cond, rs, rt) DCHECK(                                \
    (cond == cc_always && rs.is(zero_reg) && rt.rm().is(zero_reg)) ||          \
    (cond != cc_always && (!rs.is(zero_reg) || !rt.rm().is(zero_reg))))


void MacroAssembler::Branch(int32_t offset, BranchDelaySlot bdslot) {
  DCHECK(IsMipsArchVariant(kMips32r6) ? is_int26(offset) : is_int16(offset));
  BranchShort(offset, bdslot);
}


void MacroAssembler::Branch(int32_t offset, Condition cond, Register rs,
                            const Operand& rt, BranchDelaySlot bdslot) {
  bool is_near = BranchShortCheck(offset, nullptr, cond, rs, rt, bdslot);
  DCHECK(is_near);
  USE(is_near);
}


void MacroAssembler::Branch(Label* L, BranchDelaySlot bdslot) {
  if (L->is_bound()) {
    if (is_near_branch(L)) {
      BranchShort(L, bdslot);
    } else {
      BranchLong(L, bdslot);
    }
  } else {
    if (is_trampoline_emitted()) {
      BranchLong(L, bdslot);
    } else {
      BranchShort(L, bdslot);
    }
  }
}


void MacroAssembler::Branch(Label* L, Condition cond, Register rs,
                            const Operand& rt,
                            BranchDelaySlot bdslot) {
  if (L->is_bound()) {
    if (!BranchShortCheck(0, L, cond, rs, rt, bdslot)) {
      if (cond != cc_always) {
        Label skip;
        Condition neg_cond = NegateCondition(cond);
        BranchShort(&skip, neg_cond, rs, rt);
        BranchLong(L, bdslot);
        bind(&skip);
      } else {
        BranchLong(L, bdslot);
      }
    }
  } else {
    if (is_trampoline_emitted()) {
      if (cond != cc_always) {
        Label skip;
        Condition neg_cond = NegateCondition(cond);
        BranchShort(&skip, neg_cond, rs, rt);
        BranchLong(L, bdslot);
        bind(&skip);
      } else {
        BranchLong(L, bdslot);
      }
    } else {
      BranchShort(L, cond, rs, rt, bdslot);
    }
  }
}


void MacroAssembler::Branch(Label* L,
                            Condition cond,
                            Register rs,
                            Heap::RootListIndex index,
                            BranchDelaySlot bdslot) {
  LoadRoot(at, index);
  Branch(L, cond, rs, Operand(at), bdslot);
}


void MacroAssembler::BranchShortHelper(int16_t offset, Label* L,
                                       BranchDelaySlot bdslot) {
  DCHECK(L == nullptr || offset == 0);
  offset = GetOffset(offset, L, OffsetSize::kOffset16);
  b(offset);

  // Emit a nop in the branch delay slot if required.
  if (bdslot == PROTECT)
    nop();
}


void MacroAssembler::BranchShortHelperR6(int32_t offset, Label* L) {
  DCHECK(L == nullptr || offset == 0);
  offset = GetOffset(offset, L, OffsetSize::kOffset26);
  bc(offset);
}


void MacroAssembler::BranchShort(int32_t offset, BranchDelaySlot bdslot) {
  if (IsMipsArchVariant(kMips32r6) && bdslot == PROTECT) {
    DCHECK(is_int26(offset));
    BranchShortHelperR6(offset, nullptr);
  } else {
    DCHECK(is_int16(offset));
    BranchShortHelper(offset, nullptr, bdslot);
  }
}


void MacroAssembler::BranchShort(Label* L, BranchDelaySlot bdslot) {
  if (IsMipsArchVariant(kMips32r6) && bdslot == PROTECT) {
    BranchShortHelperR6(0, L);
  } else {
    BranchShortHelper(0, L, bdslot);
  }
}


static inline bool IsZero(const Operand& rt) {
  if (rt.is_reg()) {
    return rt.rm().is(zero_reg);
  } else {
    return rt.immediate() == 0;
  }
}


int32_t MacroAssembler::GetOffset(int32_t offset, Label* L, OffsetSize bits) {
  if (L) {
    offset = branch_offset_helper(L, bits) >> 2;
  } else {
    DCHECK(is_intn(offset, bits));
  }
  return offset;
}


Register MacroAssembler::GetRtAsRegisterHelper(const Operand& rt,
                                               Register scratch) {
  Register r2 = no_reg;
  if (rt.is_reg()) {
    r2 = rt.rm_;
  } else {
    r2 = scratch;
    li(r2, rt);
  }

  return r2;
}


bool MacroAssembler::BranchShortHelperR6(int32_t offset, Label* L,
                                         Condition cond, Register rs,
                                         const Operand& rt) {
  DCHECK(L == nullptr || offset == 0);
  Register scratch = rs.is(at) ? t8 : at;
  OffsetSize bits = OffsetSize::kOffset16;

  // Be careful to always use shifted_branch_offset only just before the
  // branch instruction, as the location will be remember for patching the
  // target.
  {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    switch (cond) {
      case cc_always:
        bits = OffsetSize::kOffset26;
        if (!is_near(L, bits)) return false;
        offset = GetOffset(offset, L, bits);
        bc(offset);
        break;
      case eq:
        if (rs.code() == rt.rm_.reg_code) {
          // Pre R6 beq is used here to make the code patchable. Otherwise bc
          // should be used which has no condition field so is not patchable.
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          beq(rs, scratch, offset);
          nop();
        } else if (IsZero(rt)) {
          bits = OffsetSize::kOffset21;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          beqzc(rs, offset);
        } else {
          // We don't want any other register but scratch clobbered.
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          beqc(rs, scratch, offset);
        }
        break;
      case ne:
        if (rs.code() == rt.rm_.reg_code) {
          // Pre R6 bne is used here to make the code patchable. Otherwise we
          // should not generate any instruction.
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          bne(rs, scratch, offset);
          nop();
        } else if (IsZero(rt)) {
          bits = OffsetSize::kOffset21;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          bnezc(rs, offset);
        } else {
          // We don't want any other register but scratch clobbered.
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          bnec(rs, scratch, offset);
        }
        break;

      // Signed comparison.
      case greater:
        // rs > rt
        if (rs.code() == rt.rm_.reg_code) {
          break;  // No code needs to be emitted.
        } else if (rs.is(zero_reg)) {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          bltzc(scratch, offset);
        } else if (IsZero(rt)) {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          bgtzc(rs, offset);
        } else {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          DCHECK(!rs.is(scratch));
          offset = GetOffset(offset, L, bits);
          bltc(scratch, rs, offset);
        }
        break;
      case greater_equal:
        // rs >= rt
        if (rs.code() == rt.rm_.reg_code) {
          bits = OffsetSize::kOffset26;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          bc(offset);
        } else if (rs.is(zero_reg)) {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          blezc(scratch, offset);
        } else if (IsZero(rt)) {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          bgezc(rs, offset);
        } else {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          DCHECK(!rs.is(scratch));
          offset = GetOffset(offset, L, bits);
          bgec(rs, scratch, offset);
        }
        break;
      case less:
        // rs < rt
        if (rs.code() == rt.rm_.reg_code) {
          break;  // No code needs to be emitted.
        } else if (rs.is(zero_reg)) {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          bgtzc(scratch, offset);
        } else if (IsZero(rt)) {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          bltzc(rs, offset);
        } else {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          DCHECK(!rs.is(scratch));
          offset = GetOffset(offset, L, bits);
          bltc(rs, scratch, offset);
        }
        break;
      case less_equal:
        // rs <= rt
        if (rs.code() == rt.rm_.reg_code) {
          bits = OffsetSize::kOffset26;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          bc(offset);
        } else if (rs.is(zero_reg)) {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          bgezc(scratch, offset);
        } else if (IsZero(rt)) {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          blezc(rs, offset);
        } else {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          DCHECK(!rs.is(scratch));
          offset = GetOffset(offset, L, bits);
          bgec(scratch, rs, offset);
        }
        break;

      // Unsigned comparison.
      case Ugreater:
        // rs > rt
        if (rs.code() == rt.rm_.reg_code) {
          break;  // No code needs to be emitted.
        } else if (rs.is(zero_reg)) {
          bits = OffsetSize::kOffset21;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          bnezc(scratch, offset);
        } else if (IsZero(rt)) {
          bits = OffsetSize::kOffset21;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          bnezc(rs, offset);
        } else {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          DCHECK(!rs.is(scratch));
          offset = GetOffset(offset, L, bits);
          bltuc(scratch, rs, offset);
        }
        break;
      case Ugreater_equal:
        // rs >= rt
        if (rs.code() == rt.rm_.reg_code) {
          bits = OffsetSize::kOffset26;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          bc(offset);
        } else if (rs.is(zero_reg)) {
          bits = OffsetSize::kOffset21;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          beqzc(scratch, offset);
        } else if (IsZero(rt)) {
          bits = OffsetSize::kOffset26;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          bc(offset);
        } else {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          DCHECK(!rs.is(scratch));
          offset = GetOffset(offset, L, bits);
          bgeuc(rs, scratch, offset);
        }
        break;
      case Uless:
        // rs < rt
        if (rs.code() == rt.rm_.reg_code) {
          break;  // No code needs to be emitted.
        } else if (rs.is(zero_reg)) {
          bits = OffsetSize::kOffset21;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          bnezc(scratch, offset);
        } else if (IsZero(rt)) {
          break;  // No code needs to be emitted.
        } else {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          DCHECK(!rs.is(scratch));
          offset = GetOffset(offset, L, bits);
          bltuc(rs, scratch, offset);
        }
        break;
      case Uless_equal:
        // rs <= rt
        if (rs.code() == rt.rm_.reg_code) {
          bits = OffsetSize::kOffset26;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          bc(offset);
        } else if (rs.is(zero_reg)) {
          bits = OffsetSize::kOffset26;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset = GetOffset(offset, L, bits);
          bc(offset);
        } else if (IsZero(rt)) {
          bits = OffsetSize::kOffset21;
          if (!is_near(L, bits)) return false;
          offset = GetOffset(offset, L, bits);
          beqzc(rs, offset);
        } else {
          bits = OffsetSize::kOffset16;
          if (!is_near(L, bits)) return false;
          scratch = GetRtAsRegisterHelper(rt, scratch);
          DCHECK(!rs.is(scratch));
          offset = GetOffset(offset, L, bits);
          bgeuc(scratch, rs, offset);
        }
        break;
      default:
        UNREACHABLE();
    }
  }
  CheckTrampolinePoolQuick(1);
  return true;
}


bool MacroAssembler::BranchShortHelper(int16_t offset, Label* L, Condition cond,
                                       Register rs, const Operand& rt,
                                       BranchDelaySlot bdslot) {
  DCHECK(L == nullptr || offset == 0);
  if (!is_near(L, OffsetSize::kOffset16)) return false;

  Register scratch = at;
  int32_t offset32;

  // Be careful to always use shifted_branch_offset only just before the
  // branch instruction, as the location will be remember for patching the
  // target.
  {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    switch (cond) {
      case cc_always:
        offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
        b(offset32);
        break;
      case eq:
        if (IsZero(rt)) {
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          beq(rs, zero_reg, offset32);
        } else {
          // We don't want any other register but scratch clobbered.
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          beq(rs, scratch, offset32);
        }
        break;
      case ne:
        if (IsZero(rt)) {
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          bne(rs, zero_reg, offset32);
        } else {
          // We don't want any other register but scratch clobbered.
          scratch = GetRtAsRegisterHelper(rt, scratch);
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          bne(rs, scratch, offset32);
        }
        break;

      // Signed comparison.
      case greater:
        if (IsZero(rt)) {
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          bgtz(rs, offset32);
        } else {
          Slt(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          bne(scratch, zero_reg, offset32);
        }
        break;
      case greater_equal:
        if (IsZero(rt)) {
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          bgez(rs, offset32);
        } else {
          Slt(scratch, rs, rt);
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          beq(scratch, zero_reg, offset32);
        }
        break;
      case less:
        if (IsZero(rt)) {
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          bltz(rs, offset32);
        } else {
          Slt(scratch, rs, rt);
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          bne(scratch, zero_reg, offset32);
        }
        break;
      case less_equal:
        if (IsZero(rt)) {
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          blez(rs, offset32);
        } else {
          Slt(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          beq(scratch, zero_reg, offset32);
        }
        break;

      // Unsigned comparison.
      case Ugreater:
        if (IsZero(rt)) {
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          bne(rs, zero_reg, offset32);
        } else {
          Sltu(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          bne(scratch, zero_reg, offset32);
        }
        break;
      case Ugreater_equal:
        if (IsZero(rt)) {
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          b(offset32);
        } else {
          Sltu(scratch, rs, rt);
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          beq(scratch, zero_reg, offset32);
        }
        break;
      case Uless:
        if (IsZero(rt)) {
          return true;  // No code needs to be emitted.
        } else {
          Sltu(scratch, rs, rt);
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          bne(scratch, zero_reg, offset32);
        }
        break;
      case Uless_equal:
        if (IsZero(rt)) {
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          beq(rs, zero_reg, offset32);
        } else {
          Sltu(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
          offset32 = GetOffset(offset, L, OffsetSize::kOffset16);
          beq(scratch, zero_reg, offset32);
        }
        break;
      default:
        UNREACHABLE();
    }
  }
  // Emit a nop in the branch delay slot if required.
  if (bdslot == PROTECT)
    nop();

  return true;
}


bool MacroAssembler::BranchShortCheck(int32_t offset, Label* L, Condition cond,
                                      Register rs, const Operand& rt,
                                      BranchDelaySlot bdslot) {
  BRANCH_ARGS_CHECK(cond, rs, rt);

  if (!L) {
    if (IsMipsArchVariant(kMips32r6) && bdslot == PROTECT) {
      DCHECK(is_int26(offset));
      return BranchShortHelperR6(offset, nullptr, cond, rs, rt);
    } else {
      DCHECK(is_int16(offset));
      return BranchShortHelper(offset, nullptr, cond, rs, rt, bdslot);
    }
  } else {
    DCHECK(offset == 0);
    if (IsMipsArchVariant(kMips32r6) && bdslot == PROTECT) {
      return BranchShortHelperR6(0, L, cond, rs, rt);
    } else {
      return BranchShortHelper(0, L, cond, rs, rt, bdslot);
    }
  }
  return false;
}


void MacroAssembler::BranchShort(int32_t offset, Condition cond, Register rs,
                                 const Operand& rt, BranchDelaySlot bdslot) {
  BranchShortCheck(offset, nullptr, cond, rs, rt, bdslot);
}


void MacroAssembler::BranchShort(Label* L, Condition cond, Register rs,
                                 const Operand& rt, BranchDelaySlot bdslot) {
  BranchShortCheck(0, L, cond, rs, rt, bdslot);
}


void MacroAssembler::BranchAndLink(int32_t offset, BranchDelaySlot bdslot) {
  BranchAndLinkShort(offset, bdslot);
}


void MacroAssembler::BranchAndLink(int32_t offset, Condition cond, Register rs,
                                   const Operand& rt, BranchDelaySlot bdslot) {
  bool is_near = BranchAndLinkShortCheck(offset, nullptr, cond, rs, rt, bdslot);
  DCHECK(is_near);
  USE(is_near);
}


void MacroAssembler::BranchAndLink(Label* L, BranchDelaySlot bdslot) {
  if (L->is_bound()) {
    if (is_near_branch(L)) {
      BranchAndLinkShort(L, bdslot);
    } else {
      BranchAndLinkLong(L, bdslot);
    }
  } else {
    if (is_trampoline_emitted()) {
      BranchAndLinkLong(L, bdslot);
    } else {
      BranchAndLinkShort(L, bdslot);
    }
  }
}


void MacroAssembler::BranchAndLink(Label* L, Condition cond, Register rs,
                                   const Operand& rt,
                                   BranchDelaySlot bdslot) {
  if (L->is_bound()) {
    if (!BranchAndLinkShortCheck(0, L, cond, rs, rt, bdslot)) {
      Label skip;
      Condition neg_cond = NegateCondition(cond);
      BranchShort(&skip, neg_cond, rs, rt);
      BranchAndLinkLong(L, bdslot);
      bind(&skip);
    }
  } else {
    if (is_trampoline_emitted()) {
      Label skip;
      Condition neg_cond = NegateCondition(cond);
      BranchShort(&skip, neg_cond, rs, rt);
      BranchAndLinkLong(L, bdslot);
      bind(&skip);
    } else {
      BranchAndLinkShortCheck(0, L, cond, rs, rt, bdslot);
    }
  }
}


void MacroAssembler::BranchAndLinkShortHelper(int16_t offset, Label* L,
                                              BranchDelaySlot bdslot) {
  DCHECK(L == nullptr || offset == 0);
  offset = GetOffset(offset, L, OffsetSize::kOffset16);
  bal(offset);

  // Emit a nop in the branch delay slot if required.
  if (bdslot == PROTECT)
    nop();
}


void MacroAssembler::BranchAndLinkShortHelperR6(int32_t offset, Label* L) {
  DCHECK(L == nullptr || offset == 0);
  offset = GetOffset(offset, L, OffsetSize::kOffset26);
  balc(offset);
}


void MacroAssembler::BranchAndLinkShort(int32_t offset,
                                        BranchDelaySlot bdslot) {
  if (IsMipsArchVariant(kMips32r6) && bdslot == PROTECT) {
    DCHECK(is_int26(offset));
    BranchAndLinkShortHelperR6(offset, nullptr);
  } else {
    DCHECK(is_int16(offset));
    BranchAndLinkShortHelper(offset, nullptr, bdslot);
  }
}


void MacroAssembler::BranchAndLinkShort(Label* L, BranchDelaySlot bdslot) {
  if (IsMipsArchVariant(kMips32r6) && bdslot == PROTECT) {
    BranchAndLinkShortHelperR6(0, L);
  } else {
    BranchAndLinkShortHelper(0, L, bdslot);
  }
}


bool MacroAssembler::BranchAndLinkShortHelperR6(int32_t offset, Label* L,
                                                Condition cond, Register rs,
                                                const Operand& rt) {
  DCHECK(L == nullptr || offset == 0);
  Register scratch = rs.is(at) ? t8 : at;
  OffsetSize bits = OffsetSize::kOffset16;

  BlockTrampolinePoolScope block_trampoline_pool(this);
  DCHECK((cond == cc_always && is_int26(offset)) || is_int16(offset));
  switch (cond) {
    case cc_always:
      bits = OffsetSize::kOffset26;
      if (!is_near(L, bits)) return false;
      offset = GetOffset(offset, L, bits);
      balc(offset);
      break;
    case eq:
      if (!is_near(L, bits)) return false;
      Subu(scratch, rs, rt);
      offset = GetOffset(offset, L, bits);
      beqzalc(scratch, offset);
      break;
    case ne:
      if (!is_near(L, bits)) return false;
      Subu(scratch, rs, rt);
      offset = GetOffset(offset, L, bits);
      bnezalc(scratch, offset);
      break;

    // Signed comparison.
    case greater:
      // rs > rt
      if (rs.code() == rt.rm_.reg_code) {
        break;  // No code needs to be emitted.
      } else if (rs.is(zero_reg)) {
        if (!is_near(L, bits)) return false;
        scratch = GetRtAsRegisterHelper(rt, scratch);
        offset = GetOffset(offset, L, bits);
        bltzalc(scratch, offset);
      } else if (IsZero(rt)) {
        if (!is_near(L, bits)) return false;
        offset = GetOffset(offset, L, bits);
        bgtzalc(rs, offset);
      } else {
        if (!is_near(L, bits)) return false;
        Slt(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
        offset = GetOffset(offset, L, bits);
        bnezalc(scratch, offset);
      }
      break;
    case greater_equal:
      // rs >= rt
      if (rs.code() == rt.rm_.reg_code) {
        bits = OffsetSize::kOffset26;
        if (!is_near(L, bits)) return false;
        offset = GetOffset(offset, L, bits);
        balc(offset);
      } else if (rs.is(zero_reg)) {
        if (!is_near(L, bits)) return false;
        scratch = GetRtAsRegisterHelper(rt, scratch);
        offset = GetOffset(offset, L, bits);
        blezalc(scratch, offset);
      } else if (IsZero(rt)) {
        if (!is_near(L, bits)) return false;
        offset = GetOffset(offset, L, bits);
        bgezalc(rs, offset);
      } else {
        if (!is_near(L, bits)) return false;
        Slt(scratch, rs, rt);
        offset = GetOffset(offset, L, bits);
        beqzalc(scratch, offset);
      }
      break;
    case less:
      // rs < rt
      if (rs.code() == rt.rm_.reg_code) {
        break;  // No code needs to be emitted.
      } else if (rs.is(zero_reg)) {
        if (!is_near(L, bits)) return false;
        scratch = GetRtAsRegisterHelper(rt, scratch);
        offset = GetOffset(offset, L, bits);
        bgtzalc(scratch, offset);
      } else if (IsZero(rt)) {
        if (!is_near(L, bits)) return false;
        offset = GetOffset(offset, L, bits);
        bltzalc(rs, offset);
      } else {
        if (!is_near(L, bits)) return false;
        Slt(scratch, rs, rt);
        offset = GetOffset(offset, L, bits);
        bnezalc(scratch, offset);
      }
      break;
    case less_equal:
      // rs <= r2
      if (rs.code() == rt.rm_.reg_code) {
        bits = OffsetSize::kOffset26;
        if (!is_near(L, bits)) return false;
        offset = GetOffset(offset, L, bits);
        balc(offset);
      } else if (rs.is(zero_reg)) {
        if (!is_near(L, bits)) return false;
        scratch = GetRtAsRegisterHelper(rt, scratch);
        offset = GetOffset(offset, L, bits);
        bgezalc(scratch, offset);
      } else if (IsZero(rt)) {
        if (!is_near(L, bits)) return false;
        offset = GetOffset(offset, L, bits);
        blezalc(rs, offset);
      } else {
        if (!is_near(L, bits)) return false;
        Slt(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
        offset = GetOffset(offset, L, bits);
        beqzalc(scratch, offset);
      }
      break;


    // Unsigned comparison.
    case Ugreater:
      // rs > r2
      if (!is_near(L, bits)) return false;
      Sltu(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
      offset = GetOffset(offset, L, bits);
      bnezalc(scratch, offset);
      break;
    case Ugreater_equal:
      // rs >= r2
      if (!is_near(L, bits)) return false;
      Sltu(scratch, rs, rt);
      offset = GetOffset(offset, L, bits);
      beqzalc(scratch, offset);
      break;
    case Uless:
      // rs < r2
      if (!is_near(L, bits)) return false;
      Sltu(scratch, rs, rt);
      offset = GetOffset(offset, L, bits);
      bnezalc(scratch, offset);
      break;
    case Uless_equal:
      // rs <= r2
      if (!is_near(L, bits)) return false;
      Sltu(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
      offset = GetOffset(offset, L, bits);
      beqzalc(scratch, offset);
      break;
    default:
      UNREACHABLE();
  }
  return true;
}


// Pre r6 we need to use a bgezal or bltzal, but they can't be used directly
// with the slt instructions. We could use sub or add instead but we would miss
// overflow cases, so we keep slt and add an intermediate third instruction.
bool MacroAssembler::BranchAndLinkShortHelper(int16_t offset, Label* L,
                                              Condition cond, Register rs,
                                              const Operand& rt,
                                              BranchDelaySlot bdslot) {
  DCHECK(L == nullptr || offset == 0);
  if (!is_near(L, OffsetSize::kOffset16)) return false;

  Register scratch = t8;
  BlockTrampolinePoolScope block_trampoline_pool(this);

  switch (cond) {
    case cc_always:
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bal(offset);
      break;
    case eq:
      bne(rs, GetRtAsRegisterHelper(rt, scratch), 2);
      nop();
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bal(offset);
      break;
    case ne:
      beq(rs, GetRtAsRegisterHelper(rt, scratch), 2);
      nop();
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bal(offset);
      break;

    // Signed comparison.
    case greater:
      Slt(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
      addiu(scratch, scratch, -1);
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bgezal(scratch, offset);
      break;
    case greater_equal:
      Slt(scratch, rs, rt);
      addiu(scratch, scratch, -1);
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bltzal(scratch, offset);
      break;
    case less:
      Slt(scratch, rs, rt);
      addiu(scratch, scratch, -1);
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bgezal(scratch, offset);
      break;
    case less_equal:
      Slt(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
      addiu(scratch, scratch, -1);
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bltzal(scratch, offset);
      break;

    // Unsigned comparison.
    case Ugreater:
      Sltu(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
      addiu(scratch, scratch, -1);
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bgezal(scratch, offset);
      break;
    case Ugreater_equal:
      Sltu(scratch, rs, rt);
      addiu(scratch, scratch, -1);
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bltzal(scratch, offset);
      break;
    case Uless:
      Sltu(scratch, rs, rt);
      addiu(scratch, scratch, -1);
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bgezal(scratch, offset);
      break;
    case Uless_equal:
      Sltu(scratch, GetRtAsRegisterHelper(rt, scratch), rs);
      addiu(scratch, scratch, -1);
      offset = GetOffset(offset, L, OffsetSize::kOffset16);
      bltzal(scratch, offset);
      break;

    default:
      UNREACHABLE();
  }

  // Emit a nop in the branch delay slot if required.
  if (bdslot == PROTECT)
    nop();

  return true;
}


bool MacroAssembler::BranchAndLinkShortCheck(int32_t offset, Label* L,
                                             Condition cond, Register rs,
                                             const Operand& rt,
                                             BranchDelaySlot bdslot) {
  BRANCH_ARGS_CHECK(cond, rs, rt);

  if (!L) {
    if (IsMipsArchVariant(kMips32r6) && bdslot == PROTECT) {
      DCHECK(is_int26(offset));
      return BranchAndLinkShortHelperR6(offset, nullptr, cond, rs, rt);
    } else {
      DCHECK(is_int16(offset));
      return BranchAndLinkShortHelper(offset, nullptr, cond, rs, rt, bdslot);
    }
  } else {
    DCHECK(offset == 0);
    if (IsMipsArchVariant(kMips32r6) && bdslot == PROTECT) {
      return BranchAndLinkShortHelperR6(0, L, cond, rs, rt);
    } else {
      return BranchAndLinkShortHelper(0, L, cond, rs, rt, bdslot);
    }
  }
  return false;
}


void MacroAssembler::Jump(Register target,
                          Condition cond,
                          Register rs,
                          const Operand& rt,
                          BranchDelaySlot bd) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  if (cond == cc_always) {
    jr(target);
  } else {
    BRANCH_ARGS_CHECK(cond, rs, rt);
    Branch(2, NegateCondition(cond), rs, rt);
    jr(target);
  }
  // Emit a nop in the branch delay slot if required.
  if (bd == PROTECT)
    nop();
}


void MacroAssembler::Jump(intptr_t target,
                          RelocInfo::Mode rmode,
                          Condition cond,
                          Register rs,
                          const Operand& rt,
                          BranchDelaySlot bd) {
  Label skip;
  if (cond != cc_always) {
    Branch(USE_DELAY_SLOT, &skip, NegateCondition(cond), rs, rt);
  }
  // The first instruction of 'li' may be placed in the delay slot.
  // This is not an issue, t9 is expected to be clobbered anyway.
  li(t9, Operand(target, rmode));
  Jump(t9, al, zero_reg, Operand(zero_reg), bd);
  bind(&skip);
}


void MacroAssembler::Jump(Address target,
                          RelocInfo::Mode rmode,
                          Condition cond,
                          Register rs,
                          const Operand& rt,
                          BranchDelaySlot bd) {
  DCHECK(!RelocInfo::IsCodeTarget(rmode));
  Jump(reinterpret_cast<intptr_t>(target), rmode, cond, rs, rt, bd);
}


void MacroAssembler::Jump(Handle<Code> code,
                          RelocInfo::Mode rmode,
                          Condition cond,
                          Register rs,
                          const Operand& rt,
                          BranchDelaySlot bd) {
  DCHECK(RelocInfo::IsCodeTarget(rmode));
  AllowDeferredHandleDereference embedding_raw_address;
  Jump(reinterpret_cast<intptr_t>(code.location()), rmode, cond, rs, rt, bd);
}


int MacroAssembler::CallSize(Register target,
                             Condition cond,
                             Register rs,
                             const Operand& rt,
                             BranchDelaySlot bd) {
  int size = 0;

  if (cond == cc_always) {
    size += 1;
  } else {
    size += 3;
  }

  if (bd == PROTECT)
    size += 1;

  return size * kInstrSize;
}


// Note: To call gcc-compiled C code on mips, you must call thru t9.
void MacroAssembler::Call(Register target,
                          Condition cond,
                          Register rs,
                          const Operand& rt,
                          BranchDelaySlot bd) {
#ifdef DEBUG
  int size = IsPrevInstrCompactBranch() ? kInstrSize : 0;
#endif

  BlockTrampolinePoolScope block_trampoline_pool(this);
  Label start;
  bind(&start);
  if (cond == cc_always) {
    jalr(target);
  } else {
    BRANCH_ARGS_CHECK(cond, rs, rt);
    Branch(2, NegateCondition(cond), rs, rt);
    jalr(target);
  }
  // Emit a nop in the branch delay slot if required.
  if (bd == PROTECT)
    nop();

#ifdef DEBUG
  CHECK_EQ(size + CallSize(target, cond, rs, rt, bd),
           SizeOfCodeGeneratedSince(&start));
#endif
}


int MacroAssembler::CallSize(Address target,
                             RelocInfo::Mode rmode,
                             Condition cond,
                             Register rs,
                             const Operand& rt,
                             BranchDelaySlot bd) {
  int size = CallSize(t9, cond, rs, rt, bd);
  return size + 2 * kInstrSize;
}


void MacroAssembler::Call(Address target,
                          RelocInfo::Mode rmode,
                          Condition cond,
                          Register rs,
                          const Operand& rt,
                          BranchDelaySlot bd) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Label start;
  bind(&start);
  int32_t target_int = reinterpret_cast<int32_t>(target);
  // Must record previous source positions before the
  // li() generates a new code target.
  positions_recorder()->WriteRecordedPositions();
  li(t9, Operand(target_int, rmode), CONSTANT_SIZE);
  Call(t9, cond, rs, rt, bd);
  DCHECK_EQ(CallSize(target, rmode, cond, rs, rt, bd),
            SizeOfCodeGeneratedSince(&start));
}


int MacroAssembler::CallSize(Handle<Code> code,
                             RelocInfo::Mode rmode,
                             TypeFeedbackId ast_id,
                             Condition cond,
                             Register rs,
                             const Operand& rt,
                             BranchDelaySlot bd) {
  AllowDeferredHandleDereference using_raw_address;
  return CallSize(reinterpret_cast<Address>(code.location()),
      rmode, cond, rs, rt, bd);
}


void MacroAssembler::Call(Handle<Code> code,
                          RelocInfo::Mode rmode,
                          TypeFeedbackId ast_id,
                          Condition cond,
                          Register rs,
                          const Operand& rt,
                          BranchDelaySlot bd) {
  BlockTrampolinePoolScope block_trampoline_pool(this);
  Label start;
  bind(&start);
  DCHECK(RelocInfo::IsCodeTarget(rmode));
  if (rmode == RelocInfo::CODE_TARGET && !ast_id.IsNone()) {
    SetRecordedAstId(ast_id);
    rmode = RelocInfo::CODE_TARGET_WITH_ID;
  }
  AllowDeferredHandleDereference embedding_raw_address;
  Call(reinterpret_cast<Address>(code.location()), rmode, cond, rs, rt, bd);
  DCHECK_EQ(CallSize(code, rmode, ast_id, cond, rs, rt, bd),
            SizeOfCodeGeneratedSince(&start));
}


void MacroAssembler::Ret(Condition cond,
                         Register rs,
                         const Operand& rt,
                         BranchDelaySlot bd) {
  Jump(ra, cond, rs, rt, bd);
}


void MacroAssembler::BranchLong(Label* L, BranchDelaySlot bdslot) {
  if (IsMipsArchVariant(kMips32r6) && bdslot == PROTECT &&
      (!L->is_bound() || is_near_r6(L))) {
    BranchShortHelperR6(0, L);
  } else {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    uint32_t imm32;
    imm32 = jump_address(L);
    {
      BlockGrowBufferScope block_buf_growth(this);
      // Buffer growth (and relocation) must be blocked for internal references
      // until associated instructions are emitted and available to be patched.
      RecordRelocInfo(RelocInfo::INTERNAL_REFERENCE_ENCODED);
      lui(at, (imm32 & kHiMask) >> kLuiShift);
      ori(at, at, (imm32 & kImm16Mask));
    }
    jr(at);

    // Emit a nop in the branch delay slot if required.
    if (bdslot == PROTECT) nop();
  }
}


void MacroAssembler::BranchAndLinkLong(Label* L, BranchDelaySlot bdslot) {
  if (IsMipsArchVariant(kMips32r6) && bdslot == PROTECT &&
      (!L->is_bound() || is_near_r6(L))) {
    BranchAndLinkShortHelperR6(0, L);
  } else {
    BlockTrampolinePoolScope block_trampoline_pool(this);
    uint32_t imm32;
    imm32 = jump_address(L);
    {
      BlockGrowBufferScope block_buf_growth(this);
      // Buffer growth (and relocation) must be blocked for internal references
      // until associated instructions are emitted and available to be patched.
      RecordRelocInfo(RelocInfo::INTERNAL_REFERENCE_ENCODED);
      lui(at, (imm32 & kHiMask) >> kLuiShift);
      ori(at, at, (imm32 & kImm16Mask));
    }
    jalr(at);

    // Emit a nop in the branch delay slot if required.
    if (bdslot == PROTECT) nop();
  }
}


void MacroAssembler::DropAndRet(int drop) {
  DCHECK(is_int16(drop * kPointerSize));
  Ret(USE_DELAY_SLOT);
  addiu(sp, sp, drop * kPointerSize);
}

void MacroAssembler::DropAndRet(int drop,
                                Condition cond,
                                Register r1,
                                const Operand& r2) {
  // Both Drop and Ret need to be conditional.
  Label skip;
  if (cond != cc_always) {
    Branch(&skip, NegateCondition(cond), r1, r2);
  }

  Drop(drop);
  Ret();

  if (cond != cc_always) {
    bind(&skip);
  }
}


void MacroAssembler::Drop(int count,
                          Condition cond,
                          Register reg,
                          const Operand& op) {
  if (count <= 0) {
    return;
  }

  Label skip;

  if (cond != al) {
     Branch(&skip, NegateCondition(cond), reg, op);
  }

  Addu(sp, sp, Operand(count * kPointerSize));

  if (cond != al) {
    bind(&skip);
  }
}



void MacroAssembler::Swap(Register reg1,
                          Register reg2,
                          Register scratch) {
  if (scratch.is(no_reg)) {
    Xor(reg1, reg1, Operand(reg2));
    Xor(reg2, reg2, Operand(reg1));
    Xor(reg1, reg1, Operand(reg2));
  } else {
    mov(scratch, reg1);
    mov(reg1, reg2);
    mov(reg2, scratch);
  }
}


void MacroAssembler::Call(Label* target) {
  BranchAndLink(target);
}


void MacroAssembler::Push(Handle<Object> handle) {
  li(at, Operand(handle));
  push(at);
}


void MacroAssembler::DebugBreak() {
  PrepareCEntryArgs(0);
  PrepareCEntryFunction(
      ExternalReference(Runtime::kHandleDebuggerStatement, isolate()));
  CEntryStub ces(isolate(), 1);
  DCHECK(AllowThisStubCall(&ces));
  Call(ces.GetCode(), RelocInfo::DEBUGGER_STATEMENT);
}


// ---------------------------------------------------------------------------
// Exception handling.

void MacroAssembler::PushStackHandler() {
  // Adjust this code if not the case.
  STATIC_ASSERT(StackHandlerConstants::kSize == 1 * kPointerSize);
  STATIC_ASSERT(StackHandlerConstants::kNextOffset == 0 * kPointerSize);

  // Link the current handler as the next handler.
  li(t2, Operand(ExternalReference(Isolate::kHandlerAddress, isolate())));
  lw(t1, MemOperand(t2));
  push(t1);

  // Set this new handler as the current one.
  sw(sp, MemOperand(t2));
}


void MacroAssembler::PopStackHandler() {
  STATIC_ASSERT(StackHandlerConstants::kNextOffset == 0);
  pop(a1);
  Addu(sp, sp, Operand(StackHandlerConstants::kSize - kPointerSize));
  li(at, Operand(ExternalReference(Isolate::kHandlerAddress, isolate())));
  sw(a1, MemOperand(at));
}


void MacroAssembler::Allocate(int object_size,
                              Register result,
                              Register scratch1,
                              Register scratch2,
                              Label* gc_required,
                              AllocationFlags flags) {
  DCHECK(object_size <= Page::kMaxRegularHeapObjectSize);
  if (!FLAG_inline_new) {
    if (emit_debug_code()) {
      // Trash the registers to simulate an allocation failure.
      li(result, 0x7091);
      li(scratch1, 0x7191);
      li(scratch2, 0x7291);
    }
    jmp(gc_required);
    return;
  }

  DCHECK(!AreAliased(result, scratch1, scratch2, t9));

  // Make object size into bytes.
  if ((flags & SIZE_IN_WORDS) != 0) {
    object_size *= kPointerSize;
  }
  DCHECK_EQ(0, object_size & kObjectAlignmentMask);

  // Check relative positions of allocation top and limit addresses.
  // ARM adds additional checks to make sure the ldm instruction can be
  // used. On MIPS we don't have ldm so we don't need additional checks either.
  ExternalReference allocation_top =
      AllocationUtils::GetAllocationTopReference(isolate(), flags);
  ExternalReference allocation_limit =
      AllocationUtils::GetAllocationLimitReference(isolate(), flags);

  intptr_t top = reinterpret_cast<intptr_t>(allocation_top.address());
  intptr_t limit = reinterpret_cast<intptr_t>(allocation_limit.address());
  DCHECK((limit - top) == kPointerSize);

  // Set up allocation top address and allocation limit registers.
  Register top_address = scratch1;
  // This code stores a temporary value in t9.
  Register alloc_limit = t9;
  Register result_end = scratch2;
  li(top_address, Operand(allocation_top));

  if ((flags & RESULT_CONTAINS_TOP) == 0) {
    // Load allocation top into result and allocation limit into alloc_limit.
    lw(result, MemOperand(top_address));
    lw(alloc_limit, MemOperand(top_address, kPointerSize));
  } else {
    if (emit_debug_code()) {
      // Assert that result actually contains top on entry.
      lw(alloc_limit, MemOperand(top_address));
      Check(eq, kUnexpectedAllocationTop, result, Operand(alloc_limit));
    }
    // Load allocation limit. Result already contains allocation top.
    lw(alloc_limit, MemOperand(top_address, limit - top));
  }

  if ((flags & DOUBLE_ALIGNMENT) != 0) {
    // Align the next allocation. Storing the filler map without checking top is
    // safe in new-space because the limit of the heap is aligned there.
    DCHECK(kPointerAlignment * 2 == kDoubleAlignment);
    And(result_end, result, Operand(kDoubleAlignmentMask));
    Label aligned;
    Branch(&aligned, eq, result_end, Operand(zero_reg));
    if ((flags & PRETENURE) != 0) {
      Branch(gc_required, Ugreater_equal, result, Operand(alloc_limit));
    }
    li(result_end, Operand(isolate()->factory()->one_pointer_filler_map()));
    sw(result_end, MemOperand(result));
    Addu(result, result, Operand(kDoubleSize / 2));
    bind(&aligned);
  }

  // Calculate new top and bail out if new space is exhausted. Use result
  // to calculate the new top.
  Addu(result_end, result, Operand(object_size));
  Branch(gc_required, Ugreater, result_end, Operand(alloc_limit));
  sw(result_end, MemOperand(top_address));

  // Tag object if requested.
  if ((flags & TAG_OBJECT) != 0) {
    Addu(result, result, Operand(kHeapObjectTag));
  }
}


void MacroAssembler::Allocate(Register object_size, Register result,
                              Register result_end, Register scratch,
                              Label* gc_required, AllocationFlags flags) {
  if (!FLAG_inline_new) {
    if (emit_debug_code()) {
      // Trash the registers to simulate an allocation failure.
      li(result, 0x7091);
      li(scratch, 0x7191);
      li(result_end, 0x7291);
    }
    jmp(gc_required);
    return;
  }

  // |object_size| and |result_end| may overlap if the DOUBLE_ALIGNMENT flag
  // is not specified. Other registers must not overlap.
  DCHECK(!AreAliased(object_size, result, scratch, t9));
  DCHECK(!AreAliased(result_end, result, scratch, t9));
  DCHECK((flags & DOUBLE_ALIGNMENT) == 0 || !object_size.is(result_end));

  // Check relative positions of allocation top and limit addresses.
  // ARM adds additional checks to make sure the ldm instruction can be
  // used. On MIPS we don't have ldm so we don't need additional checks either.
  ExternalReference allocation_top =
      AllocationUtils::GetAllocationTopReference(isolate(), flags);
  ExternalReference allocation_limit =
      AllocationUtils::GetAllocationLimitReference(isolate(), flags);
  intptr_t top = reinterpret_cast<intptr_t>(allocation_top.address());
  intptr_t limit = reinterpret_cast<intptr_t>(allocation_limit.address());
  DCHECK((limit - top) == kPointerSize);

  // Set up allocation top address and allocation limit registers.
  Register top_address = scratch;
  // This code stores a temporary value in t9.
  Register alloc_limit = t9;
  li(top_address, Operand(allocation_top));

  if ((flags & RESULT_CONTAINS_TOP) == 0) {
    // Load allocation top into result and allocation limit into alloc_limit.
    lw(result, MemOperand(top_address));
    lw(alloc_limit, MemOperand(top_address, kPointerSize));
  } else {
    if (emit_debug_code()) {
      // Assert that result actually contains top on entry.
      lw(alloc_limit, MemOperand(top_address));
      Check(eq, kUnexpectedAllocationTop, result, Operand(alloc_limit));
    }
    // Load allocation limit. Result already contains allocation top.
    lw(alloc_limit, MemOperand(top_address, limit - top));
  }

  if ((flags & DOUBLE_ALIGNMENT) != 0) {
    // Align the next allocation. Storing the filler map without checking top is
    // safe in new-space because the limit of the heap is aligned there.
    DCHECK(kPointerAlignment * 2 == kDoubleAlignment);
    And(result_end, result, Operand(kDoubleAlignmentMask));
    Label aligned;
    Branch(&aligned, eq, result_end, Operand(zero_reg));
    if ((flags & PRETENURE) != 0) {
      Branch(gc_required, Ugreater_equal, result, Operand(alloc_limit));
    }
    li(result_end, Operand(isolate()->factory()->one_pointer_filler_map()));
    sw(result_end, MemOperand(result));
    Addu(result, result, Operand(kDoubleSize / 2));
    bind(&aligned);
  }

  // Calculate new top and bail out if new space is exhausted. Use result
  // to calculate the new top. Object size may be in words so a shift is
  // required to get the number of bytes.
  if ((flags & SIZE_IN_WORDS) != 0) {
    Lsa(result_end, result, object_size, kPointerSizeLog2);
  } else {
    Addu(result_end, result, Operand(object_size));
  }
  Branch(gc_required, Ugreater, result_end, Operand(alloc_limit));

  // Update allocation top. result temporarily holds the new top.
  if (emit_debug_code()) {
    And(alloc_limit, result_end, Operand(kObjectAlignmentMask));
    Check(eq, kUnalignedAllocationInNewSpace, alloc_limit, Operand(zero_reg));
  }
  sw(result_end, MemOperand(top_address));

  // Tag object if requested.
  if ((flags & TAG_OBJECT) != 0) {
    Addu(result, result, Operand(kHeapObjectTag));
  }
}


void MacroAssembler::AllocateTwoByteString(Register result,
                                           Register length,
                                           Register scratch1,
                                           Register scratch2,
                                           Register scratch3,
                                           Label* gc_required) {
  // Calculate the number of bytes needed for the characters in the string while
  // observing object alignment.
  DCHECK((SeqTwoByteString::kHeaderSize & kObjectAlignmentMask) == 0);
  sll(scratch1, length, 1);  // Length in bytes, not chars.
  addiu(scratch1, scratch1,
       kObjectAlignmentMask + SeqTwoByteString::kHeaderSize);
  And(scratch1, scratch1, Operand(~kObjectAlignmentMask));

  // Allocate two-byte string in new space.
  Allocate(scratch1,
           result,
           scratch2,
           scratch3,
           gc_required,
           TAG_OBJECT);

  // Set the map, length and hash field.
  InitializeNewString(result,
                      length,
                      Heap::kStringMapRootIndex,
                      scratch1,
                      scratch2);
}


void MacroAssembler::AllocateOneByteString(Register result, Register length,
                                           Register scratch1, Register scratch2,
                                           Register scratch3,
                                           Label* gc_required) {
  // Calculate the number of bytes needed for the characters in the string
  // while observing object alignment.
  DCHECK((SeqOneByteString::kHeaderSize & kObjectAlignmentMask) == 0);
  DCHECK(kCharSize == 1);
  addiu(scratch1, length, kObjectAlignmentMask + SeqOneByteString::kHeaderSize);
  And(scratch1, scratch1, Operand(~kObjectAlignmentMask));

  // Allocate one-byte string in new space.
  Allocate(scratch1,
           result,
           scratch2,
           scratch3,
           gc_required,
           TAG_OBJECT);

  // Set the map, length and hash field.
  InitializeNewString(result, length, Heap::kOneByteStringMapRootIndex,
                      scratch1, scratch2);
}


void MacroAssembler::AllocateTwoByteConsString(Register result,
                                               Register length,
                                               Register scratch1,
                                               Register scratch2,
                                               Label* gc_required) {
  Allocate(ConsString::kSize, result, scratch1, scratch2, gc_required,
           TAG_OBJECT);
  InitializeNewString(result,
                      length,
                      Heap::kConsStringMapRootIndex,
                      scratch1,
                      scratch2);
}


void MacroAssembler::AllocateOneByteConsString(Register result, Register length,
                                               Register scratch1,
                                               Register scratch2,
                                               Label* gc_required) {
  Allocate(ConsString::kSize,
           result,
           scratch1,
           scratch2,
           gc_required,
           TAG_OBJECT);

  InitializeNewString(result, length, Heap::kConsOneByteStringMapRootIndex,
                      scratch1, scratch2);
}


void MacroAssembler::AllocateTwoByteSlicedString(Register result,
                                                 Register length,
                                                 Register scratch1,
                                                 Register scratch2,
                                                 Label* gc_required) {
  Allocate(SlicedString::kSize, result, scratch1, scratch2, gc_required,
           TAG_OBJECT);

  InitializeNewString(result,
                      length,
                      Heap::kSlicedStringMapRootIndex,
                      scratch1,
                      scratch2);
}


void MacroAssembler::AllocateOneByteSlicedString(Register result,
                                                 Register length,
                                                 Register scratch1,
                                                 Register scratch2,
                                                 Label* gc_required) {
  Allocate(SlicedString::kSize, result, scratch1, scratch2, gc_required,
           TAG_OBJECT);

  InitializeNewString(result, length, Heap::kSlicedOneByteStringMapRootIndex,
                      scratch1, scratch2);
}


void MacroAssembler::JumpIfNotUniqueNameInstanceType(Register reg,
                                                     Label* not_unique_name) {
  STATIC_ASSERT(kInternalizedTag == 0 && kStringTag == 0);
  Label succeed;
  And(at, reg, Operand(kIsNotStringMask | kIsNotInternalizedMask));
  Branch(&succeed, eq, at, Operand(zero_reg));
  Branch(not_unique_name, ne, reg, Operand(SYMBOL_TYPE));

  bind(&succeed);
}


// Allocates a heap number or jumps to the label if the young space is full and
// a scavenge is needed.
void MacroAssembler::AllocateHeapNumber(Register result,
                                        Register scratch1,
                                        Register scratch2,
                                        Register heap_number_map,
                                        Label* need_gc,
                                        TaggingMode tagging_mode,
                                        MutableMode mode) {
  // Allocate an object in the heap for the heap number and tag it as a heap
  // object.
  Allocate(HeapNumber::kSize, result, scratch1, scratch2, need_gc,
           tagging_mode == TAG_RESULT ? TAG_OBJECT : NO_ALLOCATION_FLAGS);

  Heap::RootListIndex map_index = mode == MUTABLE
      ? Heap::kMutableHeapNumberMapRootIndex
      : Heap::kHeapNumberMapRootIndex;
  AssertIsRoot(heap_number_map, map_index);

  // Store heap number map in the allocated object.
  if (tagging_mode == TAG_RESULT) {
    sw(heap_number_map, FieldMemOperand(result, HeapObject::kMapOffset));
  } else {
    sw(heap_number_map, MemOperand(result, HeapObject::kMapOffset));
  }
}


void MacroAssembler::AllocateHeapNumberWithValue(Register result,
                                                 FPURegister value,
                                                 Register scratch1,
                                                 Register scratch2,
                                                 Label* gc_required) {
  LoadRoot(t8, Heap::kHeapNumberMapRootIndex);
  AllocateHeapNumber(result, scratch1, scratch2, t8, gc_required);
  sdc1(value, FieldMemOperand(result, HeapNumber::kValueOffset));
}


void MacroAssembler::AllocateJSValue(Register result, Register constructor,
                                     Register value, Register scratch1,
                                     Register scratch2, Label* gc_required) {
  DCHECK(!result.is(constructor));
  DCHECK(!result.is(scratch1));
  DCHECK(!result.is(scratch2));
  DCHECK(!result.is(value));

  // Allocate JSValue in new space.
  Allocate(JSValue::kSize, result, scratch1, scratch2, gc_required, TAG_OBJECT);

  // Initialize the JSValue.
  LoadGlobalFunctionInitialMap(constructor, scratch1, scratch2);
  sw(scratch1, FieldMemOperand(result, HeapObject::kMapOffset));
  LoadRoot(scratch1, Heap::kEmptyFixedArrayRootIndex);
  sw(scratch1, FieldMemOperand(result, JSObject::kPropertiesOffset));
  sw(scratch1, FieldMemOperand(result, JSObject::kElementsOffset));
  sw(value, FieldMemOperand(result, JSValue::kValueOffset));
  STATIC_ASSERT(JSValue::kSize == 4 * kPointerSize);
}


void MacroAssembler::CopyBytes(Register src,
                               Register dst,
                               Register length,
                               Register scratch) {
  Label align_loop_1, word_loop, byte_loop, byte_loop_1, done;

  // Align src before copying in word size chunks.
  Branch(&byte_loop, le, length, Operand(kPointerSize));
  bind(&align_loop_1);
  And(scratch, src, kPointerSize - 1);
  Branch(&word_loop, eq, scratch, Operand(zero_reg));
  lbu(scratch, MemOperand(src));
  Addu(src, src, 1);
  sb(scratch, MemOperand(dst));
  Addu(dst, dst, 1);
  Subu(length, length, Operand(1));
  Branch(&align_loop_1, ne, length, Operand(zero_reg));

  // Copy bytes in word size chunks.
  bind(&word_loop);
  if (emit_debug_code()) {
    And(scratch, src, kPointerSize - 1);
    Assert(eq, kExpectingAlignmentForCopyBytes,
        scratch, Operand(zero_reg));
  }
  Branch(&byte_loop, lt, length, Operand(kPointerSize));
  lw(scratch, MemOperand(src));
  Addu(src, src, kPointerSize);

  // TODO(kalmard) check if this can be optimized to use sw in most cases.
  // Can't use unaligned access - copy byte by byte.
  if (kArchEndian == kLittle) {
    sb(scratch, MemOperand(dst, 0));
    srl(scratch, scratch, 8);
    sb(scratch, MemOperand(dst, 1));
    srl(scratch, scratch, 8);
    sb(scratch, MemOperand(dst, 2));
    srl(scratch, scratch, 8);
    sb(scratch, MemOperand(dst, 3));
  } else {
    sb(scratch, MemOperand(dst, 3));
    srl(scratch, scratch, 8);
    sb(scratch, MemOperand(dst, 2));
    srl(scratch, scratch, 8);
    sb(scratch, MemOperand(dst, 1));
    srl(scratch, scratch, 8);
    sb(scratch, MemOperand(dst, 0));
  }

  Addu(dst, dst, 4);

  Subu(length, length, Operand(kPointerSize));
  Branch(&word_loop);

  // Copy the last bytes if any left.
  bind(&byte_loop);
  Branch(&done, eq, length, Operand(zero_reg));
  bind(&byte_loop_1);
  lbu(scratch, MemOperand(src));
  Addu(src, src, 1);
  sb(scratch, MemOperand(dst));
  Addu(dst, dst, 1);
  Subu(length, length, Operand(1));
  Branch(&byte_loop_1, ne, length, Operand(zero_reg));
  bind(&done);
}


void MacroAssembler::InitializeFieldsWithFiller(Register current_address,
                                                Register end_address,
                                                Register filler) {
  Label loop, entry;
  Branch(&entry);
  bind(&loop);
  sw(filler, MemOperand(current_address));
  Addu(current_address, current_address, kPointerSize);
  bind(&entry);
  Branch(&loop, ult, current_address, Operand(end_address));
}


void MacroAssembler::CheckFastElements(Register map,
                                       Register scratch,
                                       Label* fail) {
  STATIC_ASSERT(FAST_SMI_ELEMENTS == 0);
  STATIC_ASSERT(FAST_HOLEY_SMI_ELEMENTS == 1);
  STATIC_ASSERT(FAST_ELEMENTS == 2);
  STATIC_ASSERT(FAST_HOLEY_ELEMENTS == 3);
  lbu(scratch, FieldMemOperand(map, Map::kBitField2Offset));
  Branch(fail, hi, scratch,
         Operand(Map::kMaximumBitField2FastHoleyElementValue));
}


void MacroAssembler::CheckFastObjectElements(Register map,
                                             Register scratch,
                                             Label* fail) {
  STATIC_ASSERT(FAST_SMI_ELEMENTS == 0);
  STATIC_ASSERT(FAST_HOLEY_SMI_ELEMENTS == 1);
  STATIC_ASSERT(FAST_ELEMENTS == 2);
  STATIC_ASSERT(FAST_HOLEY_ELEMENTS == 3);
  lbu(scratch, FieldMemOperand(map, Map::kBitField2Offset));
  Branch(fail, ls, scratch,
         Operand(Map::kMaximumBitField2FastHoleySmiElementValue));
  Branch(fail, hi, scratch,
         Operand(Map::kMaximumBitField2FastHoleyElementValue));
}


void MacroAssembler::CheckFastSmiElements(Register map,
                                          Register scratch,
                                          Label* fail) {
  STATIC_ASSERT(FAST_SMI_ELEMENTS == 0);
  STATIC_ASSERT(FAST_HOLEY_SMI_ELEMENTS == 1);
  lbu(scratch, FieldMemOperand(map, Map::kBitField2Offset));
  Branch(fail, hi, scratch,
         Operand(Map::kMaximumBitField2FastHoleySmiElementValue));
}


void MacroAssembler::StoreNumberToDoubleElements(Register value_reg,
                                                 Register key_reg,
                                                 Register elements_reg,
                                                 Register scratch1,
                                                 Register scratch2,
                                                 Register scratch3,
                                                 Label* fail,
                                                 int elements_offset) {
  DCHECK(!AreAliased(value_reg, key_reg, elements_reg, scratch1, scratch2,
                     scratch3));
  Label smi_value, maybe_nan, have_double_value, is_nan, done;
  Register mantissa_reg = scratch2;
  Register exponent_reg = scratch3;

  // Handle smi values specially.
  JumpIfSmi(value_reg, &smi_value);

  // Ensure that the object is a heap number
  CheckMap(value_reg,
           scratch1,
           Heap::kHeapNumberMapRootIndex,
           fail,
           DONT_DO_SMI_CHECK);

  // Check for nan: all NaN values have a value greater (signed) than 0x7ff00000
  // in the exponent.
  li(scratch1, Operand(kHoleNanUpper32 & HeapNumber::kExponentMask));
  lw(exponent_reg, FieldMemOperand(value_reg, HeapNumber::kExponentOffset));
  Branch(&maybe_nan, ge, exponent_reg, Operand(scratch1));

  lw(mantissa_reg, FieldMemOperand(value_reg, HeapNumber::kMantissaOffset));

  bind(&have_double_value);
  Lsa(scratch1, elements_reg, key_reg, kDoubleSizeLog2 - kSmiTagSize);
  sw(mantissa_reg,
      FieldMemOperand(scratch1, FixedDoubleArray::kHeaderSize - elements_offset
          + kHoleNanLower32Offset));
  sw(exponent_reg,
      FieldMemOperand(scratch1, FixedDoubleArray::kHeaderSize - elements_offset
          + kHoleNanUpper32Offset));
  jmp(&done);

  bind(&maybe_nan);
  // Could be NaN or Infinity. If fraction is not zero, it's NaN, otherwise
  // it's an Infinity, and the non-NaN code path applies.
  Branch(&is_nan, gt, exponent_reg, Operand(scratch1));
  lw(mantissa_reg, FieldMemOperand(value_reg, HeapNumber::kMantissaOffset));
  Branch(&have_double_value, eq, mantissa_reg, Operand(zero_reg));
  bind(&is_nan);
  // Load canonical NaN for storing into the double array.
  LoadRoot(at, Heap::kNanValueRootIndex);
  lw(mantissa_reg, FieldMemOperand(at, HeapNumber::kMantissaOffset));
  lw(exponent_reg, FieldMemOperand(at, HeapNumber::kExponentOffset));
  jmp(&have_double_value);

  bind(&smi_value);
  Addu(scratch1, elements_reg,
      Operand(FixedDoubleArray::kHeaderSize - kHeapObjectTag -
              elements_offset));
  Lsa(scratch1, scratch1, key_reg, kDoubleSizeLog2 - kSmiTagSize);
  // scratch1 is now effective address of the double element

  Register untagged_value = scratch2;
  SmiUntag(untagged_value, value_reg);
  mtc1(untagged_value, f2);
  cvt_d_w(f0, f2);
  sdc1(f0, MemOperand(scratch1, 0));
  bind(&done);
}


void MacroAssembler::CompareMapAndBranch(Register obj,
                                         Register scratch,
                                         Handle<Map> map,
                                         Label* early_success,
                                         Condition cond,
                                         Label* branch_to) {
  lw(scratch, FieldMemOperand(obj, HeapObject::kMapOffset));
  CompareMapAndBranch(scratch, map, early_success, cond, branch_to);
}


void MacroAssembler::CompareMapAndBranch(Register obj_map,
                                         Handle<Map> map,
                                         Label* early_success,
                                         Condition cond,
                                         Label* branch_to) {
  Branch(branch_to, cond, obj_map, Operand(map));
}


void MacroAssembler::CheckMap(Register obj,
                              Register scratch,
                              Handle<Map> map,
                              Label* fail,
                              SmiCheckType smi_check_type) {
  if (smi_check_type == DO_SMI_CHECK) {
    JumpIfSmi(obj, fail);
  }
  Label success;
  CompareMapAndBranch(obj, scratch, map, &success, ne, fail);
  bind(&success);
}


void MacroAssembler::DispatchWeakMap(Register obj, Register scratch1,
                                     Register scratch2, Handle<WeakCell> cell,
                                     Handle<Code> success,
                                     SmiCheckType smi_check_type) {
  Label fail;
  if (smi_check_type == DO_SMI_CHECK) {
    JumpIfSmi(obj, &fail);
  }
  lw(scratch1, FieldMemOperand(obj, HeapObject::kMapOffset));
  GetWeakValue(scratch2, cell);
  Jump(success, RelocInfo::CODE_TARGET, eq, scratch1, Operand(scratch2));
  bind(&fail);
}


void MacroAssembler::CheckMap(Register obj,
                              Register scratch,
                              Heap::RootListIndex index,
                              Label* fail,
                              SmiCheckType smi_check_type) {
  if (smi_check_type == DO_SMI_CHECK) {
    JumpIfSmi(obj, fail);
  }
  lw(scratch, FieldMemOperand(obj, HeapObject::kMapOffset));
  LoadRoot(at, index);
  Branch(fail, ne, scratch, Operand(at));
}


void MacroAssembler::GetWeakValue(Register value, Handle<WeakCell> cell) {
  li(value, Operand(cell));
  lw(value, FieldMemOperand(value, WeakCell::kValueOffset));
}


void MacroAssembler::LoadWeakValue(Register value, Handle<WeakCell> cell,
                                   Label* miss) {
  GetWeakValue(value, cell);
  JumpIfSmi(value, miss);
}


void MacroAssembler::MovFromFloatResult(DoubleRegister dst) {
  if (IsMipsSoftFloatABI) {
    if (kArchEndian == kLittle) {
      Move(dst, v0, v1);
    } else {
      Move(dst, v1, v0);
    }
  } else {
    Move(dst, f0);  // Reg f0 is o32 ABI FP return value.
  }
}


void MacroAssembler::MovFromFloatParameter(DoubleRegister dst) {
  if (IsMipsSoftFloatABI) {
    if (kArchEndian == kLittle) {
      Move(dst, a0, a1);
    } else {
      Move(dst, a1, a0);
    }
  } else {
    Move(dst, f12);  // Reg f12 is o32 ABI FP first argument value.
  }
}


void MacroAssembler::MovToFloatParameter(DoubleRegister src) {
  if (!IsMipsSoftFloatABI) {
    Move(f12, src);
  } else {
    if (kArchEndian == kLittle) {
      Move(a0, a1, src);
    } else {
      Move(a1, a0, src);
    }
  }
}


void MacroAssembler::MovToFloatResult(DoubleRegister src) {
  if (!IsMipsSoftFloatABI) {
    Move(f0, src);
  } else {
    if (kArchEndian == kLittle) {
      Move(v0, v1, src);
    } else {
      Move(v1, v0, src);
    }
  }
}


void MacroAssembler::MovToFloatParameters(DoubleRegister src1,
                                          DoubleRegister src2) {
  if (!IsMipsSoftFloatABI) {
    if (src2.is(f12)) {
      DCHECK(!src1.is(f14));
      Move(f14, src2);
      Move(f12, src1);
    } else {
      Move(f12, src1);
      Move(f14, src2);
    }
  } else {
    if (kArchEndian == kLittle) {
      Move(a0, a1, src1);
      Move(a2, a3, src2);
    } else {
      Move(a1, a0, src1);
      Move(a3, a2, src2);
    }
  }
}


// -----------------------------------------------------------------------------
// JavaScript invokes.

void MacroAssembler::InvokePrologue(const ParameterCount& expected,
                                    const ParameterCount& actual,
                                    Label* done,
                                    bool* definitely_mismatches,
                                    InvokeFlag flag,
                                    const CallWrapper& call_wrapper) {
  bool definitely_matches = false;
  *definitely_mismatches = false;
  Label regular_invoke;

  // Check whether the expected and actual arguments count match. If not,
  // setup registers according to contract with ArgumentsAdaptorTrampoline:
  //  a0: actual arguments count
  //  a1: function (passed through to callee)
  //  a2: expected arguments count

  // The code below is made a lot easier because the calling code already sets
  // up actual and expected registers according to the contract if values are
  // passed in registers.
  DCHECK(actual.is_immediate() || actual.reg().is(a0));
  DCHECK(expected.is_immediate() || expected.reg().is(a2));

  if (expected.is_immediate()) {
    DCHECK(actual.is_immediate());
    li(a0, Operand(actual.immediate()));
    if (expected.immediate() == actual.immediate()) {
      definitely_matches = true;
    } else {
      const int sentinel = SharedFunctionInfo::kDontAdaptArgumentsSentinel;
      if (expected.immediate() == sentinel) {
        // Don't worry about adapting arguments for builtins that
        // don't want that done. Skip adaption code by making it look
        // like we have a match between expected and actual number of
        // arguments.
        definitely_matches = true;
      } else {
        *definitely_mismatches = true;
        li(a2, Operand(expected.immediate()));
      }
    }
  } else if (actual.is_immediate()) {
    li(a0, Operand(actual.immediate()));
    Branch(&regular_invoke, eq, expected.reg(), Operand(a0));
  } else {
    Branch(&regular_invoke, eq, expected.reg(), Operand(actual.reg()));
  }

  if (!definitely_matches) {
    Handle<Code> adaptor =
        isolate()->builtins()->ArgumentsAdaptorTrampoline();
    if (flag == CALL_FUNCTION) {
      call_wrapper.BeforeCall(CallSize(adaptor));
      Call(adaptor);
      call_wrapper.AfterCall();
      if (!*definitely_mismatches) {
        Branch(done);
      }
    } else {
      Jump(adaptor, RelocInfo::CODE_TARGET);
    }
    bind(&regular_invoke);
  }
}


void MacroAssembler::FloodFunctionIfStepping(Register fun, Register new_target,
                                             const ParameterCount& expected,
                                             const ParameterCount& actual) {
  Label skip_flooding;
  ExternalReference step_in_enabled =
      ExternalReference::debug_step_in_enabled_address(isolate());
  li(t0, Operand(step_in_enabled));
  lb(t0, MemOperand(t0));
  Branch(&skip_flooding, eq, t0, Operand(zero_reg));
  {
    FrameScope frame(this,
                     has_frame() ? StackFrame::NONE : StackFrame::INTERNAL);
    if (expected.is_reg()) {
      SmiTag(expected.reg());
      Push(expected.reg());
    }
    if (actual.is_reg()) {
      SmiTag(actual.reg());
      Push(actual.reg());
    }
    if (new_target.is_valid()) {
      Push(new_target);
    }
    Push(fun);
    Push(fun);
    CallRuntime(Runtime::kDebugPrepareStepInIfStepping);
    Pop(fun);
    if (new_target.is_valid()) {
      Pop(new_target);
    }
    if (actual.is_reg()) {
      Pop(actual.reg());
      SmiUntag(actual.reg());
    }
    if (expected.is_reg()) {
      Pop(expected.reg());
      SmiUntag(expected.reg());
    }
  }
  bind(&skip_flooding);
}


void MacroAssembler::InvokeFunctionCode(Register function, Register new_target,
                                        const ParameterCount& expected,
                                        const ParameterCount& actual,
                                        InvokeFlag flag,
                                        const CallWrapper& call_wrapper) {
  // You can't call a function without a valid frame.
  DCHECK(flag == JUMP_FUNCTION || has_frame());
  DCHECK(function.is(a1));
  DCHECK_IMPLIES(new_target.is_valid(), new_target.is(a3));

  if (call_wrapper.NeedsDebugStepCheck()) {
    FloodFunctionIfStepping(function, new_target, expected, actual);
  }

  // Clear the new.target register if not given.
  if (!new_target.is_valid()) {
    LoadRoot(a3, Heap::kUndefinedValueRootIndex);
  }

  Label done;
  bool definitely_mismatches = false;
  InvokePrologue(expected, actual, &done, &definitely_mismatches, flag,
                 call_wrapper);
  if (!definitely_mismatches) {
    // We call indirectly through the code field in the function to
    // allow recompilation to take effect without changing any of the
    // call sites.
    Register code = t0;
    lw(code, FieldMemOperand(function, JSFunction::kCodeEntryOffset));
    if (flag == CALL_FUNCTION) {
      call_wrapper.BeforeCall(CallSize(code));
      Call(code);
      call_wrapper.AfterCall();
    } else {
      DCHECK(flag == JUMP_FUNCTION);
      Jump(code);
    }
    // Continue here if InvokePrologue does handle the invocation due to
    // mismatched parameter counts.
    bind(&done);
  }
}


void MacroAssembler::InvokeFunction(Register function,
                                    Register new_target,
                                    const ParameterCount& actual,
                                    InvokeFlag flag,
                                    const CallWrapper& call_wrapper) {
  // You can't call a function without a valid frame.
  DCHECK(flag == JUMP_FUNCTION || has_frame());

  // Contract with called JS functions requires that function is passed in a1.
  DCHECK(function.is(a1));
  Register expected_reg = a2;
  Register temp_reg = t0;

  lw(temp_reg, FieldMemOperand(a1, JSFunction::kSharedFunctionInfoOffset));
  lw(cp, FieldMemOperand(a1, JSFunction::kContextOffset));
  lw(expected_reg,
     FieldMemOperand(temp_reg,
                     SharedFunctionInfo::kFormalParameterCountOffset));
  sra(expected_reg, expected_reg, kSmiTagSize);

  ParameterCount expected(expected_reg);
  InvokeFunctionCode(function, new_target, expected, actual, flag,
                     call_wrapper);
}


void MacroAssembler::InvokeFunction(Register function,
                                    const ParameterCount& expected,
                                    const ParameterCount& actual,
                                    InvokeFlag flag,
                                    const CallWrapper& call_wrapper) {
  // You can't call a function without a valid frame.
  DCHECK(flag == JUMP_FUNCTION || has_frame());

  // Contract with called JS functions requires that function is passed in a1.
  DCHECK(function.is(a1));

  // Get the function and setup the context.
  lw(cp, FieldMemOperand(a1, JSFunction::kContextOffset));

  InvokeFunctionCode(a1, no_reg, expected, actual, flag, call_wrapper);
}


void MacroAssembler::InvokeFunction(Handle<JSFunction> function,
                                    const ParameterCount& expected,
                                    const ParameterCount& actual,
                                    InvokeFlag flag,
                                    const CallWrapper& call_wrapper) {
  li(a1, function);
  InvokeFunction(a1, expected, actual, flag, call_wrapper);
}


void MacroAssembler::IsObjectJSStringType(Register object,
                                          Register scratch,
                                          Label* fail) {
  DCHECK(kNotStringTag != 0);

  lw(scratch, FieldMemOperand(object, HeapObject::kMapOffset));
  lbu(scratch, FieldMemOperand(scratch, Map::kInstanceTypeOffset));
  And(scratch, scratch, Operand(kIsNotStringMask));
  Branch(fail, ne, scratch, Operand(zero_reg));
}


void MacroAssembler::IsObjectNameType(Register object,
                                      Register scratch,
                                      Label* fail) {
  lw(scratch, FieldMemOperand(object, HeapObject::kMapOffset));
  lbu(scratch, FieldMemOperand(scratch, Map::kInstanceTypeOffset));
  Branch(fail, hi, scratch, Operand(LAST_NAME_TYPE));
}


// ---------------------------------------------------------------------------
// Support functions.


void MacroAssembler::GetMapConstructor(Register result, Register map,
                                       Register temp, Register temp2) {
  Label done, loop;
  lw(result, FieldMemOperand(map, Map::kConstructorOrBackPointerOffset));
  bind(&loop);
  JumpIfSmi(result, &done);
  GetObjectType(result, temp, temp2);
  Branch(&done, ne, temp2, Operand(MAP_TYPE));
  lw(result, FieldMemOperand(result, Map::kConstructorOrBackPointerOffset));
  Branch(&loop);
  bind(&done);
}


void MacroAssembler::TryGetFunctionPrototype(Register function, Register result,
                                             Register scratch, Label* miss) {
  // Get the prototype or initial map from the function.
  lw(result,
     FieldMemOperand(function, JSFunction::kPrototypeOrInitialMapOffset));

  // If the prototype or initial map is the hole, don't return it and
  // simply miss the cache instead. This will allow us to allocate a
  // prototype object on-demand in the runtime system.
  LoadRoot(t8, Heap::kTheHoleValueRootIndex);
  Branch(miss, eq, result, Operand(t8));

  // If the function does not have an initial map, we're done.
  Label done;
  GetObjectType(result, scratch, scratch);
  Branch(&done, ne, scratch, Operand(MAP_TYPE));

  // Get the prototype from the initial map.
  lw(result, FieldMemOperand(result, Map::kPrototypeOffset));

  // All done.
  bind(&done);
}


void MacroAssembler::GetObjectType(Register object,
                                   Register map,
                                   Register type_reg) {
  lw(map, FieldMemOperand(object, HeapObject::kMapOffset));
  lbu(type_reg, FieldMemOperand(map, Map::kInstanceTypeOffset));
}


// -----------------------------------------------------------------------------
// Runtime calls.

void MacroAssembler::CallStub(CodeStub* stub,
                              TypeFeedbackId ast_id,
                              Condition cond,
                              Register r1,
                              const Operand& r2,
                              BranchDelaySlot bd) {
  DCHECK(AllowThisStubCall(stub));  // Stub calls are not allowed in some stubs.
  Call(stub->GetCode(), RelocInfo::CODE_TARGET, ast_id,
       cond, r1, r2, bd);
}


void MacroAssembler::TailCallStub(CodeStub* stub,
                                  Condition cond,
                                  Register r1,
                                  const Operand& r2,
                                  BranchDelaySlot bd) {
  Jump(stub->GetCode(), RelocInfo::CODE_TARGET, cond, r1, r2, bd);
}


bool MacroAssembler::AllowThisStubCall(CodeStub* stub) {
  return has_frame_ || !stub->SometimesSetsUpAFrame();
}


void MacroAssembler::IndexFromHash(Register hash, Register index) {
  // If the hash field contains an array index pick it out. The assert checks
  // that the constants for the maximum number of digits for an array index
  // cached in the hash field and the number of bits reserved for it does not
  // conflict.
  DCHECK(TenToThe(String::kMaxCachedArrayIndexLength) <
         (1 << String::kArrayIndexValueBits));
  DecodeFieldToSmi<String::ArrayIndexValueBits>(index, hash);
}


void MacroAssembler::ObjectToDoubleFPURegister(Register object,
                                               FPURegister result,
                                               Register scratch1,
                                               Register scratch2,
                                               Register heap_number_map,
                                               Label* not_number,
                                               ObjectToDoubleFlags flags) {
  Label done;
  if ((flags & OBJECT_NOT_SMI) == 0) {
    Label not_smi;
    JumpIfNotSmi(object, &not_smi);
    // Remove smi tag and convert to double.
    sra(scratch1, object, kSmiTagSize);
    mtc1(scratch1, result);
    cvt_d_w(result, result);
    Branch(&done);
    bind(&not_smi);
  }
  // Check for heap number and load double value from it.
  lw(scratch1, FieldMemOperand(object, HeapObject::kMapOffset));
  Branch(not_number, ne, scratch1, Operand(heap_number_map));

  if ((flags & AVOID_NANS_AND_INFINITIES) != 0) {
    // If exponent is all ones the number is either a NaN or +/-Infinity.
    Register exponent = scratch1;
    Register mask_reg = scratch2;
    lw(exponent, FieldMemOperand(object, HeapNumber::kExponentOffset));
    li(mask_reg, HeapNumber::kExponentMask);

    And(exponent, exponent, mask_reg);
    Branch(not_number, eq, exponent, Operand(mask_reg));
  }
  ldc1(result, FieldMemOperand(object, HeapNumber::kValueOffset));
  bind(&done);
}


void MacroAssembler::SmiToDoubleFPURegister(Register smi,
                                            FPURegister value,
                                            Register scratch1) {
  sra(scratch1, smi, kSmiTagSize);
  mtc1(scratch1, value);
  cvt_d_w(value, value);
}


static inline void BranchOvfHelper(MacroAssembler* masm, Register overflow_dst,
                                   Label* overflow_label,
                                   Label* no_overflow_label) {
  DCHECK(overflow_label || no_overflow_label);
  if (!overflow_label) {
    DCHECK(no_overflow_label);
    masm->Branch(no_overflow_label, ge, overflow_dst, Operand(zero_reg));
  } else {
    masm->Branch(overflow_label, lt, overflow_dst, Operand(zero_reg));
    if (no_overflow_label) masm->Branch(no_overflow_label);
  }
}


void MacroAssembler::AddBranchOvf(Register dst, Register left,
                                  const Operand& right, Label* overflow_label,
                                  Label* no_overflow_label, Register scratch) {
  if (right.is_reg()) {
    AddBranchOvf(dst, left, right.rm(), overflow_label, no_overflow_label,
                 scratch);
  } else {
    if (IsMipsArchVariant(kMips32r6)) {
      Register right_reg = t9;
      DCHECK(!left.is(right_reg));
      li(right_reg, Operand(right));
      AddBranchOvf(dst, left, right_reg, overflow_label, no_overflow_label);
    } else {
      Register overflow_dst = t9;
      DCHECK(!dst.is(scratch));
      DCHECK(!dst.is(overflow_dst));
      DCHECK(!scratch.is(overflow_dst));
      DCHECK(!left.is(overflow_dst));
      if (dst.is(left)) {
        mov(scratch, left);                  // Preserve left.
        Addu(dst, left, right.immediate());  // Left is overwritten.
        xor_(scratch, dst, scratch);         // Original left.
        // Load right since xori takes uint16 as immediate.
        Addu(overflow_dst, zero_reg, right);
        xor_(overflow_dst, dst, overflow_dst);
        and_(overflow_dst, overflow_dst, scratch);
      } else {
        Addu(dst, left, right.immediate());
        xor_(overflow_dst, dst, left);
        // Load right since xori takes uint16 as immediate.
        Addu(scratch, zero_reg, right);
        xor_(scratch, dst, scratch);
        and_(overflow_dst, scratch, overflow_dst);
      }
      BranchOvfHelper(this, overflow_dst, overflow_label, no_overflow_label);
    }
  }
}


void MacroAssembler::AddBranchOvf(Register dst, Register left, Register right,
                                  Label* overflow_label,
                                  Label* no_overflow_label, Register scratch) {
  if (IsMipsArchVariant(kMips32r6)) {
    if (!overflow_label) {
      DCHECK(no_overflow_label);
      DCHECK(!dst.is(scratch));
      Register left_reg = left.is(dst) ? scratch : left;
      Register right_reg = right.is(dst) ? t9 : right;
      DCHECK(!dst.is(left_reg));
      DCHECK(!dst.is(right_reg));
      Move(left_reg, left);
      Move(right_reg, right);
      addu(dst, left, right);
      bnvc(left_reg, right_reg, no_overflow_label);
    } else {
      bovc(left, right, overflow_label);
      addu(dst, left, right);
      if (no_overflow_label) bc(no_overflow_label);
    }
  } else {
    Register overflow_dst = t9;
    DCHECK(!dst.is(scratch));
    DCHECK(!dst.is(overflow_dst));
    DCHECK(!scratch.is(overflow_dst));
    DCHECK(!left.is(overflow_dst));
    DCHECK(!right.is(overflow_dst));
    DCHECK(!left.is(scratch));
    DCHECK(!right.is(scratch));

    if (left.is(right) && dst.is(left)) {
      mov(overflow_dst, right);
      right = overflow_dst;
    }

    if (dst.is(left)) {
      mov(scratch, left);           // Preserve left.
      addu(dst, left, right);       // Left is overwritten.
      xor_(scratch, dst, scratch);  // Original left.
      xor_(overflow_dst, dst, right);
      and_(overflow_dst, overflow_dst, scratch);
    } else if (dst.is(right)) {
      mov(scratch, right);          // Preserve right.
      addu(dst, left, right);       // Right is overwritten.
      xor_(scratch, dst, scratch);  // Original right.
      xor_(overflow_dst, dst, left);
      and_(overflow_dst, overflow_dst, scratch);
    } else {
      addu(dst, left, right);
      xor_(overflow_dst, dst, left);
      xor_(scratch, dst, right);
      and_(overflow_dst, scratch, overflow_dst);
    }
    BranchOvfHelper(this, overflow_dst, overflow_label, no_overflow_label);
  }
}


void MacroAssembler::SubBranchOvf(Register dst, Register left,
                                  const Operand& right, Label* overflow_label,
                                  Label* no_overflow_label, Register scratch) {
  DCHECK(overflow_label || no_overflow_label);
  if (right.is_reg()) {
    SubBranchOvf(dst, left, right.rm(), overflow_label, no_overflow_label,
                 scratch);
  } else {
    Register overflow_dst = t9;
    DCHECK(!dst.is(scratch));
    DCHECK(!dst.is(overflow_dst));
    DCHECK(!scratch.is(overflow_dst));
    DCHECK(!left.is(overflow_dst));
    DCHECK(!left.is(scratch));
    if (dst.is(left)) {
      mov(scratch, left);                      // Preserve left.
      Subu(dst, left, right.immediate());      // Left is overwritten.
      // Load right since xori takes uint16 as immediate.
      Addu(overflow_dst, zero_reg, right);
      xor_(overflow_dst, scratch, overflow_dst);  // scratch is original left.
      xor_(scratch, dst, scratch);                // scratch is original left.
      and_(overflow_dst, scratch, overflow_dst);
    } else {
      Subu(dst, left, right);
      xor_(overflow_dst, dst, left);
      // Load right since xori takes uint16 as immediate.
      Addu(scratch, zero_reg, right);
      xor_(scratch, left, scratch);
      and_(overflow_dst, scratch, overflow_dst);
    }
    BranchOvfHelper(this, overflow_dst, overflow_label, no_overflow_label);
  }
}


void MacroAssembler::SubBranchOvf(Register dst, Register left, Register right,
                                  Label* overflow_label,
                                  Label* no_overflow_label, Register scratch) {
  DCHECK(overflow_label || no_overflow_label);
  Register overflow_dst = t9;
  DCHECK(!dst.is(scratch));
  DCHECK(!dst.is(overflow_dst));
  DCHECK(!scratch.is(overflow_dst));
  DCHECK(!overflow_dst.is(left));
  DCHECK(!overflow_dst.is(right));
  DCHECK(!scratch.is(left));
  DCHECK(!scratch.is(right));

  // This happens with some crankshaft code. Since Subu works fine if
  // left == right, let's not make that restriction here.
  if (left.is(right)) {
    mov(dst, zero_reg);
    if (no_overflow_label) {
      Branch(no_overflow_label);
    }
  }

  if (dst.is(left)) {
    mov(scratch, left);  // Preserve left.
    subu(dst, left, right);  // Left is overwritten.
    xor_(overflow_dst, dst, scratch);  // scratch is original left.
    xor_(scratch, scratch, right);  // scratch is original left.
    and_(overflow_dst, scratch, overflow_dst);
  } else if (dst.is(right)) {
    mov(scratch, right);  // Preserve right.
    subu(dst, left, right);  // Right is overwritten.
    xor_(overflow_dst, dst, left);
    xor_(scratch, left, scratch);  // Original right.
    and_(overflow_dst, scratch, overflow_dst);
  } else {
    subu(dst, left, right);
    xor_(overflow_dst, dst, left);
    xor_(scratch, left, right);
    and_(overflow_dst, scratch, overflow_dst);
  }
  BranchOvfHelper(this, overflow_dst, overflow_label, no_overflow_label);
}


void MacroAssembler::CallRuntime(const Runtime::Function* f, int num_arguments,
                                 SaveFPRegsMode save_doubles,
                                 BranchDelaySlot bd) {
  // All parameters are on the stack. v0 has the return value after call.

  // If the expected number of arguments of the runtime function is
  // constant, we check that the actual number of arguments match the
  // expectation.
  CHECK(f->nargs < 0 || f->nargs == num_arguments);

  // TODO(1236192): Most runtime routines don't need the number of
  // arguments passed in because it is constant. At some point we
  // should remove this need and make the runtime routine entry code
  // smarter.
  PrepareCEntryArgs(num_arguments);
  PrepareCEntryFunction(ExternalReference(f, isolate()));
  CEntryStub stub(isolate(), 1, save_doubles);
  CallStub(&stub, TypeFeedbackId::None(), al, zero_reg, Operand(zero_reg), bd);
}


void MacroAssembler::CallExternalReference(const ExternalReference& ext,
                                           int num_arguments,
                                           BranchDelaySlot bd) {
  PrepareCEntryArgs(num_arguments);
  PrepareCEntryFunction(ext);

  CEntryStub stub(isolate(), 1);
  CallStub(&stub, TypeFeedbackId::None(), al, zero_reg, Operand(zero_reg), bd);
}


void MacroAssembler::TailCallRuntime(Runtime::FunctionId fid) {
  const Runtime::Function* function = Runtime::FunctionForId(fid);
  DCHECK_EQ(1, function->result_size);
  if (function->nargs >= 0) {
    PrepareCEntryArgs(function->nargs);
  }
  JumpToExternalReference(ExternalReference(fid, isolate()));
}


void MacroAssembler::JumpToExternalReference(const ExternalReference& builtin,
                                             BranchDelaySlot bd) {
  PrepareCEntryFunction(builtin);
  CEntryStub stub(isolate(), 1);
  Jump(stub.GetCode(),
       RelocInfo::CODE_TARGET,
       al,
       zero_reg,
       Operand(zero_reg),
       bd);
}


void MacroAssembler::SetCounter(StatsCounter* counter, int value,
                                Register scratch1, Register scratch2) {
  if (FLAG_native_code_counters && counter->Enabled()) {
    li(scratch1, Operand(value));
    li(scratch2, Operand(ExternalReference(counter)));
    sw(scratch1, MemOperand(scratch2));
  }
}


void MacroAssembler::IncrementCounter(StatsCounter* counter, int value,
                                      Register scratch1, Register scratch2) {
  DCHECK(value > 0);
  if (FLAG_native_code_counters && counter->Enabled()) {
    li(scratch2, Operand(ExternalReference(counter)));
    lw(scratch1, MemOperand(scratch2));
    Addu(scratch1, scratch1, Operand(value));
    sw(scratch1, MemOperand(scratch2));
  }
}


void MacroAssembler::DecrementCounter(StatsCounter* counter, int value,
                                      Register scratch1, Register scratch2) {
  DCHECK(value > 0);
  if (FLAG_native_code_counters && counter->Enabled()) {
    li(scratch2, Operand(ExternalReference(counter)));
    lw(scratch1, MemOperand(scratch2));
    Subu(scratch1, scratch1, Operand(value));
    sw(scratch1, MemOperand(scratch2));
  }
}


// -----------------------------------------------------------------------------
// Debugging.

void MacroAssembler::Assert(Condition cc, BailoutReason reason,
                            Register rs, Operand rt) {
  if (emit_debug_code())
    Check(cc, reason, rs, rt);
}


void MacroAssembler::AssertFastElements(Register elements) {
  if (emit_debug_code()) {
    DCHECK(!elements.is(at));
    Label ok;
    push(elements);
    lw(elements, FieldMemOperand(elements, HeapObject::kMapOffset));
    LoadRoot(at, Heap::kFixedArrayMapRootIndex);
    Branch(&ok, eq, elements, Operand(at));
    LoadRoot(at, Heap::kFixedDoubleArrayMapRootIndex);
    Branch(&ok, eq, elements, Operand(at));
    LoadRoot(at, Heap::kFixedCOWArrayMapRootIndex);
    Branch(&ok, eq, elements, Operand(at));
    Abort(kJSObjectWithFastElementsMapHasSlowElements);
    bind(&ok);
    pop(elements);
  }
}


void MacroAssembler::Check(Condition cc, BailoutReason reason,
                           Register rs, Operand rt) {
  Label L;
  Branch(&L, cc, rs, rt);
  Abort(reason);
  // Will not return here.
  bind(&L);
}


void MacroAssembler::Abort(BailoutReason reason) {
  Label abort_start;
  bind(&abort_start);
#ifdef DEBUG
  const char* msg = GetBailoutReason(reason);
  if (msg != NULL) {
    RecordComment("Abort message: ");
    RecordComment(msg);
  }

  if (FLAG_trap_on_abort) {
    stop(msg);
    return;
  }
#endif

  li(a0, Operand(Smi::FromInt(reason)));
  push(a0);
  // Disable stub call restrictions to always allow calls to abort.
  if (!has_frame_) {
    // We don't actually want to generate a pile of code for this, so just
    // claim there is a stack frame, without generating one.
    FrameScope scope(this, StackFrame::NONE);
    CallRuntime(Runtime::kAbort);
  } else {
    CallRuntime(Runtime::kAbort);
  }
  // Will not return here.
  if (is_trampoline_pool_blocked()) {
    // If the calling code cares about the exact number of
    // instructions generated, we insert padding here to keep the size
    // of the Abort macro constant.
    // Currently in debug mode with debug_code enabled the number of
    // generated instructions is 10, so we use this as a maximum value.
    static const int kExpectedAbortInstructions = 10;
    int abort_instructions = InstructionsGeneratedSince(&abort_start);
    DCHECK(abort_instructions <= kExpectedAbortInstructions);
    while (abort_instructions++ < kExpectedAbortInstructions) {
      nop();
    }
  }
}


void MacroAssembler::LoadContext(Register dst, int context_chain_length) {
  if (context_chain_length > 0) {
    // Move up the chain of contexts to the context containing the slot.
    lw(dst, MemOperand(cp, Context::SlotOffset(Context::PREVIOUS_INDEX)));
    for (int i = 1; i < context_chain_length; i++) {
      lw(dst, MemOperand(dst, Context::SlotOffset(Context::PREVIOUS_INDEX)));
    }
  } else {
    // Slot is in the current function context.  Move it into the
    // destination register in case we store into it (the write barrier
    // cannot be allowed to destroy the context in esi).
    Move(dst, cp);
  }
}


void MacroAssembler::LoadTransitionedArrayMapConditional(
    ElementsKind expected_kind,
    ElementsKind transitioned_kind,
    Register map_in_out,
    Register scratch,
    Label* no_map_match) {
  DCHECK(IsFastElementsKind(expected_kind));
  DCHECK(IsFastElementsKind(transitioned_kind));

  // Check that the function's map is the same as the expected cached map.
  lw(scratch, NativeContextMemOperand());
  lw(at, ContextMemOperand(scratch, Context::ArrayMapIndex(expected_kind)));
  Branch(no_map_match, ne, map_in_out, Operand(at));

  // Use the transitioned cached map.
  lw(map_in_out,
     ContextMemOperand(scratch, Context::ArrayMapIndex(transitioned_kind)));
}


void MacroAssembler::LoadNativeContextSlot(int index, Register dst) {
  lw(dst, NativeContextMemOperand());
  lw(dst, ContextMemOperand(dst, index));
}


void MacroAssembler::LoadGlobalFunctionInitialMap(Register function,
                                                  Register map,
                                                  Register scratch) {
  // Load the initial map. The global functions all have initial maps.
  lw(map, FieldMemOperand(function, JSFunction::kPrototypeOrInitialMapOffset));
  if (emit_debug_code()) {
    Label ok, fail;
    CheckMap(map, scratch, Heap::kMetaMapRootIndex, &fail, DO_SMI_CHECK);
    Branch(&ok);
    bind(&fail);
    Abort(kGlobalFunctionsMustHaveInitialMap);
    bind(&ok);
  }
}


void MacroAssembler::StubPrologue() {
    Push(ra, fp, cp);
    Push(Smi::FromInt(StackFrame::STUB));
    // Adjust FP to point to saved FP.
    Addu(fp, sp, Operand(StandardFrameConstants::kFixedFrameSizeFromFp));
}


void MacroAssembler::Prologue(bool code_pre_aging) {
  PredictableCodeSizeScope predictible_code_size_scope(
      this, kNoCodeAgeSequenceLength);
  // The following three instructions must remain together and unmodified
  // for code aging to work properly.
  if (code_pre_aging) {
    // Pre-age the code.
    Code* stub = Code::GetPreAgedCodeAgeStub(isolate());
    nop(Assembler::CODE_AGE_MARKER_NOP);
    // Load the stub address to t9 and call it,
    // GetCodeAgeAndParity() extracts the stub address from this instruction.
    li(t9,
       Operand(reinterpret_cast<uint32_t>(stub->instruction_start())),
       CONSTANT_SIZE);
    nop();  // Prevent jalr to jal optimization.
    jalr(t9, a0);
    nop();  // Branch delay slot nop.
    nop();  // Pad the empty space.
  } else {
    Push(ra, fp, cp, a1);
    nop(Assembler::CODE_AGE_SEQUENCE_NOP);
    // Adjust fp to point to caller's fp.
    Addu(fp, sp, Operand(StandardFrameConstants::kFixedFrameSizeFromFp));
  }
}


void MacroAssembler::EmitLoadTypeFeedbackVector(Register vector) {
  lw(vector, MemOperand(fp, JavaScriptFrameConstants::kFunctionOffset));
  lw(vector, FieldMemOperand(vector, JSFunction::kLiteralsOffset));
  lw(vector, FieldMemOperand(vector, LiteralsArray::kFeedbackVectorOffset));
}


void MacroAssembler::EnterFrame(StackFrame::Type type,
                                bool load_constant_pool_pointer_reg) {
  // Out-of-line constant pool not implemented on mips.
  UNREACHABLE();
}


void MacroAssembler::EnterFrame(StackFrame::Type type) {
  addiu(sp, sp, -5 * kPointerSize);
  li(t8, Operand(Smi::FromInt(type)));
  li(t9, Operand(CodeObject()), CONSTANT_SIZE);
  sw(ra, MemOperand(sp, 4 * kPointerSize));
  sw(fp, MemOperand(sp, 3 * kPointerSize));
  sw(cp, MemOperand(sp, 2 * kPointerSize));
  sw(t8, MemOperand(sp, 1 * kPointerSize));
  sw(t9, MemOperand(sp, 0 * kPointerSize));
  // Adjust FP to point to saved FP.
  Addu(fp, sp,
       Operand(StandardFrameConstants::kFixedFrameSizeFromFp + kPointerSize));
}


void MacroAssembler::LeaveFrame(StackFrame::Type type) {
  mov(sp, fp);
  lw(fp, MemOperand(sp, 0 * kPointerSize));
  lw(ra, MemOperand(sp, 1 * kPointerSize));
  addiu(sp, sp, 2 * kPointerSize);
}


void MacroAssembler::EnterExitFrame(bool save_doubles,
                                    int stack_space) {
  // Set up the frame structure on the stack.
  STATIC_ASSERT(2 * kPointerSize == ExitFrameConstants::kCallerSPDisplacement);
  STATIC_ASSERT(1 * kPointerSize == ExitFrameConstants::kCallerPCOffset);
  STATIC_ASSERT(0 * kPointerSize == ExitFrameConstants::kCallerFPOffset);

  // This is how the stack will look:
  // fp + 2 (==kCallerSPDisplacement) - old stack's end
  // [fp + 1 (==kCallerPCOffset)] - saved old ra
  // [fp + 0 (==kCallerFPOffset)] - saved old fp
  // [fp - 1 (==kSPOffset)] - sp of the called function
  // [fp - 2 (==kCodeOffset)] - CodeObject
  // fp - (2 + stack_space + alignment) == sp == [fp - kSPOffset] - top of the
  //   new stack (will contain saved ra)

  // Save registers.
  addiu(sp, sp, -4 * kPointerSize);
  sw(ra, MemOperand(sp, 3 * kPointerSize));
  sw(fp, MemOperand(sp, 2 * kPointerSize));
  addiu(fp, sp, 2 * kPointerSize);  // Set up new frame pointer.

  if (emit_debug_code()) {
    sw(zero_reg, MemOperand(fp, ExitFrameConstants::kSPOffset));
  }

  // Accessed from ExitFrame::code_slot.
  li(t8, Operand(CodeObject()), CONSTANT_SIZE);
  sw(t8, MemOperand(fp, ExitFrameConstants::kCodeOffset));

  // Save the frame pointer and the context in top.
  li(t8, Operand(ExternalReference(Isolate::kCEntryFPAddress, isolate())));
  sw(fp, MemOperand(t8));
  li(t8, Operand(ExternalReference(Isolate::kContextAddress, isolate())));
  sw(cp, MemOperand(t8));

  const int frame_alignment = MacroAssembler::ActivationFrameAlignment();
  if (save_doubles) {
    // The stack  must be allign to 0 modulo 8 for stores with sdc1.
    DCHECK(kDoubleSize == frame_alignment);
    if (frame_alignment > 0) {
      DCHECK(base::bits::IsPowerOfTwo32(frame_alignment));
      And(sp, sp, Operand(-frame_alignment));  // Align stack.
    }
    int space = FPURegister::kMaxNumRegisters * kDoubleSize;
    Subu(sp, sp, Operand(space));
    // Remember: we only need to save every 2nd double FPU value.
    for (int i = 0; i < FPURegister::kMaxNumRegisters; i+=2) {
      FPURegister reg = FPURegister::from_code(i);
      sdc1(reg, MemOperand(sp, i * kDoubleSize));
    }
  }

  // Reserve place for the return address, stack space and an optional slot
  // (used by the DirectCEntryStub to hold the return value if a struct is
  // returned) and align the frame preparing for calling the runtime function.
  DCHECK(stack_space >= 0);
  Subu(sp, sp, Operand((stack_space + 2) * kPointerSize));
  if (frame_alignment > 0) {
    DCHECK(base::bits::IsPowerOfTwo32(frame_alignment));
    And(sp, sp, Operand(-frame_alignment));  // Align stack.
  }

  // Set the exit frame sp value to point just before the return address
  // location.
  addiu(at, sp, kPointerSize);
  sw(at, MemOperand(fp, ExitFrameConstants::kSPOffset));
}


void MacroAssembler::LeaveExitFrame(bool save_doubles, Register argument_count,
                                    bool restore_context, bool do_return,
                                    bool argument_count_is_length) {
  // Optionally restore all double registers.
  if (save_doubles) {
    // Remember: we only need to restore every 2nd double FPU value.
    lw(t8, MemOperand(fp, ExitFrameConstants::kSPOffset));
    for (int i = 0; i < FPURegister::kMaxNumRegisters; i+=2) {
      FPURegister reg = FPURegister::from_code(i);
      ldc1(reg, MemOperand(t8, i * kDoubleSize + kPointerSize));
    }
  }

  // Clear top frame.
  li(t8, Operand(ExternalReference(Isolate::kCEntryFPAddress, isolate())));
  sw(zero_reg, MemOperand(t8));

  // Restore current context from top and clear it in debug mode.
  if (restore_context) {
    li(t8, Operand(ExternalReference(Isolate::kContextAddress, isolate())));
    lw(cp, MemOperand(t8));
  }
#ifdef DEBUG
  li(t8, Operand(ExternalReference(Isolate::kContextAddress, isolate())));
  sw(a3, MemOperand(t8));
#endif

  // Pop the arguments, restore registers, and return.
  mov(sp, fp);  // Respect ABI stack constraint.
  lw(fp, MemOperand(sp, ExitFrameConstants::kCallerFPOffset));
  lw(ra, MemOperand(sp, ExitFrameConstants::kCallerPCOffset));

  if (argument_count.is_valid()) {
    if (argument_count_is_length) {
      addu(sp, sp, argument_count);
    } else {
      Lsa(sp, sp, argument_count, kPointerSizeLog2, t8);
    }
  }

  if (do_return) {
    Ret(USE_DELAY_SLOT);
    // If returning, the instruction in the delay slot will be the addiu below.
  }
  addiu(sp, sp, 8);
}


void MacroAssembler::InitializeNewString(Register string,
                                         Register length,
                                         Heap::RootListIndex map_index,
                                         Register scratch1,
                                         Register scratch2) {
  sll(scratch1, length, kSmiTagSize);
  LoadRoot(scratch2, map_index);
  sw(scratch1, FieldMemOperand(string, String::kLengthOffset));
  li(scratch1, Operand(String::kEmptyHashField));
  sw(scratch2, FieldMemOperand(string, HeapObject::kMapOffset));
  sw(scratch1, FieldMemOperand(string, String::kHashFieldOffset));
}


int MacroAssembler::ActivationFrameAlignment() {
#if V8_HOST_ARCH_MIPS
  // Running on the real platform. Use the alignment as mandated by the local
  // environment.
  // Note: This will break if we ever start generating snapshots on one Mips
  // platform for another Mips platform with a different alignment.
  return base::OS::ActivationFrameAlignment();
#else  // V8_HOST_ARCH_MIPS
  // If we are using the simulator then we should always align to the expected
  // alignment. As the simulator is used to generate snapshots we do not know
  // if the target platform will need alignment, so this is controlled from a
  // flag.
  return FLAG_sim_stack_alignment;
#endif  // V8_HOST_ARCH_MIPS
}


void MacroAssembler::AssertStackIsAligned() {
  if (emit_debug_code()) {
      const int frame_alignment = ActivationFrameAlignment();
      const int frame_alignment_mask = frame_alignment - 1;

      if (frame_alignment > kPointerSize) {
        Label alignment_as_expected;
        DCHECK(base::bits::IsPowerOfTwo32(frame_alignment));
        andi(at, sp, frame_alignment_mask);
        Branch(&alignment_as_expected, eq, at, Operand(zero_reg));
        // Don't use Check here, as it will call Runtime_Abort re-entering here.
        stop("Unexpected stack alignment");
        bind(&alignment_as_expected);
      }
    }
}


void MacroAssembler::JumpIfNotPowerOfTwoOrZero(
    Register reg,
    Register scratch,
    Label* not_power_of_two_or_zero) {
  Subu(scratch, reg, Operand(1));
  Branch(USE_DELAY_SLOT, not_power_of_two_or_zero, lt,
         scratch, Operand(zero_reg));
  and_(at, scratch, reg);  // In the delay slot.
  Branch(not_power_of_two_or_zero, ne, at, Operand(zero_reg));
}


void MacroAssembler::SmiTagCheckOverflow(Register reg, Register overflow) {
  DCHECK(!reg.is(overflow));
  mov(overflow, reg);  // Save original value.
  SmiTag(reg);
  xor_(overflow, overflow, reg);  // Overflow if (value ^ 2 * value) < 0.
}


void MacroAssembler::SmiTagCheckOverflow(Register dst,
                                         Register src,
                                         Register overflow) {
  if (dst.is(src)) {
    // Fall back to slower case.
    SmiTagCheckOverflow(dst, overflow);
  } else {
    DCHECK(!dst.is(src));
    DCHECK(!dst.is(overflow));
    DCHECK(!src.is(overflow));
    SmiTag(dst, src);
    xor_(overflow, dst, src);  // Overflow if (value ^ 2 * value) < 0.
  }
}


void MacroAssembler::UntagAndJumpIfSmi(Register dst,
                                       Register src,
                                       Label* smi_case) {
  JumpIfSmi(src, smi_case, at, USE_DELAY_SLOT);
  SmiUntag(dst, src);
}


void MacroAssembler::UntagAndJumpIfNotSmi(Register dst,
                                          Register src,
                                          Label* non_smi_case) {
  JumpIfNotSmi(src, non_smi_case, at, USE_DELAY_SLOT);
  SmiUntag(dst, src);
}

void MacroAssembler::JumpIfSmi(Register value,
                               Label* smi_label,
                               Register scratch,
                               BranchDelaySlot bd) {
  DCHECK_EQ(0, kSmiTag);
  andi(scratch, value, kSmiTagMask);
  Branch(bd, smi_label, eq, scratch, Operand(zero_reg));
}

void MacroAssembler::JumpIfNotSmi(Register value,
                                  Label* not_smi_label,
                                  Register scratch,
                                  BranchDelaySlot bd) {
  DCHECK_EQ(0, kSmiTag);
  andi(scratch, value, kSmiTagMask);
  Branch(bd, not_smi_label, ne, scratch, Operand(zero_reg));
}


void MacroAssembler::JumpIfNotBothSmi(Register reg1,
                                      Register reg2,
                                      Label* on_not_both_smi) {
  STATIC_ASSERT(kSmiTag == 0);
  DCHECK_EQ(1, kSmiTagMask);
  or_(at, reg1, reg2);
  JumpIfNotSmi(at, on_not_both_smi);
}


void MacroAssembler::JumpIfEitherSmi(Register reg1,
                                     Register reg2,
                                     Label* on_either_smi) {
  STATIC_ASSERT(kSmiTag == 0);
  DCHECK_EQ(1, kSmiTagMask);
  // Both Smi tags must be 1 (not Smi).
  and_(at, reg1, reg2);
  JumpIfSmi(at, on_either_smi);
}


void MacroAssembler::AssertNotSmi(Register object) {
  if (emit_debug_code()) {
    STATIC_ASSERT(kSmiTag == 0);
    andi(at, object, kSmiTagMask);
    Check(ne, kOperandIsASmi, at, Operand(zero_reg));
  }
}


void MacroAssembler::AssertSmi(Register object) {
  if (emit_debug_code()) {
    STATIC_ASSERT(kSmiTag == 0);
    andi(at, object, kSmiTagMask);
    Check(eq, kOperandIsASmi, at, Operand(zero_reg));
  }
}


void MacroAssembler::AssertString(Register object) {
  if (emit_debug_code()) {
    STATIC_ASSERT(kSmiTag == 0);
    SmiTst(object, t8);
    Check(ne, kOperandIsASmiAndNotAString, t8, Operand(zero_reg));
    GetObjectType(object, t8, t8);
    Check(lo, kOperandIsNotAString, t8, Operand(FIRST_NONSTRING_TYPE));
  }
}


void MacroAssembler::AssertName(Register object) {
  if (emit_debug_code()) {
    STATIC_ASSERT(kSmiTag == 0);
    SmiTst(object, t8);
    Check(ne, kOperandIsASmiAndNotAName, t8, Operand(zero_reg));
    GetObjectType(object, t8, t8);
    Check(le, kOperandIsNotAName, t8, Operand(LAST_NAME_TYPE));
  }
}


void MacroAssembler::AssertFunction(Register object) {
  if (emit_debug_code()) {
    STATIC_ASSERT(kSmiTag == 0);
    SmiTst(object, t8);
    Check(ne, kOperandIsASmiAndNotAFunction, t8, Operand(zero_reg));
    GetObjectType(object, t8, t8);
    Check(eq, kOperandIsNotAFunction, t8, Operand(JS_FUNCTION_TYPE));
  }
}


void MacroAssembler::AssertBoundFunction(Register object) {
  if (emit_debug_code()) {
    STATIC_ASSERT(kSmiTag == 0);
    SmiTst(object, t8);
    Check(ne, kOperandIsASmiAndNotABoundFunction, t8, Operand(zero_reg));
    GetObjectType(object, t8, t8);
    Check(eq, kOperandIsNotABoundFunction, t8, Operand(JS_BOUND_FUNCTION_TYPE));
  }
}


void MacroAssembler::AssertUndefinedOrAllocationSite(Register object,
                                                     Register scratch) {
  if (emit_debug_code()) {
    Label done_checking;
    AssertNotSmi(object);
    LoadRoot(scratch, Heap::kUndefinedValueRootIndex);
    Branch(&done_checking, eq, object, Operand(scratch));
    lw(t8, FieldMemOperand(object, HeapObject::kMapOffset));
    LoadRoot(scratch, Heap::kAllocationSiteMapRootIndex);
    Assert(eq, kExpectedUndefinedOrCell, t8, Operand(scratch));
    bind(&done_checking);
  }
}


void MacroAssembler::AssertIsRoot(Register reg, Heap::RootListIndex index) {
  if (emit_debug_code()) {
    DCHECK(!reg.is(at));
    LoadRoot(at, index);
    Check(eq, kHeapNumberMapRegisterClobbered, reg, Operand(at));
  }
}


void MacroAssembler::JumpIfNotHeapNumber(Register object,
                                         Register heap_number_map,
                                         Register scratch,
                                         Label* on_not_heap_number) {
  lw(scratch, FieldMemOperand(object, HeapObject::kMapOffset));
  AssertIsRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);
  Branch(on_not_heap_number, ne, scratch, Operand(heap_number_map));
}


void MacroAssembler::JumpIfNonSmisNotBothSequentialOneByteStrings(
    Register first, Register second, Register scratch1, Register scratch2,
    Label* failure) {
  // Test that both first and second are sequential one-byte strings.
  // Assume that they are non-smis.
  lw(scratch1, FieldMemOperand(first, HeapObject::kMapOffset));
  lw(scratch2, FieldMemOperand(second, HeapObject::kMapOffset));
  lbu(scratch1, FieldMemOperand(scratch1, Map::kInstanceTypeOffset));
  lbu(scratch2, FieldMemOperand(scratch2, Map::kInstanceTypeOffset));

  JumpIfBothInstanceTypesAreNotSequentialOneByte(scratch1, scratch2, scratch1,
                                                 scratch2, failure);
}


void MacroAssembler::JumpIfNotBothSequentialOneByteStrings(Register first,
                                                           Register second,
                                                           Register scratch1,
                                                           Register scratch2,
                                                           Label* failure) {
  // Check that neither is a smi.
  STATIC_ASSERT(kSmiTag == 0);
  And(scratch1, first, Operand(second));
  JumpIfSmi(scratch1, failure);
  JumpIfNonSmisNotBothSequentialOneByteStrings(first, second, scratch1,
                                               scratch2, failure);
}


void MacroAssembler::JumpIfBothInstanceTypesAreNotSequentialOneByte(
    Register first, Register second, Register scratch1, Register scratch2,
    Label* failure) {
  const int kFlatOneByteStringMask =
      kIsNotStringMask | kStringEncodingMask | kStringRepresentationMask;
  const int kFlatOneByteStringTag =
      kStringTag | kOneByteStringTag | kSeqStringTag;
  DCHECK(kFlatOneByteStringTag <= 0xffff);  // Ensure this fits 16-bit immed.
  andi(scratch1, first, kFlatOneByteStringMask);
  Branch(failure, ne, scratch1, Operand(kFlatOneByteStringTag));
  andi(scratch2, second, kFlatOneByteStringMask);
  Branch(failure, ne, scratch2, Operand(kFlatOneByteStringTag));
}


void MacroAssembler::JumpIfInstanceTypeIsNotSequentialOneByte(Register type,
                                                              Register scratch,
                                                              Label* failure) {
  const int kFlatOneByteStringMask =
      kIsNotStringMask | kStringEncodingMask | kStringRepresentationMask;
  const int kFlatOneByteStringTag =
      kStringTag | kOneByteStringTag | kSeqStringTag;
  And(scratch, type, Operand(kFlatOneByteStringMask));
  Branch(failure, ne, scratch, Operand(kFlatOneByteStringTag));
}


static const int kRegisterPassedArguments = 4;

int MacroAssembler::CalculateStackPassedWords(int num_reg_arguments,
                                              int num_double_arguments) {
  int stack_passed_words = 0;
  num_reg_arguments += 2 * num_double_arguments;

  // Up to four simple arguments are passed in registers a0..a3.
  if (num_reg_arguments > kRegisterPassedArguments) {
    stack_passed_words += num_reg_arguments - kRegisterPassedArguments;
  }
  stack_passed_words += kCArgSlotCount;
  return stack_passed_words;
}


void MacroAssembler::EmitSeqStringSetCharCheck(Register string,
                                               Register index,
                                               Register value,
                                               Register scratch,
                                               uint32_t encoding_mask) {
  Label is_object;
  SmiTst(string, at);
  Check(ne, kNonObject, at, Operand(zero_reg));

  lw(at, FieldMemOperand(string, HeapObject::kMapOffset));
  lbu(at, FieldMemOperand(at, Map::kInstanceTypeOffset));

  andi(at, at, kStringRepresentationMask | kStringEncodingMask);
  li(scratch, Operand(encoding_mask));
  Check(eq, kUnexpectedStringType, at, Operand(scratch));

  // The index is assumed to be untagged coming in, tag it to compare with the
  // string length without using a temp register, it is restored at the end of
  // this function.
  Label index_tag_ok, index_tag_bad;
  TrySmiTag(index, scratch, &index_tag_bad);
  Branch(&index_tag_ok);
  bind(&index_tag_bad);
  Abort(kIndexIsTooLarge);
  bind(&index_tag_ok);

  lw(at, FieldMemOperand(string, String::kLengthOffset));
  Check(lt, kIndexIsTooLarge, index, Operand(at));

  DCHECK(Smi::FromInt(0) == 0);
  Check(ge, kIndexIsNegative, index, Operand(zero_reg));

  SmiUntag(index, index);
}


void MacroAssembler::PrepareCallCFunction(int num_reg_arguments,
                                          int num_double_arguments,
                                          Register scratch) {
  int frame_alignment = ActivationFrameAlignment();

  // Up to four simple arguments are passed in registers a0..a3.
  // Those four arguments must have reserved argument slots on the stack for
  // mips, even though those argument slots are not normally used.
  // Remaining arguments are pushed on the stack, above (higher address than)
  // the argument slots.
  int stack_passed_arguments = CalculateStackPassedWords(
      num_reg_arguments, num_double_arguments);
  if (frame_alignment > kPointerSize) {
    // Make stack end at alignment and make room for num_arguments - 4 words
    // and the original value of sp.
    mov(scratch, sp);
    Subu(sp, sp, Operand((stack_passed_arguments + 1) * kPointerSize));
    DCHECK(base::bits::IsPowerOfTwo32(frame_alignment));
    And(sp, sp, Operand(-frame_alignment));
    sw(scratch, MemOperand(sp, stack_passed_arguments * kPointerSize));
  } else {
    Subu(sp, sp, Operand(stack_passed_arguments * kPointerSize));
  }
}


void MacroAssembler::PrepareCallCFunction(int num_reg_arguments,
                                          Register scratch) {
  PrepareCallCFunction(num_reg_arguments, 0, scratch);
}


void MacroAssembler::CallCFunction(ExternalReference function,
                                   int num_reg_arguments,
                                   int num_double_arguments) {
  li(t8, Operand(function));
  CallCFunctionHelper(t8, num_reg_arguments, num_double_arguments);
}


void MacroAssembler::CallCFunction(Register function,
                                   int num_reg_arguments,
                                   int num_double_arguments) {
  CallCFunctionHelper(function, num_reg_arguments, num_double_arguments);
}


void MacroAssembler::CallCFunction(ExternalReference function,
                                   int num_arguments) {
  CallCFunction(function, num_arguments, 0);
}


void MacroAssembler::CallCFunction(Register function,
                                   int num_arguments) {
  CallCFunction(function, num_arguments, 0);
}


void MacroAssembler::CallCFunctionHelper(Register function,
                                         int num_reg_arguments,
                                         int num_double_arguments) {
  DCHECK(has_frame());
  // Make sure that the stack is aligned before calling a C function unless
  // running in the simulator. The simulator has its own alignment check which
  // provides more information.
  // The argument stots are presumed to have been set up by
  // PrepareCallCFunction. The C function must be called via t9, for mips ABI.

#if V8_HOST_ARCH_MIPS
  if (emit_debug_code()) {
    int frame_alignment = base::OS::ActivationFrameAlignment();
    int frame_alignment_mask = frame_alignment - 1;
    if (frame_alignment > kPointerSize) {
      DCHECK(base::bits::IsPowerOfTwo32(frame_alignment));
      Label alignment_as_expected;
      And(at, sp, Operand(frame_alignment_mask));
      Branch(&alignment_as_expected, eq, at, Operand(zero_reg));
      // Don't use Check here, as it will call Runtime_Abort possibly
      // re-entering here.
      stop("Unexpected alignment in CallCFunction");
      bind(&alignment_as_expected);
    }
  }
#endif  // V8_HOST_ARCH_MIPS

  // Just call directly. The function called cannot cause a GC, or
  // allow preemption, so the return address in the link register
  // stays correct.

  if (!function.is(t9)) {
    mov(t9, function);
    function = t9;
  }

  Call(function);

  int stack_passed_arguments = CalculateStackPassedWords(
      num_reg_arguments, num_double_arguments);

  if (base::OS::ActivationFrameAlignment() > kPointerSize) {
    lw(sp, MemOperand(sp, stack_passed_arguments * kPointerSize));
  } else {
    Addu(sp, sp, Operand(stack_passed_arguments * kPointerSize));
  }
}


#undef BRANCH_ARGS_CHECK


void MacroAssembler::CheckPageFlag(
    Register object,
    Register scratch,
    int mask,
    Condition cc,
    Label* condition_met) {
  And(scratch, object, Operand(~Page::kPageAlignmentMask));
  lw(scratch, MemOperand(scratch, MemoryChunk::kFlagsOffset));
  And(scratch, scratch, Operand(mask));
  Branch(condition_met, cc, scratch, Operand(zero_reg));
}


void MacroAssembler::JumpIfBlack(Register object,
                                 Register scratch0,
                                 Register scratch1,
                                 Label* on_black) {
  HasColor(object, scratch0, scratch1, on_black, 1, 1);  // kBlackBitPattern.
  DCHECK(strcmp(Marking::kBlackBitPattern, "11") == 0);
}


void MacroAssembler::HasColor(Register object,
                              Register bitmap_scratch,
                              Register mask_scratch,
                              Label* has_color,
                              int first_bit,
                              int second_bit) {
  DCHECK(!AreAliased(object, bitmap_scratch, mask_scratch, t8));
  DCHECK(!AreAliased(object, bitmap_scratch, mask_scratch, t9));

  GetMarkBits(object, bitmap_scratch, mask_scratch);

  Label other_color, word_boundary;
  lw(t9, MemOperand(bitmap_scratch, MemoryChunk::kHeaderSize));
  And(t8, t9, Operand(mask_scratch));
  Branch(&other_color, first_bit == 1 ? eq : ne, t8, Operand(zero_reg));
  // Shift left 1 by adding.
  Addu(mask_scratch, mask_scratch, Operand(mask_scratch));
  Branch(&word_boundary, eq, mask_scratch, Operand(zero_reg));
  And(t8, t9, Operand(mask_scratch));
  Branch(has_color, second_bit == 1 ? ne : eq, t8, Operand(zero_reg));
  jmp(&other_color);

  bind(&word_boundary);
  lw(t9, MemOperand(bitmap_scratch, MemoryChunk::kHeaderSize + kPointerSize));
  And(t9, t9, Operand(1));
  Branch(has_color, second_bit == 1 ? ne : eq, t9, Operand(zero_reg));
  bind(&other_color);
}


void MacroAssembler::GetMarkBits(Register addr_reg,
                                 Register bitmap_reg,
                                 Register mask_reg) {
  DCHECK(!AreAliased(addr_reg, bitmap_reg, mask_reg, no_reg));
  And(bitmap_reg, addr_reg, Operand(~Page::kPageAlignmentMask));
  Ext(mask_reg, addr_reg, kPointerSizeLog2, Bitmap::kBitsPerCellLog2);
  const int kLowBits = kPointerSizeLog2 + Bitmap::kBitsPerCellLog2;
  Ext(t8, addr_reg, kLowBits, kPageSizeBits - kLowBits);
  Lsa(bitmap_reg, bitmap_reg, t8, kPointerSizeLog2, t8);
  li(t8, Operand(1));
  sllv(mask_reg, t8, mask_reg);
}


void MacroAssembler::JumpIfWhite(Register value, Register bitmap_scratch,
                                 Register mask_scratch, Register load_scratch,
                                 Label* value_is_white) {
  DCHECK(!AreAliased(value, bitmap_scratch, mask_scratch, t8));
  GetMarkBits(value, bitmap_scratch, mask_scratch);

  // If the value is black or grey we don't need to do anything.
  DCHECK(strcmp(Marking::kWhiteBitPattern, "00") == 0);
  DCHECK(strcmp(Marking::kBlackBitPattern, "11") == 0);
  DCHECK(strcmp(Marking::kGreyBitPattern, "10") == 0);
  DCHECK(strcmp(Marking::kImpossibleBitPattern, "01") == 0);

  // Since both black and grey have a 1 in the first position and white does
  // not have a 1 there we only need to check one bit.
  lw(load_scratch, MemOperand(bitmap_scratch, MemoryChunk::kHeaderSize));
  And(t8, mask_scratch, load_scratch);
  Branch(value_is_white, eq, t8, Operand(zero_reg));
}


void MacroAssembler::LoadInstanceDescriptors(Register map,
                                             Register descriptors) {
  lw(descriptors, FieldMemOperand(map, Map::kDescriptorsOffset));
}


void MacroAssembler::NumberOfOwnDescriptors(Register dst, Register map) {
  lw(dst, FieldMemOperand(map, Map::kBitField3Offset));
  DecodeField<Map::NumberOfOwnDescriptorsBits>(dst);
}


void MacroAssembler::EnumLength(Register dst, Register map) {
  STATIC_ASSERT(Map::EnumLengthBits::kShift == 0);
  lw(dst, FieldMemOperand(map, Map::kBitField3Offset));
  And(dst, dst, Operand(Map::EnumLengthBits::kMask));
  SmiTag(dst);
}


void MacroAssembler::LoadAccessor(Register dst, Register holder,
                                  int accessor_index,
                                  AccessorComponent accessor) {
  lw(dst, FieldMemOperand(holder, HeapObject::kMapOffset));
  LoadInstanceDescriptors(dst, dst);
  lw(dst,
     FieldMemOperand(dst, DescriptorArray::GetValueOffset(accessor_index)));
  int offset = accessor == ACCESSOR_GETTER ? AccessorPair::kGetterOffset
                                           : AccessorPair::kSetterOffset;
  lw(dst, FieldMemOperand(dst, offset));
}


void MacroAssembler::CheckEnumCache(Label* call_runtime) {
  Register null_value = t1;
  Register  empty_fixed_array_value = t2;
  LoadRoot(empty_fixed_array_value, Heap::kEmptyFixedArrayRootIndex);
  Label next, start;
  mov(a2, a0);

  // Check if the enum length field is properly initialized, indicating that
  // there is an enum cache.
  lw(a1, FieldMemOperand(a2, HeapObject::kMapOffset));

  EnumLength(a3, a1);
  Branch(
      call_runtime, eq, a3, Operand(Smi::FromInt(kInvalidEnumCacheSentinel)));

  LoadRoot(null_value, Heap::kNullValueRootIndex);
  jmp(&start);

  bind(&next);
  lw(a1, FieldMemOperand(a2, HeapObject::kMapOffset));

  // For all objects but the receiver, check that the cache is empty.
  EnumLength(a3, a1);
  Branch(call_runtime, ne, a3, Operand(Smi::FromInt(0)));

  bind(&start);

  // Check that there are no elements. Register a2 contains the current JS
  // object we've reached through the prototype chain.
  Label no_elements;
  lw(a2, FieldMemOperand(a2, JSObject::kElementsOffset));
  Branch(&no_elements, eq, a2, Operand(empty_fixed_array_value));

  // Second chance, the object may be using the empty slow element dictionary.
  LoadRoot(at, Heap::kEmptySlowElementDictionaryRootIndex);
  Branch(call_runtime, ne, a2, Operand(at));

  bind(&no_elements);
  lw(a2, FieldMemOperand(a1, Map::kPrototypeOffset));
  Branch(&next, ne, a2, Operand(null_value));
}


void MacroAssembler::ClampUint8(Register output_reg, Register input_reg) {
  DCHECK(!output_reg.is(input_reg));
  Label done;
  li(output_reg, Operand(255));
  // Normal branch: nop in delay slot.
  Branch(&done, gt, input_reg, Operand(output_reg));
  // Use delay slot in this branch.
  Branch(USE_DELAY_SLOT, &done, lt, input_reg, Operand(zero_reg));
  mov(output_reg, zero_reg);  // In delay slot.
  mov(output_reg, input_reg);  // Value is in range 0..255.
  bind(&done);
}


void MacroAssembler::ClampDoubleToUint8(Register result_reg,
                                        DoubleRegister input_reg,
                                        DoubleRegister temp_double_reg) {
  Label above_zero;
  Label done;
  Label in_bounds;

  Move(temp_double_reg, 0.0);
  BranchF(&above_zero, NULL, gt, input_reg, temp_double_reg);

  // Double value is less than zero, NaN or Inf, return 0.
  mov(result_reg, zero_reg);
  Branch(&done);

  // Double value is >= 255, return 255.
  bind(&above_zero);
  Move(temp_double_reg, 255.0);
  BranchF(&in_bounds, NULL, le, input_reg, temp_double_reg);
  li(result_reg, Operand(255));
  Branch(&done);

  // In 0-255 range, round and truncate.
  bind(&in_bounds);
  cvt_w_d(temp_double_reg, input_reg);
  mfc1(result_reg, temp_double_reg);
  bind(&done);
}


void MacroAssembler::TestJSArrayForAllocationMemento(
    Register receiver_reg,
    Register scratch_reg,
    Label* no_memento_found,
    Condition cond,
    Label* allocation_memento_present) {
  ExternalReference new_space_start =
      ExternalReference::new_space_start(isolate());
  ExternalReference new_space_allocation_top =
      ExternalReference::new_space_allocation_top_address(isolate());
  Addu(scratch_reg, receiver_reg,
       Operand(JSArray::kSize + AllocationMemento::kSize - kHeapObjectTag));
  Branch(no_memento_found, lt, scratch_reg, Operand(new_space_start));
  li(at, Operand(new_space_allocation_top));
  lw(at, MemOperand(at));
  Branch(no_memento_found, gt, scratch_reg, Operand(at));
  lw(scratch_reg, MemOperand(scratch_reg, -AllocationMemento::kSize));
  if (allocation_memento_present) {
    Branch(allocation_memento_present, cond, scratch_reg,
           Operand(isolate()->factory()->allocation_memento_map()));
  }
}


Register GetRegisterThatIsNotOneOf(Register reg1,
                                   Register reg2,
                                   Register reg3,
                                   Register reg4,
                                   Register reg5,
                                   Register reg6) {
  RegList regs = 0;
  if (reg1.is_valid()) regs |= reg1.bit();
  if (reg2.is_valid()) regs |= reg2.bit();
  if (reg3.is_valid()) regs |= reg3.bit();
  if (reg4.is_valid()) regs |= reg4.bit();
  if (reg5.is_valid()) regs |= reg5.bit();
  if (reg6.is_valid()) regs |= reg6.bit();

  const RegisterConfiguration* config =
      RegisterConfiguration::ArchDefault(RegisterConfiguration::CRANKSHAFT);
  for (int i = 0; i < config->num_allocatable_general_registers(); ++i) {
    int code = config->GetAllocatableGeneralCode(i);
    Register candidate = Register::from_code(code);
    if (regs & candidate.bit()) continue;
    return candidate;
  }
  UNREACHABLE();
  return no_reg;
}


void MacroAssembler::JumpIfDictionaryInPrototypeChain(
    Register object,
    Register scratch0,
    Register scratch1,
    Label* found) {
  DCHECK(!scratch1.is(scratch0));
  Factory* factory = isolate()->factory();
  Register current = scratch0;
  Label loop_again, end;

  // Scratch contained elements pointer.
  Move(current, object);
  lw(current, FieldMemOperand(current, HeapObject::kMapOffset));
  lw(current, FieldMemOperand(current, Map::kPrototypeOffset));
  Branch(&end, eq, current, Operand(factory->null_value()));

  // Loop based on the map going up the prototype chain.
  bind(&loop_again);
  lw(current, FieldMemOperand(current, HeapObject::kMapOffset));
  lbu(scratch1, FieldMemOperand(current, Map::kInstanceTypeOffset));
  STATIC_ASSERT(JS_VALUE_TYPE < JS_OBJECT_TYPE);
  STATIC_ASSERT(JS_PROXY_TYPE < JS_OBJECT_TYPE);
  Branch(found, lo, scratch1, Operand(JS_OBJECT_TYPE));
  lb(scratch1, FieldMemOperand(current, Map::kBitField2Offset));
  DecodeField<Map::ElementsKindBits>(scratch1);
  Branch(found, eq, scratch1, Operand(DICTIONARY_ELEMENTS));
  lw(current, FieldMemOperand(current, Map::kPrototypeOffset));
  Branch(&loop_again, ne, current, Operand(factory->null_value()));

  bind(&end);
}


bool AreAliased(Register reg1, Register reg2, Register reg3, Register reg4,
                Register reg5, Register reg6, Register reg7, Register reg8,
                Register reg9, Register reg10) {
  int n_of_valid_regs = reg1.is_valid() + reg2.is_valid() + reg3.is_valid() +
                        reg4.is_valid() + reg5.is_valid() + reg6.is_valid() +
                        reg7.is_valid() + reg8.is_valid() + reg9.is_valid() +
                        reg10.is_valid();

  RegList regs = 0;
  if (reg1.is_valid()) regs |= reg1.bit();
  if (reg2.is_valid()) regs |= reg2.bit();
  if (reg3.is_valid()) regs |= reg3.bit();
  if (reg4.is_valid()) regs |= reg4.bit();
  if (reg5.is_valid()) regs |= reg5.bit();
  if (reg6.is_valid()) regs |= reg6.bit();
  if (reg7.is_valid()) regs |= reg7.bit();
  if (reg8.is_valid()) regs |= reg8.bit();
  if (reg9.is_valid()) regs |= reg9.bit();
  if (reg10.is_valid()) regs |= reg10.bit();
  int n_of_non_aliasing_regs = NumRegs(regs);

  return n_of_valid_regs != n_of_non_aliasing_regs;
}


CodePatcher::CodePatcher(Isolate* isolate, byte* address, int instructions,
                         FlushICache flush_cache)
    : address_(address),
      size_(instructions * Assembler::kInstrSize),
      masm_(isolate, address, size_ + Assembler::kGap, CodeObjectRequired::kNo),
      flush_cache_(flush_cache) {
  // Create a new macro assembler pointing to the address of the code to patch.
  // The size is adjusted with kGap on order for the assembler to generate size
  // bytes of instructions without failing with buffer size constraints.
  DCHECK(masm_.reloc_info_writer.pos() == address_ + size_ + Assembler::kGap);
}


CodePatcher::~CodePatcher() {
  // Indicate that code has changed.
  if (flush_cache_ == FLUSH) {
    Assembler::FlushICache(masm_.isolate(), address_, size_);
  }

  // Check that the code was patched as expected.
  DCHECK(masm_.pc_ == address_ + size_);
  DCHECK(masm_.reloc_info_writer.pos() == address_ + size_ + Assembler::kGap);
}


void CodePatcher::Emit(Instr instr) {
  masm()->emit(instr);
}


void CodePatcher::Emit(Address addr) {
  masm()->emit(reinterpret_cast<Instr>(addr));
}


void CodePatcher::ChangeBranchCondition(Instr current_instr,
                                        uint32_t new_opcode) {
  current_instr = (current_instr & ~kOpcodeMask) | new_opcode;
  masm_.emit(current_instr);
}


void MacroAssembler::TruncatingDiv(Register result,
                                   Register dividend,
                                   int32_t divisor) {
  DCHECK(!dividend.is(result));
  DCHECK(!dividend.is(at));
  DCHECK(!result.is(at));
  base::MagicNumbersForDivision<uint32_t> mag =
      base::SignedDivisionByConstant(static_cast<uint32_t>(divisor));
  li(at, Operand(mag.multiplier));
  Mulh(result, dividend, Operand(at));
  bool neg = (mag.multiplier & (static_cast<uint32_t>(1) << 31)) != 0;
  if (divisor > 0 && neg) {
    Addu(result, result, Operand(dividend));
  }
  if (divisor < 0 && !neg && mag.multiplier > 0) {
    Subu(result, result, Operand(dividend));
  }
  if (mag.shift > 0) sra(result, result, mag.shift);
  srl(at, dividend, 31);
  Addu(result, result, Operand(at));
}


}  // namespace internal
}  // namespace v8

#endif  // V8_TARGET_ARCH_MIPS
