/*
 * Copyright (c) 2017 ARM Limited.
 *
 * SPDX-License-Identifier: MIT
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "arm_compute/runtime/NEON/functions/NEGEMMLowp.h"

#include "arm_compute/core/Error.h"
#include "arm_compute/core/Helpers.h"
#include "arm_compute/core/ITensor.h"
#include "arm_compute/core/TensorInfo.h"
#include "arm_compute/core/Types.h"
#include "arm_compute/core/Validate.h"
#include "arm_compute/runtime/NEON/NEScheduler.h"
#include "arm_compute/runtime/TensorAllocator.h"

using namespace arm_compute;

NEGEMMLowp::NEGEMMLowp()
    : _interleave_kernel(), _transpose_kernel(), _mm_kernel(), _tmp_a(), _tmp_b()
{
}

void NEGEMMLowp::configure(const ITensor *a, const ITensor *b, ITensor *output, int32_t a_offset, int32_t b_offset, int32_t output_offset, int32_t output_mult_int, int32_t shift)
{
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(a, 1, DataType::U8);
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(b, 1, DataType::U8);
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(output, 1, DataType::U8);
    ARM_COMPUTE_ERROR_ON_MISMATCHING_DATA_TYPES(a, b, output);
    ARM_COMPUTE_ERROR_ON_MSG(a->info()->dimension(0) != b->info()->dimension(1), "The product AB is defined only if the number of columns in A is equal to the number of rows in B");
    ARM_COMPUTE_ERROR_ON_MSG(a->info()->dimension(1) != output->info()->dimension(1), "The C matrix must have the same number of rows as the matrix A");
    ARM_COMPUTE_ERROR_ON_MSG(b->info()->dimension(0) != output->info()->dimension(0), "The C matrix must have the same number of columns as the matrix C");

    /* The interleaved output matrix will have the following shape: [ a_height * 4, a_width / 4 ] */
    TensorShape shape_tmp_a = a->info()->tensor_shape();
    shape_tmp_a.set(0, a->info()->dimension(0) * 4);
    shape_tmp_a.set(1, std::ceil(a->info()->dimension(1) / 4.f));

    TensorShape shape_tmp_b = b->info()->tensor_shape();
    shape_tmp_b.set(0, b->info()->dimension(1) * 4);
    shape_tmp_b.set(1, std::ceil(b->info()->dimension(0) / 4.f));

    TensorInfo info_a(shape_tmp_a, 1, a->info()->data_type());
    TensorInfo info_b(shape_tmp_b, 1, b->info()->data_type());
    _tmp_a.allocator()->init(info_a);
    _tmp_b.allocator()->init(info_b);

    _interleave_kernel.configure(a, &_tmp_a);
    _transpose_kernel.configure(b, &_tmp_b);
    _mm_kernel.configure(&_tmp_a, &_tmp_b, output, a_offset, b_offset, output_offset, output_mult_int, shift);

    _tmp_a.allocator()->allocate();
    _tmp_b.allocator()->allocate();
}

void NEGEMMLowp::run()
{
    /* Run interleave kernel */
    NEScheduler::get().multithread(&_interleave_kernel);

    /* Run transpose kernel */
    NEScheduler::get().multithread(&_transpose_kernel);

    /* Run matrix multiply kernel */
    NEScheduler::get().multithread(&_mm_kernel);
}
