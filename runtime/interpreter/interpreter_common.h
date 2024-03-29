/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_RUNTIME_INTERPRETER_INTERPRETER_COMMON_H_
#define ART_RUNTIME_INTERPRETER_INTERPRETER_COMMON_H_

#include "interpreter.h"
#include "interpreter_intrinsics.h"

#include <math.h>

#include <atomic>
#include <iostream>
#include <sstream>

#include <android-base/logging.h>
#include <android-base/stringprintf.h>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/enums.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "class_linker-inl.h"
#include "common_dex_operations.h"
#include "common_throws.h"
#include "dex/dex_file-inl.h"
#include "dex/dex_instruction-inl.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "handle_scope-inl.h"
#include "interpreter_mterp_impl.h"
#include "interpreter_switch_impl.h"
#include "jit/jit.h"
#include "mirror/call_site.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache.h"
#include "mirror/method.h"
#include "mirror/method_handles_lookup.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "mterp/mterp.h"
#include "obj_ptr.h"
#include "stack.h"
#include "thread.h"
#include "unstarted_runtime.h"
#include "well_known_classes.h"

namespace art {
namespace interpreter {

void ThrowNullPointerExceptionFromInterpreter()
    REQUIRES_SHARED(Locks::mutator_lock_);

template <bool kMonitorCounting>
static inline void DoMonitorEnter(Thread* self, ShadowFrame* frame, ObjPtr<mirror::Object> ref)
    NO_THREAD_SAFETY_ANALYSIS
    REQUIRES(!Roles::uninterruptible_) {
  DCHECK(!ref.IsNull());
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> h_ref(hs.NewHandle(ref));
  h_ref->MonitorEnter(self);
  DCHECK(self->HoldsLock(h_ref.Get()));
  if (UNLIKELY(self->IsExceptionPending())) {
    bool unlocked = h_ref->MonitorExit(self);
    DCHECK(unlocked);
    return;
  }
  if (kMonitorCounting && frame->GetMethod()->MustCountLocks()) {
    frame->GetLockCountData().AddMonitor(self, h_ref.Get());
  }
}

template <bool kMonitorCounting>
static inline void DoMonitorExit(Thread* self, ShadowFrame* frame, ObjPtr<mirror::Object> ref)
    NO_THREAD_SAFETY_ANALYSIS
    REQUIRES(!Roles::uninterruptible_) {
  StackHandleScope<1> hs(self);
  Handle<mirror::Object> h_ref(hs.NewHandle(ref));
  h_ref->MonitorExit(self);
  if (kMonitorCounting && frame->GetMethod()->MustCountLocks()) {
    frame->GetLockCountData().RemoveMonitorOrThrow(self, h_ref.Get());
  }
}

template <bool kMonitorCounting>
static inline bool DoMonitorCheckOnExit(Thread* self, ShadowFrame* frame)
    NO_THREAD_SAFETY_ANALYSIS
    REQUIRES(!Roles::uninterruptible_) {
  if (kMonitorCounting && frame->GetMethod()->MustCountLocks()) {
    return frame->GetLockCountData().CheckAllMonitorsReleasedOrThrow(self);
  }
  return true;
}

void AbortTransactionF(Thread* self, const char* fmt, ...)
    __attribute__((__format__(__printf__, 2, 3)))
    REQUIRES_SHARED(Locks::mutator_lock_);

void AbortTransactionV(Thread* self, const char* fmt, va_list args)
    REQUIRES_SHARED(Locks::mutator_lock_);

void RecordArrayElementsInTransaction(ObjPtr<mirror::Array> array, int32_t count)
    REQUIRES_SHARED(Locks::mutator_lock_);

// Invokes the given method. This is part of the invocation support and is used by DoInvoke,
// DoFastInvoke and DoInvokeVirtualQuick functions.
// Returns true on success, otherwise throws an exception and returns false.
template<bool is_range, bool do_assignability_check>
bool DoCall(ArtMethod* called_method, Thread* self, ShadowFrame& shadow_frame,
            const Instruction* inst, uint16_t inst_data, JValue* result);

template<InvokeType type>
static ALWAYS_INLINE bool UseInterpreterToInterpreterFastPath(ArtMethod* method)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  Runtime* runtime = Runtime::Current();
  if (!runtime->IsStarted()) {
    return false;
  }
  const void* quick_code = method->GetEntryPointFromQuickCompiledCode();
  if (!runtime->GetClassLinker()->IsQuickToInterpreterBridge(quick_code)) {
    return false;
  }
  if (!method->SkipAccessChecks() || method->IsNative() || method->IsProxyMethod()) {
    return false;
  }
  if (method->GetDeclaringClass()->IsStringClass() && method->IsConstructor()) {
    return false;
  }
  if (type == kStatic && !method->GetDeclaringClass()->IsInitialized()) {
    return false;
  }
  if (runtime->IsActiveTransaction() || runtime->GetInstrumentation()->HasMethodEntryListeners()) {
    return false;
  }
  ProfilingInfo* profiling_info = method->GetProfilingInfo(kRuntimePointerSize);
  if ((profiling_info != nullptr) && (profiling_info->GetSavedEntryPoint() != nullptr)) {
    return false;
  }
  if (runtime->GetJit() != nullptr && runtime->GetJit()->JitAtFirstUse()) {
    return false;
  }
  return true;
}

// Throws exception if we are getting close to the end of the stack.
NO_INLINE bool CheckStackOverflow(Thread* self, size_t frame_size)
    REQUIRES_SHARED(Locks::mutator_lock_);

// Handles all invoke-XXX/range instructions except for invoke-polymorphic[/range].
// Returns true on success, otherwise throws an exception and returns false.
template<InvokeType type, bool is_range, bool do_access_check, bool is_mterp>
static ALWAYS_INLINE bool DoInvoke(Thread* self,
                                   ShadowFrame& shadow_frame,
                                   const Instruction* inst,
                                   uint16_t inst_data,
                                   JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  // Make sure to check for async exceptions before anything else.
  if (UNLIKELY(self->ObserveAsyncException())) {
    return false;
  }
  const uint32_t method_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
  const uint32_t vregC = (is_range) ? inst->VRegC_3rc() : inst->VRegC_35c();
  ObjPtr<mirror::Object> receiver =
      (type == kStatic) ? nullptr : shadow_frame.GetVRegReference(vregC);
  ArtMethod* sf_method = shadow_frame.GetMethod();
  ArtMethod* const called_method = FindMethodFromCode<type, do_access_check>(
      method_idx, &receiver, sf_method, self);
  if (UNLIKELY(called_method == nullptr)) {
    CHECK(self->IsExceptionPending());
    result->SetJ(0);
    return false;
  }
  if (UNLIKELY(!called_method->IsInvokable())) {
    called_method->ThrowInvocationTimeError();
    result->SetJ(0);
    return false;
  }

  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr && (type == kVirtual || type == kInterface)) {
    jit->InvokeVirtualOrInterface(receiver, sf_method, shadow_frame.GetDexPC(), called_method);
  }

