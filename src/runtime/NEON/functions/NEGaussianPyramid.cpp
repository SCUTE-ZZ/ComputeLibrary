/*
 * Copyright (c) 2016, 2017 ARM Limited.
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
#include "arm_compute/runtime/NEON/functions/NEGaussianPyramid.h"

#include "arm_compute/core/Error.h"
#include "arm_compute/core/Helpers.h"
#include "arm_compute/core/ITensor.h"
#include "arm_compute/core/NEON/kernels/NEGaussianPyramidKernel.h"
#include "arm_compute/core/NEON/kernels/NEScaleKernel.h"
#include "arm_compute/core/PixelValue.h"
#include "arm_compute/core/TensorInfo.h"
#include "arm_compute/core/Validate.h"
#include "arm_compute/runtime/NEON/NEScheduler.h"
#include "arm_compute/runtime/NEON/functions/NEGaussian5x5.h"
#include "arm_compute/runtime/Pyramid.h"
#include "arm_compute/runtime/Tensor.h"
#include "arm_compute/runtime/TensorAllocator.h"

#include <cstddef>

using namespace arm_compute;

NEGaussianPyramid::NEGaussianPyramid()
    : _input(nullptr), _pyramid(nullptr), _tmp()
{
}

NEGaussianPyramidHalf::NEGaussianPyramidHalf()
    : _border_handler(), _horizontal_reduction(), _vertical_reduction()
{
}

void NEGaussianPyramidHalf::configure(const ITensor *input, IPyramid *pyramid, BorderMode border_mode, uint8_t constant_border_value)
{
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(input, 1, DataType::U8);
    ARM_COMPUTE_ERROR_ON(nullptr == pyramid);
    ARM_COMPUTE_ERROR_ON(input->info()->num_dimensions() != pyramid->get_pyramid_level(0)->info()->num_dimensions());
    ARM_COMPUTE_ERROR_ON(input->info()->dimension(0) != pyramid->info()->width());
    ARM_COMPUTE_ERROR_ON(input->info()->dimension(1) != pyramid->info()->height());
    ARM_COMPUTE_ERROR_ON(SCALE_PYRAMID_HALF != pyramid->info()->scale());

    /* Get number of pyramid levels */
    const size_t num_levels = pyramid->info()->num_levels();

    _input   = input;
    _pyramid = pyramid;

    if(num_levels > 1)
    {
        _border_handler       = arm_compute::cpp14::make_unique<NEFillBorderKernel[]>(num_levels - 1);
        _horizontal_reduction = arm_compute::cpp14::make_unique<NEGaussianPyramidHorKernel[]>(num_levels - 1);
        _vertical_reduction   = arm_compute::cpp14::make_unique<NEGaussianPyramidVertKernel[]>(num_levels - 1);

        // Apply half scale to the X dimension of the tensor shape
        TensorShape tensor_shape = pyramid->info()->tensor_shape();
        tensor_shape.set(0, (pyramid->info()->width() + 1) * SCALE_PYRAMID_HALF);

        PyramidInfo pyramid_info(num_levels - 1, SCALE_PYRAMID_HALF, tensor_shape, Format::S16);
        _tmp.init(pyramid_info);

        for(unsigned int i = 0; i < num_levels - 1; ++i)
        {
            /* Configure horizontal kernel */
            _horizontal_reduction[i].configure(_pyramid->get_pyramid_level(i), _tmp.get_pyramid_level(i), border_mode == BorderMode::UNDEFINED);

            /* Configure vertical kernel */
            _vertical_reduction[i].configure(_tmp.get_pyramid_level(i), _pyramid->get_pyramid_level(i + 1), border_mode == BorderMode::UNDEFINED);

            /* Configure border */
            _border_handler[i].configure(_pyramid->get_pyramid_level(i), _horizontal_reduction[i].border_size(), border_mode, PixelValue(constant_border_value));
        }

        _tmp.allocate();
    }
}

