/*
  Copyright 1999-2016 ImageMagick Studio LLC, a non-profit organization
  dedicated to making software imaging solutions freely available.

  You may not use this file except in compliance with the License.
  obtain a copy of the License at

    http://www.imagemagick.org/script/license.php

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  MagickCore acceleration methods.
*/
#ifndef _MAGICKCORE_ACCELERATE_H
#define _MAGICKCORE_ACCELERATE_H

#include "MagickCore/fx.h"
#include "MagickCore/morphology.h"
#include "MagickCore/resample.h"
#include "MagickCore/resize.h"
#include "MagickCore/statistic.h"

#if defined(__cplusplus) || defined(c_plusplus)
extern "C" {
#endif

extern MagickExport Image
  *AccelerateAddNoiseImage(const Image*,const NoiseType,ExceptionInfo *),
  *AccelerateBlurImage(const Image *,const double,const double,ExceptionInfo *),
  *AccelerateConvolveImage(const Image *,const KernelInfo *,ExceptionInfo *),
  *AccelerateDespeckleImage(const Image *,ExceptionInfo *),
  *AccelerateLocalContrastImage(const Image *,const double,const double,
    ExceptionInfo *),
  *AccelerateMotionBlurImage(const Image*,const double*,const size_t,
    const OffsetInfo*,ExceptionInfo*),
  *AccelerateResizeImage(const Image *,const size_t,const size_t,
    const ResizeFilter *,ExceptionInfo *),
  *AccelerateRotationalBlurImage(const Image *,const double,ExceptionInfo *),
  *AccelerateUnsharpMaskImage(const Image *,const double,const double,
    const double,const double,ExceptionInfo *),
  *AccelerateWaveletDenoiseImage(const Image *,const double,ExceptionInfo *);

extern MagickExport MagickBooleanType
  AccelerateCompositeImage(Image *,const CompositeOperator,const Image *,
    const float,const float,ExceptionInfo *),
  AccelerateContrastImage(Image *,const MagickBooleanType,ExceptionInfo *),
  AccelerateContrastStretchImage(Image *,const double,const double,
    ExceptionInfo*),
  AccelerateEqualizeImage(Image *,ExceptionInfo *),
  AccelerateFunctionImage(Image *,const MagickFunction,const size_t,
    const double *,ExceptionInfo *),
  AccelerateGrayscaleImage(Image *,const PixelIntensityMethod,
    ExceptionInfo *),
  AccelerateModulateImage(Image *,const double,const double,const double,
    const ColorspaceType, ExceptionInfo*),
  AccelerateRandomImage(Image*,ExceptionInfo*);

#if defined(__cplusplus) || defined(c_plusplus)
}
#endif

#endif