  if (is_mterp && !is_range && called_method->IsIntrinsic()) {
    if (type == kDirect || type == kStatic || type == kVirtual) {
      if (MterpHandleIntrinsic(&shadow_frame, called_method, inst, inst_data,
                               shadow_frame.GetResultRegister())) {
        return !self->IsExceptionPending();
      }
    } else {
      // TODO: Remove the InvokeVirtualOrInterface instrumentation, as it was only used by the JIT.
      if (type == kVirtual || type == kInterface) {
        instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
        if (UNLIKELY(instrumentation->HasInvokeVirtualOrInterfaceListeners())) {
          instrumentation->InvokeVirtualOrInterface(
              self, receiver.Ptr(), sf_method, shadow_frame.GetDexPC(), called_method);
        }
      }
    }
  }

  if (is_mterp && UseInterpreterToInterpreterFastPath<type>(called_method)) {
    const uint16_t number_of_inputs =
        (is_range) ? inst->VRegA_3rc(inst_data) : inst->VRegA_35c(inst_data);
    CodeItemDataAccessor accessor(called_method->DexInstructionData());
    uint32_t num_regs = accessor.RegistersSize();
    DCHECK_EQ(number_of_inputs, accessor.InsSize());
    DCHECK_GE(num_regs, number_of_inputs);
    size_t first_dest_reg = num_regs - number_of_inputs;

    if (UNLIKELY(!CheckStackOverflow(self, ShadowFrame::ComputeSize(num_regs)))) {
      return false;
    }

    if (jit != nullptr) {
      jit->AddSamples(self, called_method, 1, /* with_backedges */false);
    }

    // Create shadow frame on the stack.
    const char* old_cause = self->StartAssertNoThreadSuspension("DoFastInvoke");
    ShadowFrameAllocaUniquePtr shadow_frame_unique_ptr =
        CREATE_SHADOW_FRAME(num_regs, &shadow_frame, called_method, /* dex pc */ 0);
    ShadowFrame* new_shadow_frame = shadow_frame_unique_ptr.get();
    if (is_range) {
      size_t src = vregC;
      for (size_t i = 0, dst = first_dest_reg; i < number_of_inputs; ++i, ++dst, ++src) {
        *new_shadow_frame->GetVRegAddr(dst) = *shadow_frame.GetVRegAddr(src);
        *new_shadow_frame->GetShadowRefAddr(dst) = *shadow_frame.GetShadowRefAddr(src);
      }
    } else {
      uint32_t arg[Instruction::kMaxVarArgRegs];
      inst->GetVarArgs(arg, inst_data);
      for (size_t i = 0, dst = first_dest_reg; i < number_of_inputs; ++i, ++dst) {
        *new_shadow_frame->GetVRegAddr(dst) = *shadow_frame.GetVRegAddr(arg[i]);
        *new_shadow_frame->GetShadowRefAddr(dst) = *shadow_frame.GetShadowRefAddr(arg[i]);
      }
    }
    self->PushShadowFrame(new_shadow_frame);
    self->EndAssertNoThreadSuspension(old_cause);

    DCheckStaticState(self, called_method);
    while (true) {
      // Mterp does not support all instrumentation/debugging.
      if (MterpShouldSwitchInterpreters() != 0) {
        *result =
            ExecuteSwitchImpl<false, false>(self, accessor, *new_shadow_frame, *result, false);
        break;
      }
      if (ExecuteMterpImpl(self, accessor.Insns(), new_shadow_frame, result)) {
        break;
      } else {
        // Mterp didn't like that instruction.  Single-step it with the reference interpreter.
        *result = ExecuteSwitchImpl<false, false>(self, accessor, *new_shadow_frame, *result, true);
        if (new_shadow_frame->GetDexPC() == dex::kDexNoIndex) {
          break;  // Single-stepped a return or an exception not handled locally.
        }
      }
    }
    self->PopShadowFrame();

    return !self->IsExceptionPending();
  }