void NEGaussianPyramidHalf::run()
{
    ARM_COMPUTE_ERROR_ON_MSG(_pyramid == nullptr, "Unconfigured function");

    /* Get number of pyramid levels */
    const size_t num_levels = _pyramid->info()->num_levels();

    /* The first level of the pyramid has the input image */
    _pyramid->get_pyramid_level(0)->copy_from(*_input);

    for(unsigned int i = 0; i < num_levels - 1; ++i)
    {
        _border_handler[i].run(_border_handler[i].window());
        NEScheduler::get().multithread(_horizontal_reduction.get() + i);
        NEScheduler::get().multithread(_vertical_reduction.get() + i);
    }
}

NEGaussianPyramidOrb::NEGaussianPyramidOrb()
    : _offsets(), _gaus5x5(), _scale_nearest()
{
}

void NEGaussianPyramidOrb::configure(const ITensor *input, IPyramid *pyramid, BorderMode border_mode, uint8_t constant_border_value)
{
    ARM_COMPUTE_ERROR_ON_DATA_TYPE_CHANNEL_NOT_IN(input, 1, DataType::U8);
    ARM_COMPUTE_ERROR_ON(nullptr == pyramid);
    ARM_COMPUTE_ERROR_ON(input->info()->num_dimensions() != pyramid->get_pyramid_level(0)->info()->num_dimensions());
    ARM_COMPUTE_ERROR_ON(input->info()->dimension(0) != pyramid->info()->width());
    ARM_COMPUTE_ERROR_ON(input->info()->dimension(1) != pyramid->info()->height());
    ARM_COMPUTE_ERROR_ON(SCALE_PYRAMID_ORB != pyramid->info()->scale());

    /* Get number of pyramid levels */
    const size_t num_levels = pyramid->info()->num_levels();

    _input   = input;
    _pyramid = pyramid;

    if(num_levels > 1)
    {
        _gaus5x5       = arm_compute::cpp14::make_unique<NEGaussian5x5[]>(num_levels - 1);
        _scale_nearest = arm_compute::cpp14::make_unique<NEScaleKernel[]>(num_levels - 1);
        _offsets       = arm_compute::cpp14::make_unique<Image[]>(num_levels - 1);

        PyramidInfo pyramid_info(num_levels - 1, SCALE_PYRAMID_ORB, pyramid->info()->tensor_shape(), Format::U8);
        _tmp.init(pyramid_info);

        for(unsigned int i = 0; i < num_levels - 1; ++i)
        {
            const size_t width  = _pyramid->get_pyramid_level(i + 1)->info()->dimension(0);
            const size_t height = _pyramid->get_pyramid_level(i + 1)->info()->dimension(1);

            /* Allocate Image for the offsets used by NEAREST interpolation */
            TensorInfo tensor_info(TensorShape(width, height), Format::S32);
            _offsets[i].allocator()->init(tensor_info);

            /* Configure gaussian 5x5 */
            _gaus5x5[i].configure(_pyramid->get_pyramid_level(i), _tmp.get_pyramid_level(i), border_mode, constant_border_value);

            /* Configure scale image kernel */
            _scale_nearest[i].configure(_tmp.get_pyramid_level(i), nullptr, nullptr, _offsets.get() + i, _pyramid->get_pyramid_level(i + 1), InterpolationPolicy::NEAREST_NEIGHBOR,
                                        border_mode == BorderMode::UNDEFINED);

            _offsets[i].allocator()->allocate();
        }

        _tmp.allocate();
    }
}

void NEGaussianPyramidOrb::run()
{
    ARM_COMPUTE_ERROR_ON_MSG(_pyramid == nullptr, "Unconfigured function");

    /* Get number of pyramid levels */
    const size_t num_levels = _pyramid->info()->num_levels();

    /* The first level of the pyramid has the input image */
    _pyramid->get_pyramid_level(0)->copy_from(*_input);

    for(unsigned int i = 0; i < num_levels - 1; ++i)
    {
        _gaus5x5[i].run();
        NEScheduler::get().multithread(_scale_nearest.get() + i);
    }
}
