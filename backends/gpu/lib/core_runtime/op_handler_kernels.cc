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

//===- op_handler_kernels.cc ----------------------------------------------===//
//
// This file contains TFRT kernels to register GPU OpHandler.
//
//===----------------------------------------------------------------------===//

#include "op_handler_kernels.h"

#include "tfrt/core_runtime/core_runtime.h"
#include "tfrt/gpu/core_runtime/gpu_op_handler.h"
#include "tfrt/host_context/kernel_registry.h"
#include "tfrt/host_context/kernel_utils.h"

namespace tfrt {

static void CreateGpuOpHandlerKernel(Argument<int> gpu_ordinal,
                                     Argument<OpHandler *> fallback,
                                     Result<OpHandler *> op_handler,
                                     const ExecutionContext &exec_ctx) {
  auto *runtime = CoreRuntime::GetFromHostContext(exec_ctx.host());
  assert(runtime);
  auto op_handler_ptr =
      CreateGpuOpHandler(runtime, gpu_ordinal.get(), fallback.get());
  assert(op_handler_ptr);
  op_handler.Emplace(op_handler_ptr.get());
}
//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterGpuOpHandlerKernels(KernelRegistry *registry) {
  registry->AddKernel("corert.create_gpu_op_handler",
                      TFRT_KERNEL(CreateGpuOpHandlerKernel));
}

}  // namespace tfrt