  return DoCall<is_range, do_access_check>(called_method, self, shadow_frame, inst, inst_data,
                                           result);
}

static inline ObjPtr<mirror::MethodHandle> ResolveMethodHandle(Thread* self,
                                                               uint32_t method_handle_index,
                                                               ArtMethod* referrer)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->ResolveMethodHandle(self, method_handle_index, referrer);
}

static inline ObjPtr<mirror::MethodType> ResolveMethodType(Thread* self,
                                                           dex::ProtoIndex method_type_index,
                                                           ArtMethod* referrer)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return class_linker->ResolveMethodType(self, method_type_index, referrer);
}

#define DECLARE_SIGNATURE_POLYMORPHIC_HANDLER(Name, ...)              \
bool Do ## Name(Thread* self,                                         \
                ShadowFrame& shadow_frame,                            \
                const Instruction* inst,                              \
                uint16_t inst_data,                                   \
                JValue* result) REQUIRES_SHARED(Locks::mutator_lock_);
#include "intrinsics_list.h"
INTRINSICS_LIST(DECLARE_SIGNATURE_POLYMORPHIC_HANDLER)
#undef INTRINSICS_LIST
#undef DECLARE_SIGNATURE_POLYMORPHIC_HANDLER

// Performs a invoke-polymorphic or invoke-polymorphic-range.
template<bool is_range>
bool DoInvokePolymorphic(Thread* self,
                         ShadowFrame& shadow_frame,
                         const Instruction* inst,
                         uint16_t inst_data,
                         JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_);

