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

//===- test_kernels.h - CUDA testing kernels --------------------*- C++ -*-===//
//
// This file declares the C++ functions that implement CUDA test-only kernels.
//
//===----------------------------------------------------------------------===//
#ifndef TFRT_BACKENDS_GPU_LIB_KERNELS_CUDA_TEST_KERNELS_H_
#define TFRT_BACKENDS_GPU_LIB_KERNELS_CUDA_TEST_KERNELS_H_

namespace tfrt {

class KernelRegistry;

namespace cuda {

void RegisterTestCudaKernels(KernelRegistry* kernel_reg);

}  // namespace cuda
}  // namespace tfrt

#endif  // TFRT_BACKENDS_GPU_LIB_KERNELS_CUDA_TEST_KERNELS_H_
