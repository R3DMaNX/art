/*
 * Copyright (C) 2011 The Android Open Source Project
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

#ifndef ART_RUNTIME_MIRROR_CLASS_REFVISITOR_INL_H_
#define ART_RUNTIME_MIRROR_CLASS_REFVISITOR_INL_H_

#include "class-inl.h"

#include "art_field-inl.h"
#include "class_ext-inl.h"

namespace art {
namespace mirror {

template <bool kVisitNativeRoots,
          VerifyObjectFlags kVerifyFlags,
          ReadBarrierOption kReadBarrierOption,
          typename Visitor>
inline void Class::VisitReferences(ObjPtr<Class> klass, const Visitor& visitor) {
  VisitInstanceFieldsReferences<kVerifyFlags, kReadBarrierOption>(klass.Ptr(), visitor);
  // Right after a class is allocated, but not yet loaded
  // (ClassStatus::kNotReady, see ClassLinker::LoadClass()), GC may find it
  // and scan it. IsTemp() may call Class::GetAccessFlags() but may
  // fail in the DCHECK in Class::GetAccessFlags() because the class
  // status is ClassStatus::kNotReady. To avoid it, rely on IsResolved()
  // only. This is fine because a temp class never goes into the
  // ClassStatus::kResolved state.
  if (IsResolved<kVerifyFlags>()) {
    // Temp classes don't ever populate imt/vtable or static fields and they are not even
    // allocated with the right size for those. Also, unresolved classes don't have fields
    // linked yet.
    VisitStaticFieldsReferences<kVerifyFlags, kReadBarrierOption>(this, visitor);
  }
  if (kVisitNativeRoots) {
    // Since this class is reachable, we must also visit the associated roots when we scan it.
    VisitNativeRoots<kReadBarrierOption>(
        visitor, Runtime::Current()->GetClassLinker()->GetImagePointerSize());
  }
}

template<ReadBarrierOption kReadBarrierOption, class Visitor>
void Class::VisitNativeRoots(Visitor& visitor, PointerSize pointer_size) {
  for (ArtField& field : GetSFieldsUnchecked()) {
    // Visit roots first in case the declaring class gets moved.
    field.VisitRoots(visitor);
    if (kIsDebugBuild && IsResolved()) {
      CHECK_EQ(field.GetDeclaringClass<kReadBarrierOption>(), this) << GetStatus();
    }
  }
  for (ArtField& field : GetIFieldsUnchecked()) {
    // Visit roots first in case the declaring class gets moved.
    field.VisitRoots(visitor);
    if (kIsDebugBuild && IsResolved()) {
      CHECK_EQ(field.GetDeclaringClass<kReadBarrierOption>(), this) << GetStatus();
    }
  }

  bool need_check = true;
  do {
    ArraySlice<ArtMethod> methods = GetMethods(pointer_size);
    uint32_t num_method = NumMethods();
    if (num_method == 0) {
      need_check = false;
      break;
    }
    /*
     * 1. HeapTaskDeamon may wait on this class before enter for-loop.
     *    The waiting period may last serveral milliseconds.
     * 2. During this period, some other thread may modify this class, such as
     *    setting methods_ to 0 by SetMethodsPtrUnchecked(nullptr, 0, 0) when
     *    they calling ClassLinker::LinkClass if this is a temp class.
     * 3. When HeapTaskDeamon wakeup and enter for-loop, it will meet crash due
     *    to the invalid methods_:
     *    signal 11 (SIGSEGV), code 1 (SEGV_MAPERR), fault addr 0x8
     *    The fault addr is equal to PionterSize, see function OffsetOfElement.
     * 4. Add CHECK: if num of methods changed, then retry from the beginning.
     */
    for (ArtMethod& method : methods) {
      if (need_check) {
        if (num_method != NumMethods()) {
          break;
        }
        need_check = false;
      }
      method.VisitRoots<kReadBarrierOption>(visitor, pointer_size);
    }
  } while (need_check);

  ObjPtr<ClassExt> ext(GetExtData<kDefaultVerifyFlags, kReadBarrierOption>());
  if (!ext.IsNull()) {
    ext->VisitNativeRoots<kReadBarrierOption, Visitor>(visitor, pointer_size);
  }
}

}  // namespace mirror
}  // namespace art

#endif  // ART_RUNTIME_MIRROR_CLASS_REFVISITOR_INL_H_