bool DoInvokeCustom(Thread* self,
                    ShadowFrame& shadow_frame,
                    uint32_t call_site_idx,
                    const InstructionOperands* operands,
                    JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_);

// Performs a custom invoke (invoke-custom/invoke-custom-range).
template<bool is_range>
bool DoInvokeCustom(Thread* self,
                    ShadowFrame& shadow_frame,
                    const Instruction* inst,
                    uint16_t inst_data,
                    JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const uint32_t call_site_idx = is_range ? inst->VRegB_3rc() : inst->VRegB_35c();
  if (is_range) {
    RangeInstructionOperands operands(inst->VRegC_3rc(), inst->VRegA_3rc());
    return DoInvokeCustom(self, shadow_frame, call_site_idx, &operands, result);
  } else {
    uint32_t args[Instruction::kMaxVarArgRegs];
    inst->GetVarArgs(args, inst_data);
    VarArgsInstructionOperands operands(args, inst->VRegA_35c());
    return DoInvokeCustom(self, shadow_frame, call_site_idx, &operands, result);
  }
}

// Handles invoke-virtual-quick and invoke-virtual-quick-range instructions.
// Returns true on success, otherwise throws an exception and returns false.
template<bool is_range>
static inline bool DoInvokeVirtualQuick(Thread* self, ShadowFrame& shadow_frame,
                                        const Instruction* inst, uint16_t inst_data,
                                        JValue* result)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const uint32_t vregC = (is_range) ? inst->VRegC_3rc() : inst->VRegC_35c();
  ObjPtr<mirror::Object> const receiver = shadow_frame.GetVRegReference(vregC);
  if (UNLIKELY(receiver == nullptr)) {
    // We lost the reference to the method index so we cannot get a more
    // precised exception message.
    ThrowNullPointerExceptionFromDexPC();
    return false;
  }
  const uint32_t vtable_idx = (is_range) ? inst->VRegB_3rc() : inst->VRegB_35c();
  CHECK(receiver->GetClass()->ShouldHaveEmbeddedVTable());
  ArtMethod* const called_method = receiver->GetClass()->GetEmbeddedVTableEntry(
      vtable_idx, Runtime::Current()->GetClassLinker()->GetImagePointerSize());
  if (UNLIKELY(called_method == nullptr)) {
    CHECK(self->IsExceptionPending());
    result->SetJ(0);
    return false;
  } else if (UNLIKELY(!called_method->IsInvokable())) {
    called_method->ThrowInvocationTimeError();
    result->SetJ(0);
    return false;
  } else {
    jit::Jit* jit = Runtime::Current()->GetJit();
    if (jit != nullptr) {
      jit->InvokeVirtualOrInterface(
          receiver, shadow_frame.GetMethod(), shadow_frame.GetDexPC(), called_method);
      jit->AddSamples(self, shadow_frame.GetMethod(), 1, /*with_backedges*/false);
    }
    instrumentation::Instrumentation* instrumentation = Runtime::Current()->GetInstrumentation();
    // TODO: Remove the InvokeVirtualOrInterface instrumentation, as it was only used by the JIT.
    if (UNLIKELY(instrumentation->HasInvokeVirtualOrInterfaceListeners())) {
      instrumentation->InvokeVirtualOrInterface(
          self, receiver.Ptr(), shadow_frame.GetMethod(), shadow_frame.GetDexPC(), called_method);
    }
    // No need to check since we've been quickened.
    return DoCall<is_range, false>(called_method, self, shadow_frame, inst, inst_data, result);
  }
}

// Handles iget-XXX and sget-XXX instructions.
// Returns true on success, otherwise throws an exception and returns false.
template<FindFieldType find_type, Primitive::Type field_type, bool do_access_check,
         bool transaction_active = false>
bool DoFieldGet(Thread* self, ShadowFrame& shadow_frame, const Instruction* inst,
                uint16_t inst_data) REQUIRES_SHARED(Locks::mutator_lock_);

// Handles iget-quick, iget-wide-quick and iget-object-quick instructions.
// Returns true on success, otherwise throws an exception and returns false.
template<Primitive::Type field_type>
bool DoIGetQuick(ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data)
    REQUIRES_SHARED(Locks::mutator_lock_);

