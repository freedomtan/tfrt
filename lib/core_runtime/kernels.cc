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

//===- core_runtime/kernels.cc --------------------------------------------===//
//
// This library contains kernels that allows the bef_executor to drive the core
// runtime.
//
//===----------------------------------------------------------------------===//

#include "tfrt/core_runtime/kernels.h"

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/SmallString.h"
#include "tfrt/core_runtime/core_runtime.h"
#include "tfrt/core_runtime/execute_op_impl.h"
#include "tfrt/core_runtime/op_attrs.h"
#include "tfrt/core_runtime/op_handler.h"
#include "tfrt/core_runtime/tensor_handle.h"
#include "tfrt/host_context/async_value.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/host_context/chain.h"
#include "tfrt/host_context/function.h"
#include "tfrt/host_context/kernel_utils.h"
#include "tfrt/support/error_util.h"
#include "tfrt/support/ref_count.h"
#include "tfrt/tensor/dense_host_tensor.h"
#include "tfrt/tensor/dense_host_tensor_view.h"
#include "tfrt/tensor/host_tensor.h"
#include "tfrt/tensor/string_host_tensor.h"
#include "tfrt/tensor/tensor_serialize_utils.h"

namespace tfrt {

// Convert a HostTensor (or subclass) into a TensorHandle for use by
// Core Runtime.
static void HTToTensorHandle(Argument<HostTensor> arg, Argument<Chain> in_chain,
                             Result<TensorHandle> tensorhandle_output) {
  // Since we know the Tensor is present, we can access its metadata.
  // TODO(b/158775215): Replace the placeholder device with the device from
  // HostTensor.
  tensorhandle_output.Emplace(RCReference<Device>(), arg->metadata(),
                              arg.ValueRef());
}

static void TensorHandleToHT(Argument<TensorHandle> arg,
                             Result<HostTensor> ht_output) {
  ht_output.Set(FormRef(arg->GetAsyncTensor()));
}

// Get TensorShape of a TensorHandle for use by Core Runtime.
static void TensorHandleToShape(Argument<TensorHandle> arg,
                                Result<TensorShape> tensorshape_result,
                                const ExecutionContext &exec_ctx) {
  if (arg->IsMetadataAvailable()) {
    auto shape = arg->GetAvailableMetadata().shape;
    tensorshape_result.Emplace(std::move(shape));
    return;
  }
  // The metadata is not available yet.
  const AsyncValueRef<TensorMetadata> &metadata = arg->GetAsyncMetadata();

  auto value = tensorshape_result.AllocateIndirect();
  metadata.AndThen([value_ref = std::move(value),
                    metadata_ref = metadata.CopyRef(),
                    host = exec_ctx.host()]() mutable {
    if (metadata_ref.IsError()) {
      value_ref->ForwardTo(metadata_ref.ReleaseRCRef());
      return;
    }
    auto shape = metadata_ref.get().shape;
    value_ref->ForwardTo(
        host->MakeAvailableAsyncValueRef<TensorShape>(std::move(shape)));
  });
}

// Convert a HostTensor (or subclass) into a TensorHandle for use by
// Core Runtime.
static Chain PrintTensorHandle(Argument<TensorHandle> arg) {
  llvm::SmallString<256> message;
  llvm::raw_svector_ostream(message) << arg.get() << "\n";
  printf("%s", message.c_str());
  fflush(stdout);
  return Chain();
}

static void CreateOpAttrs(Result<OpAttrs> result) { result.Emplace(); }

static Chain OpAttrsSetBool(Argument<OpAttrs> attrs, StringAttribute key,
                            Attribute<int8_t> value) {
  attrs->Set(key, static_cast<bool>(*value));
  return Chain();
}

template <typename T>
static Chain OpAttrsSet(Argument<OpAttrs> attrs, StringAttribute key,
                        Attribute<T> value) {
  attrs->Set(key, *value);
  return Chain();
}

static Chain OpAttrsSetDType(Argument<OpAttrs> attrs, StringAttribute key,
                             Attribute<BEFDataType> value) {
  attrs->Set(key, GetOpAttrTypeFromBEFDataType(*value));
  return Chain();
}

static Chain OpAttrsSetDense(Argument<OpAttrs> attrs, StringAttribute key,
                             DenseAttr value) {  // NOLINT
  attrs->Set(key, value);
  return Chain();
}

static Chain OpAttrsSetAggregate(Argument<OpAttrs> attrs, StringAttribute key,
                                 AggregateAttr value) {  // NOLINT
  attrs->Set(key, value);
  return Chain();
}
static Chain OpAttrsSetShape(Argument<OpAttrs> attrs, StringAttribute key,
                             ShapeAttr value) {  // NOLINT
  attrs->Set(key, value);
  return Chain();
}

template <typename T>
static Chain OpAttrsSetArray(Argument<OpAttrs> attrs, StringAttribute key,
                             ArrayAttribute<T> value) {
  attrs->SetArray(key, value.data());
  return Chain();
}

static Chain OpAttrsSetString(Argument<OpAttrs> attrs, StringAttribute key,
                              StringAttribute value) {
  attrs->SetString(key, value.get());
  return Chain();
}

static llvm::Expected<TensorHandle> ConstStringTensor(
    ArrayAttr shape, AggregateAttr value, const ExecutionContext &exec_ctx) {
  TensorMetadata metadata(DType(DType::String), shape.GetValue<ssize_t>());

  auto tensor_ref =
      StringHostTensor::MakeConstructedAsyncValueRef(metadata, exec_ctx.host());
  if (!tensor_ref)
    return MakeStringError("failed to allocate string host tensor");

  auto strings = tensor_ref.get().strings();
  assert(strings.size() == value.GetNumElements());

  for (int i = 0, e = strings.size(); i != e; ++i) {
    strings[i] = value.GetAttributeOfType<StringAttr>(i).GetValue().str();
  }

  tensor_ref.SetStateConcrete();

  // TODO(b/158775215): Replace the placeholder device with the device from
  // HostContext.
  return TensorHandle(/*device=*/{}, metadata, std::move(tensor_ref));
}

static llvm::Expected<TensorHandle> ConstDenseTensor(
    DenseAttr value, const ExecutionContext &context) {
  auto *host = context.host();
  auto dht = DeserializeDenseHostTensorFromDenseAttr(value, host);
  if (!dht) return dht.takeError();

  auto metadata = dht->metadata();
  auto tensor_ref =
      host->MakeAvailableAsyncValueRef<DenseHostTensor>(std::move(*dht));
  if (!tensor_ref)
    return MakeStringError("failed to allocate dense host tensor");

  // TODO(b/158775215): Replace the placeholder device with the device from
  // HostContext.
  return TensorHandle(/*device=*/{}, metadata, std::move(tensor_ref));
}

template <typename DType>
static llvm::Expected<TensorHandle> CreateDenseTensor(
    ArrayAttribute<ssize_t> shape, ArrayAttribute<DType> value,
    const ExecutionContext &context) {
  auto *host = context.host();

  TensorMetadata metadata(GetDType<DType>(), shape.data());
  auto dht = DenseHostTensor::MakeConstructedAsyncValueRef(metadata, host);
  if (!dht) return MakeStringError("failed to allocate dense host tensor");

  std::memcpy(dht->data(), value.data().data(), dht->DataSizeInBytes());

  dht.SetStateConcrete();

  return TensorHandle(metadata, std::move(dht));
}

// ExecuteOp executes the `op_name` operation on the `op_handler`.
static void ExecuteOp(Argument<OpHandler *> op_handler, RemainingArguments args,
                      RemainingResults results, AggregateAttr op_attr_array,
                      StringAttr op_name, KernelErrorHandler handler,
                      const ExecutionContext &exec_ctx) {
  auto *host = exec_ctx.host();
  auto *core_rt = CoreRuntime::GetFromHostContext(host);
  if (!core_rt) return handler.ReportError("no CoreRuntime available");

  auto expected_op = core_rt->MakeOp(op_name.GetValue(), op_handler.get());
  if (!expected_op) return handler.ReportError(StrCat(expected_op.takeError()));

  for (int b = 0, e = results.size(); b < e; ++b)
    results.AllocateAt<TensorHandle>(b);

  ExecuteOpImpl(std::move(expected_op.get()), args.values(),
                /*op_chain =*/nullptr, results.values(), op_attr_array,
                exec_ctx);
}

// ExecuteOpSeq executes the `op_name` operation on the `op_handler`. It takes
// an `in_op_chain` and produces an `out_op_chain` for sequencing op execution.
// The execution is only started when `in_op_chain` is ready, and the
// `out_op_chain` is ready only after the execution is finished.
static void ExecuteOpSeq(Argument<OpHandler *> op_handler,
                         Argument<Chain> in_op_chain, RemainingArguments args,
                         Result<Chain> out_op_chain, RemainingResults results,
                         AggregateAttr op_attr_array, StringAttr op_name,
                         KernelErrorHandler handler,
                         const ExecutionContext &exec_ctx) {
  auto *host = exec_ctx.host();
  auto *core_rt = CoreRuntime::GetFromHostContext(host);
  if (!core_rt) return handler.ReportError("no CoreRuntime available");

  for (int b = 0, e = results.size(); b < e; ++b)
    results.AllocateAt<TensorHandle>(b);

  SmallVector<AsyncValue *, 4> async_args;
  if (!op_handler.value()->IsConcrete())
    async_args.push_back(op_handler.value());
  for (auto *arg_av : args.values())
    if (!arg_av->IsConcrete()) async_args.push_back(arg_av);

  // If all arguments except in_op_chain are ready, we can just execute the op.
  if (async_args.empty()) {
    auto expected_op = core_rt->MakeOp(op_name.GetValue(), op_handler.get());
    if (!expected_op)
      return handler.ReportError(StrCat(expected_op.takeError()));

    auto op_chain = in_op_chain.ValueRef();
    ExecuteOpImpl(std::move(expected_op.get()), args.values(), &op_chain,
                  results.values(), op_attr_array, exec_ctx);
    out_op_chain.Set(std::move(op_chain));
    return;
  }

  // Otherwise, we need to create references to all arguments and asynchronouly
  // execute the op when they are ready.

  SmallVector<AsyncValueRef<TensorHandle>, 4> arg_refs;
  for (auto *av : args.values()) {
    arg_refs.push_back(AsyncValueRef<TensorHandle>(FormRef(av)));
  }

  SmallVector<RCReference<AsyncValue>, 4> result_refs;
  for (auto &av : results.values()) {
    result_refs.push_back(av.CopyRef());
  }

  host->RunWhenReady(
      async_args,
      [core_rt, op_handler = op_handler.ValueRef(),
       op_chain = in_op_chain.ValueRef(), arg_refs = std::move(arg_refs),
       result_refs = std::move(result_refs),
       out_op_chain = out_op_chain.Allocate(), op_name = op_name.GetValue(),
       op_attr_array, exec_ctx]() mutable {
        auto propgate_error = [&](const DecodedDiagnostic &diag) {
          out_op_chain.SetError(diag);
          for (auto &result_ref : result_refs) result_ref->SetError(diag);
        };

        if (op_handler.IsError()) return propgate_error(op_handler.GetError());
        if (op_chain.IsError()) return propgate_error(op_chain.GetError());

        auto expected_op = core_rt->MakeOp(op_name, op_handler.get());
        if (!expected_op)
          return propgate_error(
              EmitError(exec_ctx, StrCat(expected_op.takeError())));

        SmallVector<AsyncValue *, 4> arg_avs;
        for (const auto &arg_ref : arg_refs) {
          if (arg_ref.IsError()) return propgate_error(arg_ref.GetError());
          arg_avs.push_back(arg_ref.GetAsyncValue());
        }

        ExecuteOpImpl(std::move(expected_op.get()), arg_avs, &op_chain,
                      result_refs, op_attr_array, exec_ctx);

        auto *op_chain_av = op_chain.GetAsyncValue();
        op_chain_av->AndThen([op_chain = std::move(op_chain),
                              out_op_chain = out_op_chain.CopyRef()]() {
          // TODO(chky): we should have a version of AndThen that passes the
          // resolved state into the waiter.
          if (op_chain.IsError()) {
            out_op_chain.SetError(op_chain.GetError());
          } else {
            out_op_chain.emplace();
          }
        });
      });
}

// ExecuteOp executes the `op_name` operation on the `op_handler`.
static void ExecuteCoreRuntimeOp(Argument<CoreRuntimeOp> op,
                                 RemainingArguments args,
                                 RemainingResults results,
                                 AggregateAttr op_attrs,
                                 KernelErrorHandler handler,
                                 const ExecutionContext &exec_ctx) {
  auto *host = exec_ctx.host();
  auto *core_rt = CoreRuntime::GetFromHostContext(host);
  if (!core_rt) return handler.ReportError("no CoreRuntime available");

  for (int b = 0, e = results.size(); b < e; ++b)
    results.AllocateAt<TensorHandle>(b);

  ExecuteOpImpl(std::move(op.get()), args.values(),
                /*op_chain =*/nullptr, results.values(), op_attrs, exec_ctx);
}

static tfrt::Expected<CoreRuntimeOp> MakeCompositeOp(
    Attribute<Function> fn_const, const ExecutionContext &exec_ctx) {
  auto *host = exec_ctx.host();
  auto *core_rt = CoreRuntime::GetFromHostContext(host);
  if (!core_rt) return MakeStringError("no CoreRuntime available");

  Function *fn = const_cast<Function *>(&(*fn_const));
  return core_rt->MakeCompositeOp(fn);
}

// GetOpHandler accepts chains because the op_handlers now can be registered
// dynamically as well.
static tfrt::Expected<OpHandler *> GetOpHandler(
    Argument<Chain> in_op_chain, StringAttribute op_handler_name,
    const ExecutionContext &exec_ctx) {
  auto *runtime = CoreRuntime::GetFromHostContext(exec_ctx.host());
  assert(runtime);

  if (auto *op_handler = runtime->GetOpHandler(op_handler_name.get())) {
    return op_handler;
  }
  return tfrt::MakeStringError("op_handler not found.");
}

static Chain RegisterOpHandlerChain(Argument<OpHandler *> root,
                                    StringAttribute chain_name,
                                    const ExecutionContext &exec_ctx) {
  assert(root.get());
  auto *runtime = CoreRuntime::GetFromHostContext(exec_ctx.host());
  assert(runtime);

  runtime->RegisterOpHandlerChain(chain_name, root.get());
  return Chain();
}

static bool GetDHTPredicateValue(const DenseHostTensor &dht) {
  switch (dht.dtype().kind()) {
    default:
      llvm_unreachable("dtype not supported");
      break;
    case DType::BOOL: {
      auto dht_view = DHTArrayView<bool>(&dht);
      assert(dht_view.NumElements() == 1);
      return dht_view[0];
    }
#define DTYPE_INT(ENUM)                                                      \
  case DType::ENUM: {                                                        \
    auto dht_view = DHTArrayView<tfrt::TypeForDTypeKind<DType::ENUM>>(&dht); \
    assert(dht_view.NumElements() == 1);                                     \
    return dht_view[0] != 0;                                                 \
  }
#include "tfrt/dtype/dtype.def"  // NOLINT
  }
}

// corert.cond dispatches to a 'true' or 'false' function based on a condition.
//
// Arguments: The first argument is the condition, with type TensorHandle, and
// any additional arguments are passed to the selected function.
//
// Attributes: The first attribute is the true_fn, and the second attribute is
// the false_fn. The functions must have matching signatures, and their
// signatures must match corert.cond's signature.
//
// corert.cond supports "non-strict" invocation: it is safe to invoke before all
// its arguments are ready. The caller must set the bef.nonstrict attribute on
// hex.if to make an invocation non-strict.
static void CoreRtConditional(RemainingArguments args, RemainingResults results,
                              Attribute<Function> true_fn_const,
                              Attribute<Function> false_fn_const,
                              const ExecutionContext &exec_ctx) {
  assert(args.size() > 0);

  const Function *true_fn = &(*true_fn_const);
  const Function *false_fn = &(*false_fn_const);

  assert(true_fn->argument_types().size() == args.size() - 1 &&
         "argument count mismatch");
  assert(true_fn->result_types().size() == results.size() &&
         "result count mismatch");
  assert(true_fn->argument_types() == false_fn->argument_types() &&
         true_fn->result_types() == false_fn->result_types() &&
         "true and false function types need to line up");

  // Note: At this point, the condition's availability is unknown. It may become
  // available at any time.

  // Copy `args` and add a ref to each arg. These refs will be dropped when the
  // RCArray is destroyed. arg_refs is captured by the lambda so the kernel's
  // arguments will be available when the closure runs.
  RCArray<AsyncValue> arg_refs(args.values());

  // We need to create all the result values eagerly so we can return them
  // from the HexIf function, even though we don't know their types.  Use
  // an IndirectAsyncValue for this, because it can lazily get resolved.
  SmallVector<RCReference<IndirectAsyncValue>, 4> result_refs;
  result_refs.reserve(results.size());
  for (int i = 0, e = results.size(); i != e; ++i) {
    auto result = results.AllocateIndirectResultAt(i);
    // To ensure the results live long enough to be filled in by our deferred
    // evaluation, we keep the RCReferences holding the results.
    result_refs.push_back(std::move(result));
  }

  auto handle_error_and_return =
      [](AsyncValue *condition,
         MutableArrayRef<RCReference<IndirectAsyncValue>> results) {
        // If we have an error, then we can force propagate errors to all the
        // results.
        if (condition->IsError()) {
          for (auto &result : results) result->ForwardTo(FormRef(condition));
          return;
        }
      };

  auto if_impl =
      [](const HostTensor &ht, const Function *true_fn,
         const Function *false_fn, ArrayRef<AsyncValue *> arg_refs,
         MutableArrayRef<RCReference<IndirectAsyncValue>> result_refs,
         const ExecutionContext &exec_ctx) {
        bool predicate = false;
        // TODO(zhangqiaorjc): Handle other tensor types and other
        // dtypes.
        if (const DenseHostTensor *dht = llvm::dyn_cast<DenseHostTensor>(&ht)) {
          predicate = GetDHTPredicateValue(*dht);
        } else if (const StringHostTensor *sht =
                       llvm::dyn_cast<StringHostTensor>(&ht)) {
          auto strings = sht->strings();
          // Only empty string is false.
          if (strings.empty() || strings[0].empty()) {
            predicate = false;
          } else {
            predicate = true;
          }
        } else {
          assert(false && "tensor type not yet supported");
        }

        const Function *fn = predicate ? true_fn : false_fn;
        SmallVector<RCReference<AsyncValue>, 8> results;
        results.resize(result_refs.size());
        fn->Execute(exec_ctx, arg_refs.drop_front(), results);

        // Forward result_refs to results. This transfers the +1
        // results returned by Execute to the ForwardTo call.
        for (int i = 0, e = result_refs.size(); i != e; ++i) {
          result_refs[i]->ForwardTo(std::move(results[i]));
        }
      };

  // Arg[0] is a TensorHandle async value condition predicate.
  AsyncValue *condition_tensorhandle = args[0];
  // Dispatch when the condition becomes available.
  condition_tensorhandle->AndThen(
      [condition_tensorhandle, handle_error_and_return, exec_ctx, if_impl,
       true_fn_ref = FormRef(true_fn), false_fn_ref = FormRef(false_fn),
       arg_refs = std::move(arg_refs),
       result_refs = std::move(result_refs)]() mutable {
        handle_error_and_return(condition_tensorhandle, result_refs);
        AsyncValue *condition_async_tensor =
            condition_tensorhandle->get<TensorHandle>().GetAsyncTensor();

        condition_async_tensor->AndThen(
            [condition_async_tensor, handle_error_and_return, exec_ctx, if_impl,
             true_fn_ref = std::move(true_fn_ref),
             false_fn_ref = std::move(false_fn_ref),
             arg_refs = std::move(arg_refs),
             result_refs = std::move(result_refs)]() mutable {
              handle_error_and_return(condition_async_tensor, result_refs);

              auto &tensor = condition_async_tensor->get<Tensor>();
              uint32_t allowed_formats =
                  1 << static_cast<uint32_t>(Tensor::Subclass::DenseHost);
              AsyncValueRef<HostTensor> condition_host_tensor =
                  tensor.ConvertToHostTensor(exec_ctx.host(), allowed_formats);

              condition_host_tensor.AndThen(
                  [condition_host_tensor = condition_host_tensor.CopyRef(),
                   handle_error_and_return, exec_ctx, if_impl,
                   true_fn_ref = std::move(true_fn_ref),
                   false_fn_ref = std::move(false_fn_ref),
                   arg_refs = std::move(arg_refs),
                   result_refs = std::move(result_refs)]() mutable {
                    handle_error_and_return(
                        condition_host_tensor.GetAsyncValue(), result_refs);

                    if_impl(*condition_host_tensor, true_fn_ref.get(),
                            false_fn_ref.get(), arg_refs.values(), result_refs,
                            exec_ctx);
                  });
            });
      });
}

//===----------------------------------------------------------------------===//
// Registration
//===----------------------------------------------------------------------===//

void RegisterCreateDenseTensor(KernelRegistry *registry) {
#define REGISTER_CREATE_DENSE_TENSOR(CPP_TYPE, TYPE_NAME)       \
  registry->AddKernel("corert.create_dense_tensor." #TYPE_NAME, \
                      TFRT_KERNEL(CreateDenseTensor<CPP_TYPE>))
  REGISTER_CREATE_DENSE_TENSOR(uint8_t, ui8);
  REGISTER_CREATE_DENSE_TENSOR(uint16_t, ui16);
  REGISTER_CREATE_DENSE_TENSOR(uint32_t, ui32);
  REGISTER_CREATE_DENSE_TENSOR(uint64_t, ui64);
  REGISTER_CREATE_DENSE_TENSOR(int8_t, i1);
  REGISTER_CREATE_DENSE_TENSOR(int8_t, i8);
  REGISTER_CREATE_DENSE_TENSOR(int16_t, i16);
  REGISTER_CREATE_DENSE_TENSOR(int32_t, i32);
  REGISTER_CREATE_DENSE_TENSOR(int64_t, i64);
  REGISTER_CREATE_DENSE_TENSOR(float, f32);
  REGISTER_CREATE_DENSE_TENSOR(double, f64);
#undef REGISTER_CREATE_DENSE_TENSOR
}

void RegisterCoreRuntimeKernels(KernelRegistry *registry) {
  registry->AddKernel("corert.tensorhandle_to_shape",
                      TFRT_KERNEL(TensorHandleToShape));
  registry->AddKernel("corert.ht_to_tensorhandle",
                      TFRT_KERNEL(HTToTensorHandle));
  registry->AddKernel("corert.tensorhandle_to_ht",
                      TFRT_KERNEL(TensorHandleToHT));
  registry->AddKernel("corert.print_tensorhandle",
                      TFRT_KERNEL(PrintTensorHandle));
  registry->AddKernel("corert.create_op_attrs", TFRT_KERNEL(CreateOpAttrs));
  registry->AddKernel("corert.op_attrs_set.bool", TFRT_KERNEL(OpAttrsSetBool));
  registry->AddKernel("corert.op_attrs_set.i32",
                      TFRT_KERNEL(OpAttrsSet<int32_t>));
  registry->AddKernel("corert.op_attrs_set_array.i32",
                      TFRT_KERNEL(OpAttrsSetArray<int32_t>));
  registry->AddKernel("corert.op_attrs_set_array.i64",
                      TFRT_KERNEL(OpAttrsSetArray<int64_t>));
  registry->AddKernel("corert.op_attrs_set.f32",
                      TFRT_KERNEL(OpAttrsSet<float>));
  registry->AddKernel("corert.op_attrs_set_array.f32",
                      TFRT_KERNEL(OpAttrsSetArray<float>));
  registry->AddKernel("corert.op_attrs_set.dtype",
                      TFRT_KERNEL(OpAttrsSetDType));
  registry->AddKernel("corert.op_attrs_set.dense",
                      TFRT_KERNEL(OpAttrsSetDense));
  registry->AddKernel("corert.op_attrs_set.aggregate",
                      TFRT_KERNEL(OpAttrsSetAggregate));
  registry->AddKernel("corert.op_attrs_set.shape",
                      TFRT_KERNEL(OpAttrsSetShape));
  registry->AddKernel("corert.op_attrs_set.str", TFRT_KERNEL(OpAttrsSetString));
  registry->AddKernel("corert.executeop", TFRT_KERNEL(ExecuteOp));
  registry->AddKernel("corert.executeop.seq", TFRT_KERNEL(ExecuteOpSeq));
  registry->AddKernel("corert.execute_crt_op",
                      TFRT_KERNEL(ExecuteCoreRuntimeOp));
  registry->AddKernel("corert.make_composite_op", TFRT_KERNEL(MakeCompositeOp));
  registry->AddKernel("corert.get_op_handler", TFRT_KERNEL(GetOpHandler));
  registry->AddKernel("corert.register_op_handler_chain",
                      TFRT_KERNEL(RegisterOpHandlerChain));
  registry->AddKernel("corert.const_dense_tensor",
                      TFRT_KERNEL(ConstDenseTensor));
  registry->AddKernel("corert.const_string_tensor",
                      TFRT_KERNEL(ConstStringTensor));
  registry->AddKernel("corert.cond", TFRT_KERNEL(CoreRtConditional));

  RegisterCreateDenseTensor(registry);
}

}  // namespace tfrt
