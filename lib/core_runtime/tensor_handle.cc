// Copyright 2020 The TensorFlow Runtime Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//===- tensor_handle.cc -----------------------------------------*- C++ -*-===//
//
// This file defines tensor handle.
//
//===----------------------------------------------------------------------===//

#include "tfrt/core_runtime/tensor_handle.h"

#include "llvm/Support/raw_ostream.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/tensor/tensor.h"

namespace tfrt {

TensorHandle::TensorHandle(RCReference<Device> device,
                           AsyncValueRef<TensorMetadata> async_metadata,
                           AsyncValueRef<Tensor> tensor)
    : device_(std::move(device)) {
  // TODO(b/158775215): Assert the device is valid. We cannot do it now because
  // there is still some callers in TF side that create TensorHandle with
  // absent device.
  assert(async_metadata.GetAsyncValue());
  assert(tensor.GetAsyncValue());
  tensor_and_is_metadata_inline_.setPointerAndInt(tensor.release(), false);
  new (&async_metadata_)
      AsyncValueRef<TensorMetadata>(std::move(async_metadata));
}

TensorHandle::TensorHandle(RCReference<Device> device,
                           const TensorMetadata& metadata,
                           AsyncValueRef<Tensor> tensor)
    : device_(std::move(device)) {
  // TODO(b/158775215): Assert the device is valid.
  assert(tensor.GetAsyncValue());
  tensor_and_is_metadata_inline_.setPointerAndInt(tensor.release(), true);
  new (&inlined_metadata_) TensorMetadata(metadata);
}

TensorHandle::TensorHandle(AsyncValueRef<TensorHandle> error)
    : TensorHandle({}, AsyncValueRef<TensorMetadata>(error.CopyRef()),
                   AsyncValueRef<Tensor>(error.CopyRef())) {
  assert(error.IsError());
}

TensorHandle::TensorHandle(AsyncValueRef<TensorMetadata> async_metadata,
                           AsyncValueRef<Tensor> tensor)
    : TensorHandle({}, std::move(async_metadata), std::move(tensor)) {}
TensorHandle::TensorHandle(const TensorMetadata& metadata,
                           AsyncValueRef<Tensor> tensor)
    : TensorHandle({}, metadata, std::move(tensor)) {}

TensorHandle TensorHandle::CreateError(RCReference<AsyncValue> error) {
  assert(error->IsError());
  auto tensor_md = AsyncValueRef<TensorMetadata>(error.CopyRef());
  auto tensor = AsyncValueRef<Tensor>(std::move(error));
  return TensorHandle({}, std::move(tensor_md), std::move(tensor));
}

raw_ostream& operator<<(raw_ostream& os, const TensorHandle& handle) {
  auto tensor = handle.GetAsyncTensor();
  // Check for invalid states.  Both null could happen when in a moved-from
  // state.
  if (!handle.IsMetadataInline() && !handle.async_metadata_.GetAsyncValue() &&
      !tensor)
    return os << "NULL TensorHandle!";

  // Handle truly invalid states gracefully.
  if (!handle.IsMetadataInline() && !handle.async_metadata_.GetAsyncValue())
    return os << "Invalid TensorHandle with null metadata!";
  if (!tensor) return os << "Invalid TensorHandle with null tensor!";

  // If the tensor is resolved, just print it.
  if (handle.GetAsyncTensor()->IsConcrete()) return os << tensor->get<Tensor>();

  // If the tensor resolved to an error, print that.
  if (auto* error = tensor->GetErrorIfPresent())
    return os << "Error TensorHandle: '" << error->message << "'";

  // Otherwise, if the shape is present, print just that.  Note that there could
  // be a race between the check above and this check; we're ok with that.
  if (handle.IsMetadataInline())
    return os << "future TensorHandle with metadata "
              << handle.inlined_metadata_;
  else if (handle.async_metadata_.IsConcrete())
    return os << "future TensorHandle with metadata "
              << handle.async_metadata_.get();
  else if (auto* error = handle.async_metadata_.GetErrorIfPresent())
    return os << "future TensorHandle with error metadata '" << error->message
              << "'";

  return os << "fully future TensorHandle with unresolved metadata";
}

}  // namespace tfrt