// Handles iput-XXX and sput-XXX instructions.
// Returns true on success, otherwise throws an exception and returns false.
template<FindFieldType find_type, Primitive::Type field_type, bool do_access_check,
         bool transaction_active>
bool DoFieldPut(Thread* self, const ShadowFrame& shadow_frame, const Instruction* inst,
                uint16_t inst_data) REQUIRES_SHARED(Locks::mutator_lock_);

// Handles iput-quick, iput-wide-quick and iput-object-quick instructions.
// Returns true on success, otherwise throws an exception and returns false.
template<Primitive::Type field_type, bool transaction_active>
bool DoIPutQuick(const ShadowFrame& shadow_frame, const Instruction* inst, uint16_t inst_data)
    REQUIRES_SHARED(Locks::mutator_lock_);


// Handles string resolution for const-string and const-string-jumbo instructions. Also ensures the
// java.lang.String class is initialized.
static inline ObjPtr<mirror::String> ResolveString(Thread* self,
                                                   ShadowFrame& shadow_frame,
                                                   dex::StringIndex string_idx)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  ObjPtr<mirror::Class> java_lang_string_class = mirror::String::GetJavaLangString();
  if (UNLIKELY(!java_lang_string_class->IsInitialized())) {
    ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
    StackHandleScope<1> hs(self);
    Handle<mirror::Class> h_class(hs.NewHandle(java_lang_string_class));
    if (UNLIKELY(!class_linker->EnsureInitialized(self, h_class, true, true))) {
      DCHECK(self->IsExceptionPending());
      return nullptr;
    }
  }
  ArtMethod* method = shadow_frame.GetMethod();
  ObjPtr<mirror::String> string_ptr = method->GetDexCache()->GetResolvedString(string_idx);
  if (UNLIKELY(string_ptr == nullptr)) {
    StackHandleScope<1> hs(self);
    Handle<mirror::DexCache> dex_cache(hs.NewHandle(method->GetDexCache()));
    string_ptr = Runtime::Current()->GetClassLinker()->ResolveString(string_idx, dex_cache);
  }
  return string_ptr;
}

// Handles div-int, div-int/2addr, div-int/li16 and div-int/lit8 instructions.
// Returns true on success, otherwise throws a java.lang.ArithmeticException and return false.
static inline bool DoIntDivide(ShadowFrame& shadow_frame, size_t result_reg,
                               int32_t dividend, int32_t divisor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  constexpr int32_t kMinInt = std::numeric_limits<int32_t>::min();
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero();
    return false;
  }
  if (UNLIKELY(dividend == kMinInt && divisor == -1)) {
    shadow_frame.SetVReg(result_reg, kMinInt);
  } else {
    shadow_frame.SetVReg(result_reg, dividend / divisor);
  }
  return true;
}

// Handles rem-int, rem-int/2addr, rem-int/li16 and rem-int/lit8 instructions.
// Returns true on success, otherwise throws a java.lang.ArithmeticException and return false.
static inline bool DoIntRemainder(ShadowFrame& shadow_frame, size_t result_reg,
                                  int32_t dividend, int32_t divisor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  constexpr int32_t kMinInt = std::numeric_limits<int32_t>::min();
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero();
    return false;
  }
  if (UNLIKELY(dividend == kMinInt && divisor == -1)) {
    shadow_frame.SetVReg(result_reg, 0);
  } else {
    shadow_frame.SetVReg(result_reg, dividend % divisor);
  }
  return true;
}

// Handles div-long and div-long-2addr instructions.
// Returns true on success, otherwise throws a java.lang.ArithmeticException and return false.
static inline bool DoLongDivide(ShadowFrame& shadow_frame,
                                size_t result_reg,
                                int64_t dividend,
                                int64_t divisor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const int64_t kMinLong = std::numeric_limits<int64_t>::min();
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero();
    return false;
  }
  if (UNLIKELY(dividend == kMinLong && divisor == -1)) {
    shadow_frame.SetVRegLong(result_reg, kMinLong);
  } else {
    shadow_frame.SetVRegLong(result_reg, dividend / divisor);
  }
  return true;
}

