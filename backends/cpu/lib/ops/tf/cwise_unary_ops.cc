/*
 * Copyright 2020 The TensorFlow Runtime Authors
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

//===- cwise_unary_ops.cc - -------------------------------------*- C++ -*-===//
//
// Column wise unary Tensorflow operations.
//
//===----------------------------------------------------------------------===//

#ifndef TFRT_BACKENDS_CPU_OPS_TF_CWISE_UNARY_OPS_H_
#define TFRT_BACKENDS_CPU_OPS_TF_CWISE_UNARY_OPS_H_

#include "cwise_unary_ops.h"

#include "../../kernels/cwise_unary_kernels.h"
#include "tfrt/common/compat/eigen/eigen_dtype.h"
#include "tfrt/common/ops/tf/metadata_functions.h"
#include "tfrt/core_runtime/op_attrs.h"
#include "tfrt/core_runtime/op_utils.h"
#include "tfrt/cpu/core_runtime/cpu_op_registry.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/chain.h"
#include "tfrt/host_context/kernel_utils.h"
#include "tfrt/support/forward_decls.h"
#include "tfrt/tensor/dense_host_tensor.h"
#include "tfrt/tensor/dense_host_tensor_view.h"
#include "tfrt/tensor/tensor_serialize_utils.h"

namespace tfrt {
namespace {

template <typename UnaryFunctor>
static AsyncValueRef<DenseHostTensor> TfUnaryOp(
    const DenseHostTensor& input, const TensorMetadata& output_md,
    const ExecutionContext& exec_ctx) {
  HostContext* host = exec_ctx.host();

  auto dest = DenseHostTensor::CreateUninitialized(output_md, host);
  if (!dest) {
    return EmitErrorAsync(exec_ctx, "out of memory allocating result");
  }

  AsyncValueRef<Chain> chain;
  switch (input.dtype().kind()) {
    default:
      chain = EmitErrorAsync(exec_ctx, "unsupported dtype");
      break;
#define DTYPE_FLOAT(ENUM)                                                    \
  case DType::ENUM: {                                                        \
    using F = typename UnaryFunctor::template Functor<                       \
        EigenTypeForDTypeKind<DType::ENUM>>;                                 \
    chain = ::tfrt::cpu::UnaryKernel<F>(input, dest.getPointer(), exec_ctx); \
  } break;
#include "tfrt/dtype/dtype.def"  // NOLINT
  }

  return ForwardValue(dest.getValue(), std::move(chain), host);
}

template <typename Functor>
void RegisterTfUnaryOp(CpuOpRegistry* op_registry, string_view op_name) {
  op_registry->AddOp(op_name, TFRT_CPU_OP(TfUnaryOp<Functor>),
                     CpuOpFlags::NoSideEffects);
}

}  // namespace

void RegisterTfUnaryCpuOps(CpuOpRegistry* op_registry) {
  RegisterTfUnaryOp<cpu::functor::Log>(op_registry, "tf.Log");
  RegisterTfUnaryOp<cpu::functor::Log1p>(op_registry, "tf.Log1p");
}

}  // namespace tfrt

#endif  // TFRT_BACKENDS_CPU_OPS_TF_CWISE_UNARY_OPS_H_