// Handles rem-long and rem-long-2addr instructions.
// Returns true on success, otherwise throws a java.lang.ArithmeticException and return false.
static inline bool DoLongRemainder(ShadowFrame& shadow_frame,
                                   size_t result_reg,
                                   int64_t dividend,
                                   int64_t divisor)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const int64_t kMinLong = std::numeric_limits<int64_t>::min();
  if (UNLIKELY(divisor == 0)) {
    ThrowArithmeticExceptionDivideByZero();
    return false;
  }
  if (UNLIKELY(dividend == kMinLong && divisor == -1)) {
    shadow_frame.SetVRegLong(result_reg, 0);
  } else {
    shadow_frame.SetVRegLong(result_reg, dividend % divisor);
  }
  return true;
}

// Handles filled-new-array and filled-new-array-range instructions.
// Returns true on success, otherwise throws an exception and returns false.
template <bool is_range, bool do_access_check, bool transaction_active>
bool DoFilledNewArray(const Instruction* inst, const ShadowFrame& shadow_frame,
                      Thread* self, JValue* result);

// Handles packed-switch instruction.
// Returns the branch offset to the next instruction to execute.
static inline int32_t DoPackedSwitch(const Instruction* inst, const ShadowFrame& shadow_frame,
                                     uint16_t inst_data)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(inst->Opcode() == Instruction::PACKED_SWITCH);
  const uint16_t* switch_data = reinterpret_cast<const uint16_t*>(inst) + inst->VRegB_31t();
  int32_t test_val = shadow_frame.GetVReg(inst->VRegA_31t(inst_data));
  DCHECK_EQ(switch_data[0], static_cast<uint16_t>(Instruction::kPackedSwitchSignature));
  uint16_t size = switch_data[1];
  if (size == 0) {
    // Empty packed switch, move forward by 3 (size of PACKED_SWITCH).
    return 3;
  }
  const int32_t* keys = reinterpret_cast<const int32_t*>(&switch_data[2]);
  DCHECK_ALIGNED(keys, 4);
  int32_t first_key = keys[0];
  const int32_t* targets = reinterpret_cast<const int32_t*>(&switch_data[4]);
  DCHECK_ALIGNED(targets, 4);
  int32_t index = test_val - first_key;
  if (index >= 0 && index < size) {
    return targets[index];
  } else {
    // No corresponding value: move forward by 3 (size of PACKED_SWITCH).
    return 3;
  }
}

// Handles sparse-switch instruction.
// Returns the branch offset to the next instruction to execute.
static inline int32_t DoSparseSwitch(const Instruction* inst, const ShadowFrame& shadow_frame,
                                     uint16_t inst_data)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  DCHECK(inst->Opcode() == Instruction::SPARSE_SWITCH);
  const uint16_t* switch_data = reinterpret_cast<const uint16_t*>(inst) + inst->VRegB_31t();
  int32_t test_val = shadow_frame.GetVReg(inst->VRegA_31t(inst_data));
  DCHECK_EQ(switch_data[0], static_cast<uint16_t>(Instruction::kSparseSwitchSignature));
  uint16_t size = switch_data[1];
  // Return length of SPARSE_SWITCH if size is 0.
  if (size == 0) {
    return 3;
  }
  const int32_t* keys = reinterpret_cast<const int32_t*>(&switch_data[2]);
  DCHECK_ALIGNED(keys, 4);
  const int32_t* entries = keys + size;
  DCHECK_ALIGNED(entries, 4);
  int lo = 0;
  int hi = size - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    int32_t foundVal = keys[mid];
    if (test_val < foundVal) {
      hi = mid - 1;
    } else if (test_val > foundVal) {
      lo = mid + 1;
    } else {
      return entries[mid];
    }
  }
  // No corresponding value: move forward by 3 (size of SPARSE_SWITCH).
  return 3;
}

// We execute any instrumentation events triggered by throwing and/or handing the pending exception
// and change the shadow_frames dex_pc to the appropriate exception handler if the current method
// has one. If the exception has been handled and the shadow_frame is now pointing to a catch clause
// we return true. If the current method is unable to handle the exception we return false.
// This function accepts a null Instrumentation* as a way to cause instrumentation events not to be
// reported.
// TODO We might wish to reconsider how we cause some events to be ignored.
bool MoveToExceptionHandler(Thread* self,
                            ShadowFrame& shadow_frame,
                            const instrumentation::Instrumentation* instrumentation)
    REQUIRES_SHARED(Locks::mutator_lock_);

NO_RETURN void UnexpectedOpcode(const Instruction* inst, const ShadowFrame& shadow_frame)
  __attribute__((cold))
  REQUIRES_SHARED(Locks::mutator_lock_);

// Set true if you want TraceExecution invocation before each bytecode execution.
constexpr bool kTraceExecutionEnabled = false;

static inline void TraceExecution(const ShadowFrame& shadow_frame, const Instruction* inst,
                                  const uint32_t dex_pc)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (kTraceExecutionEnabled) {
#define TRACE_LOG std::cerr
    std::ostringstream oss;
    oss << shadow_frame.GetMethod()->PrettyMethod()
        << android::base::StringPrintf("\n0x%x: ", dex_pc)
        << inst->DumpString(shadow_frame.GetMethod()->GetDexFile()) << "\n";
    for (uint32_t i = 0; i < shadow_frame.NumberOfVRegs(); ++i) {
      uint32_t raw_value = shadow_frame.GetVReg(i);
      ObjPtr<mirror::Object> ref_value = shadow_frame.GetVRegReference(i);
      oss << android::base::StringPrintf(" vreg%u=0x%08X", i, raw_value);
      if (ref_value != nullptr) {
        if (ref_value->GetClass()->IsStringClass() &&
            !ref_value->AsString()->IsValueNull()) {
          oss << "/java.lang.String \"" << ref_value->AsString()->ToModifiedUtf8() << "\"";
        } else {
          oss << "/" << ref_value->PrettyTypeOf();
        }
      }
    }
    TRACE_LOG << oss.str() << "\n";
#undef TRACE_LOG
  }
}

static inline bool IsBackwardBranch(int32_t branch_offset) {
  return branch_offset <= 0;
}

// The arg_offset is the offset to the first input register in the frame.
void ArtInterpreterToCompiledCodeBridge(Thread* self,
                                        ArtMethod* caller,
                                        ShadowFrame* shadow_frame,
                                        uint16_t arg_offset,
                                        JValue* result);

static inline bool IsStringInit(const DexFile* dex_file, uint32_t method_idx)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  const DexFile::MethodId& method_id = dex_file->GetMethodId(method_idx);
  const char* class_name = dex_file->StringByTypeIdx(method_id.class_idx_);
  const char* method_name = dex_file->GetMethodName(method_id);
  // Instead of calling ResolveMethod() which has suspend point and can trigger
  // GC, look up the method symbolically.
  // Compare method's class name and method name against string init.
  // It's ok since it's not allowed to create your own java/lang/String.
  // TODO: verify that assumption.
  if ((strcmp(class_name, "Ljava/lang/String;") == 0) &&
      (strcmp(method_name, "<init>") == 0)) {
    return true;
  }
  return false;
}

static inline bool IsStringInit(const Instruction* instr, ArtMethod* caller)
    REQUIRES_SHARED(Locks::mutator_lock_) {
  if (instr->Opcode() == Instruction::INVOKE_DIRECT ||
      instr->Opcode() == Instruction::INVOKE_DIRECT_RANGE) {
    uint16_t callee_method_idx = (instr->Opcode() == Instruction::INVOKE_DIRECT_RANGE) ?
        instr->VRegB_3rc() : instr->VRegB_35c();
    return IsStringInit(caller->GetDexFile(), callee_method_idx);
  }
  return false;
}

// Set string value created from StringFactory.newStringFromXXX() into all aliases of
// StringFactory.newEmptyString().
void SetStringInitValueToAllAliases(ShadowFrame* shadow_frame,
                                    uint16_t this_obj_vreg,
                                    JValue result);

}  // namespace interpreter
}  // namespace art

#endif  // ART_RUNTIME_INTERPRETER_INTERPRETER_COMMON_H_
