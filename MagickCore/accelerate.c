/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     AAA     CCCC    CCCC  EEEEE  L      EEEEE  RRRR    AAA   TTTTT  EEEEE   %
%    A   A   C       C      E      L      E      R   R  A   A    T    E       %
%    AAAAA   C       C      EEE    L      EEE    RRRR   AAAAA    T    EEE     %
%    A   A   C       C      E      L      E      R R    A   A    T    E       %
%    A   A    CCCC    CCCC  EEEEE  LLLLL  EEEEE  R  R   A   A    T    EEEEE   %
%                                                                             %
%                                                                             %
%                       MagickCore Acceleration Methods                       %
%                                                                             %
%                              Software Design                                %
%                                  Cristy                                     %
%                               SiuChi Chan                                   %
%                               Guansong Zhang                                %
%                               January 2010                                  %
%                                                                             %
%                                                                             %
%  Copyright 1999-2016 ImageMagick Studio LLC, a non-profit organization      %
%  dedicated to making software imaging solutions freely available.           %
%                                                                             %
%  You may not use this file except in compliance with the License.  You may  %
%  obtain a copy of the License at                                            %
%                                                                             %
%    http://www.imagemagick.org/script/license.php                            %
%                                                                             %
%  Unless required by applicable law or agreed to in writing, software        %
%  distributed under the License is distributed on an "AS IS" BASIS,          %
%  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.   %
%  See the License for the specific language governing permissions and        %
%  limitations under the License.                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/
 
/*
Include declarations.
*/
#include "MagickCore/studio.h"
#include "MagickCore/accelerate.h"
#include "MagickCore/accelerate-private.h"
#include "MagickCore/artifact.h"
#include "MagickCore/cache.h"
#include "MagickCore/cache-private.h"
#include "MagickCore/cache-view.h"
#include "MagickCore/color-private.h"
#include "MagickCore/delegate-private.h"
#include "MagickCore/enhance.h"
#include "MagickCore/exception.h"
#include "MagickCore/exception-private.h"
#include "MagickCore/gem.h"
#include "MagickCore/image.h"
#include "MagickCore/image-private.h"
#include "MagickCore/linked-list.h"
#include "MagickCore/list.h"
#include "MagickCore/memory_.h"
#include "MagickCore/monitor-private.h"
#include "MagickCore/accelerate.h"
#include "MagickCore/opencl.h"
#include "MagickCore/opencl-private.h"
#include "MagickCore/option.h"
#include "MagickCore/pixel-accessor.h"
#include "MagickCore/pixel-private.h"
#include "MagickCore/prepress.h"
#include "MagickCore/quantize.h"
#include "MagickCore/quantum-private.h"
#include "MagickCore/random_.h"
#include "MagickCore/random-private.h"
#include "MagickCore/registry.h"
#include "MagickCore/resize.h"
#include "MagickCore/resize-private.h"
#include "MagickCore/semaphore.h"
#include "MagickCore/splay-tree.h"
#include "MagickCore/statistic.h"
#include "MagickCore/string_.h"
#include "MagickCore/string-private.h"
#include "MagickCore/token.h"

#ifdef MAGICKCORE_CLPERFMARKER
#include "CLPerfMarker.h"
#endif

#define MAGICK_MAX(x,y) (((x) >= (y))?(x):(y))
#define MAGICK_MIN(x,y) (((x) <= (y))?(x):(y))

#if defined(MAGICKCORE_OPENCL_SUPPORT)

/*
  Define declarations.
*/
#define ALIGNED(pointer,type) ((((size_t)(pointer)) & (sizeof(type)-1)) == 0)

/*
  Static declarations.
*/
static const ResizeWeightingFunctionType supportedResizeWeighting[] =
{
  BoxWeightingFunction,
  TriangleWeightingFunction,
  HannWeightingFunction,
  HammingWeightingFunction,
  BlackmanWeightingFunction,
  CubicBCWeightingFunction,
  SincWeightingFunction,
  SincFastWeightingFunction,
  LastWeightingFunction
};

/*
  Forward declarations.
*/
static Image *ComputeUnsharpMaskImageSingle(const Image *image,
  const double radius,const double sigma,const double gain,
  const double threshold,int blurOnly,ExceptionInfo *exception);

/*
  Helper functions.
*/
static MagickBooleanType checkAccelerateCondition(const Image* image)
{
  /* check if the image's colorspace is supported */
  if (image->colorspace != RGBColorspace &&
      image->colorspace != sRGBColorspace &&
      image->colorspace != GRAYColorspace)
    return(MagickFalse);

  /* check if the virtual pixel method is compatible with the OpenCL implementation */
  if ((GetImageVirtualPixelMethod(image) != UndefinedVirtualPixelMethod) &&
      (GetImageVirtualPixelMethod(image) != EdgeVirtualPixelMethod))
    return(MagickFalse);

  /* check if the image has read / write mask */
  if (image->read_mask != MagickFalse || image->write_mask != MagickFalse)
    return(MagickFalse);

  /* check if the image has an alpha channel */
  if (image->alpha_trait == UndefinedPixelTrait)
    return(MagickFalse);

  /* check if pixel order is RGBA */
  if (GetPixelChannelOffset(image,RedPixelChannel) != 0 ||
      GetPixelChannelOffset(image,GreenPixelChannel) != 1 ||
      GetPixelChannelOffset(image,BluePixelChannel) != 2 ||
      GetPixelChannelOffset(image,AlphaPixelChannel) != 3)
    return(MagickFalse);

  /* check if all channels are available */
  if ((GetPixelRedTraits(image) == UndefinedPixelTrait) ||
      (GetPixelGreenTraits(image) == UndefinedPixelTrait) ||
      (GetPixelBlueTraits(image) == UndefinedPixelTrait) ||
      (GetPixelAlphaTraits(image) == UndefinedPixelTrait))
    return(MagickFalse);

  return(MagickTrue);
}

static MagickBooleanType checkHistogramCondition(Image *image)
{
  /* ensure this is the only pass get in for now. */
  if ((image->channel_mask & SyncChannels) == 0)
    return MagickFalse;

  if (image->intensity == Rec601LuminancePixelIntensityMethod ||
      image->intensity == Rec709LuminancePixelIntensityMethod)
    return MagickFalse;

  if (image->colorspace != sRGBColorspace)
    return MagickFalse;

  return MagickTrue;
}

static MagickBooleanType checkOpenCLEnvironment(ExceptionInfo* exception)
{
  MagickBooleanType
    flag;

  MagickCLEnv
    clEnv;

  clEnv=GetDefaultOpenCLEnv();

  GetMagickOpenCLEnvParam(clEnv,MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED,
    sizeof(MagickBooleanType),&flag,exception);
  if (flag != MagickFalse)
    return(MagickFalse);

  GetMagickOpenCLEnvParam(clEnv,MAGICK_OPENCL_ENV_PARAM_OPENCL_INITIALIZED,
    sizeof(MagickBooleanType),&flag,exception);
  if (flag == MagickFalse)
    {
      if (InitOpenCLEnv(clEnv,exception) == MagickFalse)
        return(MagickFalse);

      GetMagickOpenCLEnvParam(clEnv,MAGICK_OPENCL_ENV_PARAM_OPENCL_DISABLED,
        sizeof(MagickBooleanType),&flag,exception);
      if (flag != MagickFalse)
        return(MagickFalse);
    }

  return(MagickTrue);
}

/* pad the global workgroup size to the next multiple of 
   the local workgroup size */
inline static unsigned int padGlobalWorkgroupSizeToLocalWorkgroupSize(
  const unsigned int orgGlobalSize,const unsigned int localGroupSize) 
{
  return ((orgGlobalSize+(localGroupSize-1))/localGroupSize*localGroupSize);
}

static MagickBooleanType splitImage(const Image* image)
{
  MagickBooleanType
    split;

  MagickCLEnv
    clEnv;

  unsigned long
    allocSize,
    tempSize;

  clEnv=GetDefaultOpenCLEnv();

  allocSize=GetOpenCLDeviceMaxMemAllocSize(clEnv);
  tempSize=(unsigned long) (image->columns * image->rows * 4 * 4);

  split = ((tempSize > allocSize) ? MagickTrue : MagickFalse);
  return(split);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e A d d N o i s e I m a g e                           %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static Image *ComputeAddNoiseImage(const Image *image,
  const NoiseType noise_type,ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    inputPixelCount,
    pixelsPerWorkitem,
    clStatus;

  cl_uint
    seed0,
    seed1;

  cl_kernel
    addNoiseKernel;

  cl_event
    event;

  cl_mem_flags
    mem_flags;

  cl_mem
    filteredImageBuffer,
    imageBuffer;

  const char
    *option;

  const void
    *inputPixels;

  float
    attenuate;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  Image
    *filteredImage;

  RandomInfo
    **magick_restrict random_info;

  size_t
    global_work_size[1],
    local_work_size[1];

  unsigned int
    k,
    numRandomNumberPerPixel;

#if defined(MAGICKCORE_OPENMP_SUPPORT)
  unsigned long
    key;
#endif

  void
    *filteredPixels,
    *hostPtr;

  outputReady = MagickFalse;
  clEnv = NULL;
  inputPixels = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  filteredPixels = NULL;
  context = NULL;
  imageBuffer = NULL;
  filteredImageBuffer = NULL;
  queue = NULL;
  addNoiseKernel = NULL;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  image_view=AcquireVirtualCacheView(image,exception);
  inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }


  filteredImage = CloneImage(image,image->columns,image->rows,MagickTrue,exception);
  assert(filteredImage != NULL);
  if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
    goto cleanup;
  }
  filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
  filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
  if (filteredPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
    goto cleanup;
  }

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
    hostPtr = filteredPixels;
  }
  else 
  {
    mem_flags = CL_MEM_WRITE_ONLY;
    hostPtr = NULL;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  /* find out how many random numbers needed by pixel */
  numRandomNumberPerPixel = 0;
  {
    unsigned int numRandPerChannel = 0;
    switch (noise_type)
    {
    case UniformNoise:
    case ImpulseNoise:
    case LaplacianNoise:
    case RandomNoise:
    default:
      numRandPerChannel = 1;
      break;
    case GaussianNoise:
    case MultiplicativeGaussianNoise:
    case PoissonNoise:
      numRandPerChannel = 2;
      break;
    };

    if ((image->channel_mask & RedChannel) != 0)
      numRandomNumberPerPixel+=numRandPerChannel;
    if ((image->channel_mask & GreenChannel) != 0)
      numRandomNumberPerPixel+=numRandPerChannel;
    if ((image->channel_mask & BlueChannel) != 0)
      numRandomNumberPerPixel+=numRandPerChannel;
    if ((image->channel_mask & AlphaChannel) != 0)
      numRandomNumberPerPixel+=numRandPerChannel;
  }

  /* set up the random number generators */
  attenuate=1.0;
  option=GetImageArtifact(image,"attenuate");
  if (option != (char *) NULL)
    attenuate=StringToDouble(option,(char **) NULL);
  random_info=AcquireRandomInfoThreadSet();
#if defined(MAGICKCORE_OPENMP_SUPPORT)
  key=GetRandomSecretKey(random_info[0]);
  (void) key;
#endif

  addNoiseKernel = AcquireOpenCLKernel(clEnv,MAGICK_OPENCL_ACCELERATE,"AddNoise");

  {
    cl_uint computeUnitCount;
    cl_uint workItemCount;
    clEnv->library->clGetDeviceInfo(clEnv->device, CL_DEVICE_MAX_COMPUTE_UNITS, sizeof(cl_uint), &computeUnitCount, NULL);
    workItemCount = computeUnitCount * 2 * 256;			// 256 work items per group, 2 groups per CU
    inputPixelCount = (cl_int) (image->columns * image->rows);
    pixelsPerWorkitem = (inputPixelCount + workItemCount - 1) / workItemCount;
    pixelsPerWorkitem = ((pixelsPerWorkitem + 3) / 4) * 4;

    local_work_size[0] = 256;
    global_work_size[0] = workItemCount;
  }
  {
    RandomInfo* randomInfo = AcquireRandomInfo();
	const unsigned long* s = GetRandomInfoSeed(randomInfo);
	seed0 = s[0];
	GetPseudoRandomValue(randomInfo);
	seed1 = s[0];
	randomInfo = DestroyRandomInfo(randomInfo);
  }

  k = 0;
  clEnv->library->clSetKernelArg(addNoiseKernel,k++,sizeof(cl_mem),(void *)&imageBuffer);
  clEnv->library->clSetKernelArg(addNoiseKernel,k++,sizeof(cl_mem),(void *)&filteredImageBuffer);
  clEnv->library->clSetKernelArg(addNoiseKernel,k++,sizeof(cl_uint),(void *)&inputPixelCount);
  clEnv->library->clSetKernelArg(addNoiseKernel,k++,sizeof(cl_uint),(void *)&pixelsPerWorkitem);
  clEnv->library->clSetKernelArg(addNoiseKernel,k++,sizeof(ChannelType),(void *)&image->channel_mask);
  clEnv->library->clSetKernelArg(addNoiseKernel,k++,sizeof(NoiseType),(void *)&noise_type);
  attenuate=1.0f;
  option=GetImageArtifact(image,"attenuate");
  if (option != (char *) NULL)
    attenuate=(float)StringToDouble(option,(char **) NULL);
  clEnv->library->clSetKernelArg(addNoiseKernel,k++,sizeof(float),(void *)&attenuate);
  clEnv->library->clSetKernelArg(addNoiseKernel,k++,sizeof(cl_uint),(void *)&seed0);
  clEnv->library->clSetKernelArg(addNoiseKernel,k++,sizeof(cl_uint),(void *)&seed1);
  clEnv->library->clSetKernelArg(addNoiseKernel,k++,sizeof(unsigned int),(void *)&numRandomNumberPerPixel);

  clEnv->library->clEnqueueNDRangeKernel(queue,addNoiseKernel,1,NULL,global_work_size,NULL,0,NULL,&event);

  RecordProfileData(clEnv,AddNoiseKernel,event);
  clEnv->library->clReleaseEvent(event);

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (queue!=NULL)                  RelinquishOpenCLCommandQueue(clEnv, queue);
  if (addNoiseKernel!=NULL)         RelinquishOpenCLKernel(clEnv, addNoiseKernel);
  if (imageBuffer!=NULL)		    clEnv->library->clReleaseMemObject(imageBuffer);
  if (filteredImageBuffer!=NULL)	  clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (outputReady == MagickFalse && filteredImage != NULL) 
    filteredImage=DestroyImage(filteredImage);

  return(filteredImage);
}

MagickExport Image *AccelerateAddNoiseImage(const Image *image,
  const NoiseType noise_type,ExceptionInfo *exception)
{
  Image
    *filteredImage;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return NULL;

  filteredImage=ComputeAddNoiseImage(image,noise_type,exception);
  return(filteredImage);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e B l u r I m a g e                                   %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static Image *ComputeBlurImage(const Image* image,const double radius,
  const double sigma,ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  char
    geometry[MagickPathExtent];

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus;

  cl_kernel
    blurColumnKernel,
    blurRowKernel;

  cl_event
    event;

  cl_mem
    filteredImageBuffer,
    imageBuffer,
    imageKernelBuffer,
    tempImageBuffer;
  
  cl_mem_flags
    mem_flags;

  const void
    *inputPixels;

  float
    *kernelBufferPtr;

  Image
    *filteredImage;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  KernelInfo
    *kernel;

  unsigned int
    i,
    imageColumns,
    imageRows,
    kernelWidth;

  void
    *filteredPixels,
    *hostPtr;

  context = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  imageBuffer = NULL;
  tempImageBuffer = NULL;
  filteredImageBuffer = NULL;
  imageKernelBuffer = NULL;
  blurRowKernel = NULL;
  blurColumnKernel = NULL;
  queue = NULL;
  kernel = NULL;

  outputReady = MagickFalse;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  /* Create and initialize OpenCL buffers. */
  {
    image_view=AcquireVirtualCacheView(image,exception);
    inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
    if (inputPixels == (const void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
      goto cleanup;
    }
    /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
    if (ALIGNED(inputPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
    }
    else 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
    }
    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create output */
  {
    filteredImage = CloneImage(image,image->columns,image->rows,MagickTrue,exception);
    assert(filteredImage != NULL);
    if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
      goto cleanup;
    }
    filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
    filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
    if (filteredPixels == (void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
      goto cleanup;
    }

    if (ALIGNED(filteredPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
      hostPtr = filteredPixels;
    }
    else 
    {
      mem_flags = CL_MEM_WRITE_ONLY;
      hostPtr = NULL;
    }
    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create processing kernel */
  {
    (void) FormatLocaleString(geometry,MagickPathExtent,"blur:%.20gx%.20g;blur:%.20gx%.20g+90",radius,sigma,radius,sigma);
    kernel=AcquireKernelInfo(geometry,exception);
    if (kernel == (KernelInfo *) NULL)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "MemoryAllocationFailed.",".");
      goto cleanup;
    }

    imageKernelBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR, kernel->width * sizeof(float), NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
    kernelBufferPtr = (float*)clEnv->library->clEnqueueMapBuffer(queue, imageKernelBuffer, CL_TRUE, CL_MAP_WRITE, 0, kernel->width * sizeof(float), 0, NULL, NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueMapBuffer failed.",".");
      goto cleanup;
    }

    for (i = 0; i < kernel->width; i++)
    {
      kernelBufferPtr[i] = (float) kernel->values[i];
    }

    clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, imageKernelBuffer, kernelBufferPtr, 0, NULL, NULL);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
      goto cleanup;
    }
  }

  {

    /* create temp buffer */
    {
      length = image->columns * image->rows;
      tempImageBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_WRITE, length * 4 * sizeof(float), NULL, &clStatus);
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
        goto cleanup;
      }
    }

    /* get the OpenCL kernels */
    {
      blurRowKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "BlurRow");
      if (blurRowKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };

      blurColumnKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "BlurColumn");
      if (blurColumnKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };
    }

    {
      /* need logic to decide this value */
      int chunkSize = 256;

      {
        imageColumns = (unsigned int) image->columns;
        imageRows = (unsigned int) image->rows;

        /* set the kernel arguments */
        i = 0;
        clStatus=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&tempImageBuffer);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(ChannelType),&image->channel_mask);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&imageKernelBuffer);
        kernelWidth = (unsigned int) kernel->width;
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&kernelWidth);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&imageRows);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(CLPixelPacket)*(chunkSize+kernel->width),(void *) NULL);
        if (clStatus != CL_SUCCESS)
        {
          (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
          goto cleanup;
        }
      }

      /* launch the kernel */
      {
        size_t gsize[2];
        size_t wsize[2];

        gsize[0] = chunkSize*((image->columns+chunkSize-1)/chunkSize);
        gsize[1] = image->rows;
        wsize[0] = chunkSize;
        wsize[1] = 1;

		clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, blurRowKernel, 2, NULL, gsize, wsize, 0, NULL, &event);
        if (clStatus != CL_SUCCESS)
        {
          (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
          goto cleanup;
        }
        clEnv->library->clFlush(queue);
        RecordProfileData(clEnv,BlurRowKernel,event);
        clEnv->library->clReleaseEvent(event);
      }
    }

    {
      /* need logic to decide this value */
      int chunkSize = 256;

      {
        imageColumns = (unsigned int) image->columns;
        imageRows = (unsigned int) image->rows;

        /* set the kernel arguments */
        i = 0;
        clStatus=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_mem),(void *)&tempImageBuffer);
        clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_mem),(void *)&filteredImageBuffer);
        clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(ChannelType),&image->channel_mask);
        clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_mem),(void *)&imageKernelBuffer);
        kernelWidth = (unsigned int) kernel->width;
        clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&kernelWidth);
        clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
        clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&imageRows);
        clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_float4)*(chunkSize+kernel->width),(void *) NULL);
        if (clStatus != CL_SUCCESS)
        {
          (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
          goto cleanup;
        }
      }

      /* launch the kernel */
      {
        size_t gsize[2];
        size_t wsize[2];

        gsize[0] = image->columns;
        gsize[1] = chunkSize*((image->rows+chunkSize-1)/chunkSize);
        wsize[0] = 1;
        wsize[1] = chunkSize;

		clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, blurColumnKernel, 2, NULL, gsize, wsize, 0, NULL, &event);
        if (clStatus != CL_SUCCESS)
        {
          (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
          goto cleanup;
        }
        clEnv->library->clFlush(queue);
        RecordProfileData(clEnv,BlurColumnKernel,event);
        clEnv->library->clReleaseEvent(event);
      }
    }

  }

  /* get result */ 
  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (imageBuffer!=NULL)     clEnv->library->clReleaseMemObject(imageBuffer);
  if (tempImageBuffer!=NULL)      clEnv->library->clReleaseMemObject(tempImageBuffer);
  if (filteredImageBuffer!=NULL)  clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (imageKernelBuffer!=NULL)    clEnv->library->clReleaseMemObject(imageKernelBuffer);
  if (blurRowKernel!=NULL)        RelinquishOpenCLKernel(clEnv, blurRowKernel);
  if (blurColumnKernel!=NULL)     RelinquishOpenCLKernel(clEnv, blurColumnKernel);
  if (queue != NULL)              RelinquishOpenCLCommandQueue(clEnv, queue);
  if (kernel!=NULL)               DestroyKernelInfo(kernel);
  if (outputReady == MagickFalse && filteredImage != NULL)
    filteredImage=DestroyImage(filteredImage);
  return(filteredImage);
}

static Image* ComputeBlurImageSection(const Image* image,
  const double radius,const double sigma,ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  char
    geometry[MagickPathExtent];

  cl_command_queue
    queue;

  cl_int
    clStatus;

  cl_kernel
    blurColumnKernel,
    blurRowKernel;

  cl_event
    event;

  cl_mem
    imageBuffer,
    tempImageBuffer,
    filteredImageBuffer,
    imageKernelBuffer;

  cl_mem_flags
    mem_flags;

  cl_context
    context;
  
  const void
    *inputPixels;

  float
    *kernelBufferPtr;

  Image
    *filteredImage;

  KernelInfo
    *kernel;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  unsigned int
    i,
    imageColumns,
    imageRows,
    kernelWidth;

  void
    *filteredPixels,
    *hostPtr;

  context = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  imageBuffer = NULL;
  tempImageBuffer = NULL;
  filteredImageBuffer = NULL;
  imageKernelBuffer = NULL;
  blurRowKernel = NULL;
  blurColumnKernel = NULL;
  queue = NULL;
  kernel = NULL;

  outputReady = MagickFalse;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  /* Create and initialize OpenCL buffers. */
  {
    image_view=AcquireVirtualCacheView(image,exception);
    inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
    if (inputPixels == (const void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
      goto cleanup;
    }
    /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
    if (ALIGNED(inputPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
    }
    else 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
    }
    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create output */
  {
    filteredImage = CloneImage(image,image->columns,image->rows,MagickTrue,exception);
    assert(filteredImage != NULL);
    if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
      goto cleanup;
    }
    filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
    filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
    if (filteredPixels == (void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
      goto cleanup;
    }

    if (ALIGNED(filteredPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
      hostPtr = filteredPixels;
    }
    else 
    {
      mem_flags = CL_MEM_WRITE_ONLY;
      hostPtr = NULL;
    }
    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create processing kernel */
  {
    (void) FormatLocaleString(geometry,MagickPathExtent,"blur:%.20gx%.20g;blur:%.20gx%.20g+90",radius,sigma,radius,sigma);
    kernel=AcquireKernelInfo(geometry,exception);
    if (kernel == (KernelInfo *) NULL)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "MemoryAllocationFailed.",".");
      goto cleanup;
    }

    imageKernelBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR, kernel->width * sizeof(float), NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
    kernelBufferPtr = (float*)clEnv->library->clEnqueueMapBuffer(queue, imageKernelBuffer, CL_TRUE, CL_MAP_WRITE, 0, kernel->width * sizeof(float), 0, NULL, NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueMapBuffer failed.",".");
      goto cleanup;
    }

    for (i = 0; i < kernel->width; i++)
    {
      kernelBufferPtr[i] = (float) kernel->values[i];
    }

    clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, imageKernelBuffer, kernelBufferPtr, 0, NULL, NULL);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
      goto cleanup;
    }
  }

  {
    unsigned int offsetRows;
    unsigned int sec;

    /* create temp buffer */
    {
      length = image->columns * (image->rows / 2 + 1 + (kernel->width-1) / 2);
      tempImageBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_WRITE, length * 4 * sizeof(float), NULL, &clStatus);
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
        goto cleanup;
      }
    }

    /* get the OpenCL kernels */
    {
      blurRowKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "BlurSectionRow");
      if (blurRowKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };

      blurColumnKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "BlurSectionColumn");
      if (blurColumnKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };
    }

    for (sec = 0; sec < 2; sec++)
    {
      {
        /* need logic to decide this value */
        int chunkSize = 256;

        {
          imageColumns = (unsigned int) image->columns;
          if (sec == 0)
            imageRows = (unsigned int) (image->rows / 2 + (kernel->width-1) / 2);
          else
            imageRows = (unsigned int) ((image->rows - image->rows / 2) + (kernel->width-1) / 2);

          offsetRows = (unsigned int) (sec * image->rows / 2);

          kernelWidth = (unsigned int) kernel->width;

          /* set the kernel arguments */
          i = 0;
          clStatus=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
          clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&tempImageBuffer);
          clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(ChannelType),&image->channel_mask);
          clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&imageKernelBuffer);
          clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&kernelWidth);
          clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
          clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&imageRows);
          clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(CLPixelPacket)*(chunkSize+kernel->width),(void *) NULL);
          clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&offsetRows);
          clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&sec);
          if (clStatus != CL_SUCCESS)
          {
            (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
            goto cleanup;
          }
        }

        /* launch the kernel */
        {
          size_t gsize[2];
          size_t wsize[2];

          gsize[0] = chunkSize*((imageColumns+chunkSize-1)/chunkSize);
          gsize[1] = imageRows;
          wsize[0] = chunkSize;
          wsize[1] = 1;

		  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, blurRowKernel, 2, NULL, gsize, wsize, 0, NULL, &event);
          if (clStatus != CL_SUCCESS)
          {
            (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
            goto cleanup;
          }
          clEnv->library->clFlush(queue);
          RecordProfileData(clEnv,BlurRowKernel,event);
          clEnv->library->clReleaseEvent(event);
        }
      }

      {
        /* need logic to decide this value */
        int chunkSize = 256;

        {
          imageColumns = (unsigned int) image->columns;
          if (sec == 0)
            imageRows = (unsigned int) (image->rows / 2);
          else
            imageRows = (unsigned int) ((image->rows - image->rows / 2));

          offsetRows = (unsigned int) (sec * image->rows / 2);

          kernelWidth = (unsigned int) kernel->width;

          /* set the kernel arguments */
          i = 0;
          clStatus=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_mem),(void *)&tempImageBuffer);
          clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_mem),(void *)&filteredImageBuffer);
          clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(ChannelType),&image->channel_mask);
          clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_mem),(void *)&imageKernelBuffer);
          clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&kernelWidth);
          clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
          clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&imageRows);
          clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_float4)*(chunkSize+kernel->width),(void *) NULL);
          clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&offsetRows);
          clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&sec);
          if (clStatus != CL_SUCCESS)
          {
            (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
            goto cleanup;
          }
        }

        /* launch the kernel */
        {
          size_t gsize[2];
          size_t wsize[2];

          gsize[0] = imageColumns;
          gsize[1] = chunkSize*((imageRows+chunkSize-1)/chunkSize);
          wsize[0] = 1;
          wsize[1] = chunkSize;

		  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, blurColumnKernel, 2, NULL, gsize, wsize, 0, NULL, &event);
          if (clStatus != CL_SUCCESS)
          {
            (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
            goto cleanup;
          }
          clEnv->library->clFlush(queue);
          RecordProfileData(clEnv,BlurColumnKernel,event);
          clEnv->library->clReleaseEvent(event);
        }
      }
    }

  }

  /* get result */
  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (imageBuffer!=NULL)     clEnv->library->clReleaseMemObject(imageBuffer);
  if (tempImageBuffer!=NULL)      clEnv->library->clReleaseMemObject(tempImageBuffer);
  if (filteredImageBuffer!=NULL)  clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (imageKernelBuffer!=NULL)    clEnv->library->clReleaseMemObject(imageKernelBuffer);
  if (blurRowKernel!=NULL)        RelinquishOpenCLKernel(clEnv, blurRowKernel);
  if (blurColumnKernel!=NULL)     RelinquishOpenCLKernel(clEnv, blurColumnKernel);
  if (queue != NULL)              RelinquishOpenCLCommandQueue(clEnv, queue);
  if (kernel!=NULL)               DestroyKernelInfo(kernel);
  if (outputReady == MagickFalse)
  {
    if (filteredImage != NULL)
    {
      DestroyImage(filteredImage);
      filteredImage = NULL;
    }
  }
  return filteredImage;
}

static Image* ComputeBlurImageSingle(const Image* image,
  const double radius,const double sigma,ExceptionInfo *exception)
{
  return ComputeUnsharpMaskImageSingle(image,radius,sigma,0.0,0.0,1,exception);
}

MagickExport Image* AccelerateBlurImage(const Image *image,
  const double radius,const double sigma,ExceptionInfo *exception)
{
  Image
    *filteredImage;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return NULL;

  if (radius < 12.1)
    filteredImage=ComputeBlurImageSingle(image,radius,sigma,exception);
  else if (splitImage(image) && (image->rows / 2 > radius)) 
    filteredImage=ComputeBlurImageSection(image,radius,sigma,exception);
  else
    filteredImage=ComputeBlurImage(image,radius,sigma,exception);
  return(filteredImage);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e C o m p o s i t e I m a g e                         %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static MagickBooleanType LaunchCompositeKernel(MagickCLEnv clEnv,
  cl_command_queue queue,cl_mem imageBuffer,const unsigned int inputWidth,
  const unsigned int inputHeight,const unsigned int matte,
  const ChannelType channel,const CompositeOperator compose,
  const cl_mem compositeImageBuffer,const unsigned int compositeWidth,
  const unsigned int compositeHeight,const float destination_dissolve,
  const float source_dissolve,ExceptionInfo *magick_unused(exception))
{
  cl_int
    clStatus;

  cl_kernel
    compositeKernel;

  cl_event
    event;

  int
    k;

  size_t
    global_work_size[2],
    local_work_size[2];

  unsigned int
    composeOp;

  magick_unreferenced(exception);

  compositeKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE,
    "Composite");

  k = 0;
  clStatus=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(cl_mem),(void*)&imageBuffer);
  clStatus|=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(unsigned int),(void*)&inputWidth);
  clStatus|=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(unsigned int),(void*)&inputHeight);
  clStatus|=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(cl_mem),(void*)&compositeImageBuffer);
  clStatus|=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(unsigned int),(void*)&compositeWidth);
  clStatus|=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(unsigned int),(void*)&compositeHeight);
  composeOp = (unsigned int)compose;
  clStatus|=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(unsigned int),(void*)&composeOp);
  clStatus|=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(ChannelType),(void*)&channel);
  clStatus|=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(unsigned int),(void*)&matte);
  clStatus|=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(float),(void*)&destination_dissolve);
  clStatus|=clEnv->library->clSetKernelArg(compositeKernel,k++,sizeof(float),(void*)&source_dissolve);

  if (clStatus!=CL_SUCCESS)
    return MagickFalse;

  local_work_size[0] = 64;
  local_work_size[1] = 1;

  global_work_size[0] = padGlobalWorkgroupSizeToLocalWorkgroupSize(inputWidth,
    (unsigned int) local_work_size[0]);
  global_work_size[1] = inputHeight;
  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, compositeKernel, 2, NULL, 
	  global_work_size, local_work_size, 0, NULL, &event);

  RecordProfileData(clEnv,CompositeKernel,event);
  clEnv->library->clReleaseEvent(event);

  RelinquishOpenCLKernel(clEnv, compositeKernel);

  return((clStatus==CL_SUCCESS) ? MagickTrue : MagickFalse);
}

static MagickBooleanType ComputeCompositeImage(Image *image,
  const CompositeOperator compose,const Image *compositeImage,
  const float destination_dissolve,const float source_dissolve,ExceptionInfo *exception)
{
  CacheView
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus;

  cl_mem_flags
    mem_flags;

  cl_mem
    compositeImageBuffer,
    imageBuffer;

  const void
    *composePixels;

  MagickBooleanType
    outputReady,
    status;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  void
    *inputPixels;

  status = MagickFalse;
  outputReady = MagickFalse;
  composePixels = NULL;
  imageBuffer = NULL;
  compositeImageBuffer = NULL;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  /* Create and initialize OpenCL buffers. */
  image_view=AcquireAuthenticCacheView(image,exception);
  inputPixels=GetCacheViewAuthenticPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,
      "UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, 
    length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), 
      ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }


  /* Create and initialize OpenCL buffers. */
  composePixels = AcquirePixelCachePixels(compositeImage, &length, exception); 
  if (composePixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,
      "UnableToReadPixelCache.","`%s'",compositeImage->filename);
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(composePixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = compositeImage->columns * compositeImage->rows;
  compositeImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, 
    length * sizeof(CLPixelPacket), (void*)composePixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), 
      ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }
  
  status = LaunchCompositeKernel(clEnv,queue,imageBuffer,
           (unsigned int) image->columns,
           (unsigned int) image->rows,
           (unsigned int) (image->alpha_trait > CopyPixelTrait) ? 1 : 0,
           image->channel_mask, compose, compositeImageBuffer,
           (unsigned int) compositeImage->columns,
           (unsigned int) compositeImage->rows,
           destination_dissolve,source_dissolve,
           exception);

  if (status==MagickFalse)
    goto cleanup;

  length = image->columns * image->rows;
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    clEnv->library->clEnqueueMapBuffer(queue, imageBuffer, CL_TRUE, 
      CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, 
      NULL, &clStatus);
  }
  else
  {
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, imageBuffer, CL_TRUE, 0, 
      length * sizeof(CLPixelPacket), inputPixels, 0, NULL, NULL);
  }
  if (clStatus==CL_SUCCESS)
    outputReady=SyncCacheViewAuthenticPixels(image_view,exception);

cleanup:

  image_view=DestroyCacheView(image_view);
  if (imageBuffer!=NULL)      clEnv->library->clReleaseMemObject(imageBuffer);
  if (compositeImageBuffer!=NULL)  clEnv->library->clReleaseMemObject(compositeImageBuffer);
  if (queue != NULL)               RelinquishOpenCLCommandQueue(clEnv, queue);

  return(outputReady);
}

MagickExport MagickBooleanType AccelerateCompositeImage(Image *image,
  const CompositeOperator compose,const Image *composite,
  const float destination_dissolve,const float source_dissolve,
  ExceptionInfo *exception)
{
  MagickBooleanType
    status;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return(MagickFalse);

  /* only support images with the size for now */
  if ((image->columns != composite->columns) ||
      (image->rows != composite->rows))
    return MagickFalse;

  switch(compose)
  {
    case ColorDodgeCompositeOp: 
    case BlendCompositeOp:
      break;
    default:
      // unsupported compose operator, quit
      return MagickFalse;
  };

  status=ComputeCompositeImage(image,compose,composite,destination_dissolve,
    source_dissolve,exception);
  return(status);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e C o n t r a s t I m a g e                           %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static MagickBooleanType ComputeContrastImage(Image *image,
  const MagickBooleanType sharpen,ExceptionInfo *exception)
{
  CacheView
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus;

  cl_kernel
    filterKernel;

  cl_event
    event;

  cl_mem
    imageBuffer;

  cl_mem_flags
    mem_flags;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  size_t
    global_work_size[2];

  unsigned int
    i,
    uSharpen;

  void
    *inputPixels;

  outputReady = MagickFalse;
  clEnv = NULL;
  inputPixels = NULL;
  context = NULL;
  imageBuffer = NULL;
  filterKernel = NULL;
  queue = NULL;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);

  /* Create and initialize OpenCL buffers. */
  image_view=AcquireAuthenticCacheView(image,exception);
  inputPixels=GetCacheViewAuthenticPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }
  
  filterKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "Contrast");
  if (filterKernel == NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }

  i = 0;
  clStatus=clEnv->library->clSetKernelArg(filterKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);

  uSharpen = (sharpen == MagickFalse)?0:1;
  clStatus|=clEnv->library->clSetKernelArg(filterKernel,i++,sizeof(cl_uint),&uSharpen);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }

  global_work_size[0] = image->columns;
  global_work_size[1] = image->rows;
  /* launch the kernel */
  queue = AcquireOpenCLCommandQueue(clEnv);
  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, filterKernel, 2, NULL, global_work_size, NULL, 0, NULL, &event);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);
  RecordProfileData(clEnv,ContrastKernel,event);
  clEnv->library->clReleaseEvent(event);

  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, imageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, imageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), inputPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }
  outputReady=SyncCacheViewAuthenticPixels(image_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);

  if (imageBuffer!=NULL)		      clEnv->library->clReleaseMemObject(imageBuffer);
  if (filterKernel!=NULL)                     RelinquishOpenCLKernel(clEnv, filterKernel);
  if (queue != NULL)                          RelinquishOpenCLCommandQueue(clEnv, queue);
  return(outputReady);
}

MagickExport MagickBooleanType AccelerateContrastImage(Image *image,
  const MagickBooleanType sharpen,ExceptionInfo *exception)
{
  MagickBooleanType
    status;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return(MagickFalse);

  status=ComputeContrastImage(image,sharpen,exception);
  return(status);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e C o n t r a s t S t r e t c h I m a g e             %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static MagickBooleanType LaunchHistogramKernel(MagickCLEnv clEnv,
  cl_command_queue queue,cl_mem imageBuffer,cl_mem histogramBuffer,
  Image *image,const ChannelType channel,ExceptionInfo *exception)
{
  MagickBooleanType
    outputReady;

  cl_int
    clStatus,
    colorspace,
    method;

  cl_kernel
    histogramKernel; 

  cl_event
    event;

  register ssize_t
    i;

  size_t
    global_work_size[2];

  histogramKernel = NULL; 

  outputReady = MagickFalse;
  method = image->intensity;
  colorspace = image->colorspace;

  /* get the OpenCL kernel */
  histogramKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "Histogram");
  if (histogramKernel == NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }

  /* set the kernel arguments */
  i = 0;
  clStatus=clEnv->library->clSetKernelArg(histogramKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
  clStatus|=clEnv->library->clSetKernelArg(histogramKernel,i++,sizeof(ChannelType),&channel);
  clStatus|=clEnv->library->clSetKernelArg(histogramKernel,i++,sizeof(cl_int),&method);
  clStatus|=clEnv->library->clSetKernelArg(histogramKernel,i++,sizeof(cl_int),&colorspace);
  clStatus|=clEnv->library->clSetKernelArg(histogramKernel,i++,sizeof(cl_mem),(void *)&histogramBuffer);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }

  /* launch the kernel */
  global_work_size[0] = image->columns;
  global_work_size[1] = image->rows;

  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, histogramKernel, 2, NULL, global_work_size, NULL, 0, NULL, &event);

  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);
  RecordProfileData(clEnv,HistogramKernel,event);
  clEnv->library->clReleaseEvent(event);

  outputReady = MagickTrue;

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);
 
  if (histogramKernel!=NULL)                     
    RelinquishOpenCLKernel(clEnv, histogramKernel);

  return(outputReady);
}

static MagickBooleanType ComputeContrastStretchImage(Image *image,
  const double black_point,const double white_point,ExceptionInfo *exception)
{
#define ContrastStretchImageTag  "ContrastStretch/Image"
#define MaxRange(color)  ((MagickRealType) ScaleQuantumToMap((Quantum) (color)))

  CacheView
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus;

  cl_mem_flags
    mem_flags;

  cl_mem
    histogramBuffer,
    imageBuffer,
    stretchMapBuffer;

  cl_kernel
    histogramKernel,
    stretchKernel;

  cl_event
    event;

  cl_uint4
    *histogram;

  double
    intensity;

  FloatPixelPacket
    black,
    white;

  MagickBooleanType
    outputReady,
    status;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  PixelPacket
    *stretch_map;

  register ssize_t
    i;

  size_t
    global_work_size[2];

  void
    *hostPtr,
    *inputPixels;

  histogram=NULL;
  stretch_map=NULL;
  inputPixels = NULL;
  imageBuffer = NULL;
  histogramBuffer = NULL;
  stretchMapBuffer = NULL;
  histogramKernel = NULL; 
  stretchKernel = NULL; 
  context = NULL;
  queue = NULL;
  outputReady = MagickFalse;


  assert(image != (Image *) NULL);
  assert(image->signature == MagickCoreSignature);
  if (image->debug != MagickFalse)
    (void) LogMagickEvent(TraceEvent,GetMagickModule(),"%s",image->filename);

  //exception=(&image->exception);

  /*
   * initialize opencl env
   */
  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  /*
    Allocate and initialize histogram arrays.
  */
  histogram=(cl_uint4 *) AcquireQuantumMemory(MaxMap+1UL, sizeof(*histogram));

  if (histogram == (cl_uint4 *) NULL)
    ThrowBinaryException(ResourceLimitError,"MemoryAllocationFailed", image->filename);
 
  /* reset histogram */
  (void) ResetMagickMemory(histogram,0,(MaxMap+1)*sizeof(*histogram));

  /*
  if (IsGrayImage(image,exception) != MagickFalse)
    (void) SetImageColorspace(image,GRAYColorspace);
  */

  status=MagickTrue;


  /*
    Form histogram.
  */
  /* Create and initialize OpenCL buffers. */
  /* inputPixels = AcquirePixelCachePixels(image, &length, exception); */
  /* assume this  will get a writable image */
  image_view=AcquireAuthenticCacheView(image,exception);
  inputPixels=GetCacheViewAuthenticPixels(image_view,0,0,image->columns,image->rows,exception);

  if (inputPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }
  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of cl_uint, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(histogram,cl_uint4)) 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_USE_HOST_PTR;
    hostPtr = histogram;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
    hostPtr = histogram;
  }
  /* create a CL buffer for histogram  */
  length = (MaxMap+1); 
  histogramBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(cl_uint4), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  status = LaunchHistogramKernel(clEnv, queue, imageBuffer, histogramBuffer, image, image->channel_mask, exception);
  if (status == MagickFalse)
    goto cleanup;

  /* read from the kenel output */
  if (ALIGNED(histogram,cl_uint4)) 
  {
    length = (MaxMap+1); 
    clEnv->library->clEnqueueMapBuffer(queue, histogramBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(cl_uint4), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = (MaxMap+1); 
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, histogramBuffer, CL_TRUE, 0, length * sizeof(cl_uint4), histogram, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  /* unmap, don't block gpu to use this buffer again.  */
  if (ALIGNED(histogram,cl_uint4))
  {
    clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, histogramBuffer, histogram, 0, NULL, NULL);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
      goto cleanup;
    }
  }

  /* recreate input buffer later, in case image updated */
#ifdef RECREATEBUFFER 
  if (imageBuffer!=NULL)		      
    clEnv->library->clReleaseMemObject(imageBuffer);
#endif

  /* CPU stuff */
  /*
     Find the histogram boundaries by locating the black/white levels.
  */
  black.red=0.0;
  white.red=MaxRange(QuantumRange);
  if ((image->channel_mask & RedChannel) != 0)
  {
    intensity=0.0;
    for (i=0; i <= (ssize_t) MaxMap; i++)
    {
      intensity+=histogram[i].s[2];
      if (intensity > black_point)
        break;
    }
    black.red=(MagickRealType) i;
    intensity=0.0;
    for (i=(ssize_t) MaxMap; i != 0; i--)
    {
      intensity+=histogram[i].s[2];
      if (intensity > ((double) image->columns*image->rows-white_point))
        break;
    }
    white.red=(MagickRealType) i;
  }
  black.green=0.0;
  white.green=MaxRange(QuantumRange);
  if ((image->channel_mask & GreenChannel) != 0)
  {
    intensity=0.0;
    for (i=0; i <= (ssize_t) MaxMap; i++)
    {
      intensity+=histogram[i].s[2];
      if (intensity > black_point)
        break;
    }
    black.green=(MagickRealType) i;
    intensity=0.0;
    for (i=(ssize_t) MaxMap; i != 0; i--)
    {
      intensity+=histogram[i].s[2];
      if (intensity > ((double) image->columns*image->rows-white_point))
        break;
    }
    white.green=(MagickRealType) i;
  }
  black.blue=0.0;
  white.blue=MaxRange(QuantumRange);
  if ((image->channel_mask & BlueChannel) != 0)
  {
    intensity=0.0;
    for (i=0; i <= (ssize_t) MaxMap; i++)
    {
      intensity+=histogram[i].s[2];
      if (intensity > black_point)
        break;
    }
    black.blue=(MagickRealType) i;
    intensity=0.0;
    for (i=(ssize_t) MaxMap; i != 0; i--)
    {
      intensity+=histogram[i].s[2];
      if (intensity > ((double) image->columns*image->rows-white_point))
        break;
    }
    white.blue=(MagickRealType) i;
  }
  black.alpha=0.0;
  white.alpha=MaxRange(QuantumRange);
  if ((image->channel_mask & AlphaChannel) != 0)
  {
    intensity=0.0;
    for (i=0; i <= (ssize_t) MaxMap; i++)
    {
      intensity+=histogram[i].s[2];
      if (intensity > black_point)
        break;
    }
    black.alpha=(MagickRealType) i;
    intensity=0.0;
    for (i=(ssize_t) MaxMap; i != 0; i--)
    {
      intensity+=histogram[i].s[2];
      if (intensity > ((double) image->columns*image->rows-white_point))
        break;
    }
    white.alpha=(MagickRealType) i;
  }
  /*
  black.index=0.0;
  white.index=MaxRange(QuantumRange);
  if (((channel & IndexChannel) != 0) && (image->colorspace == CMYKColorspace))
  {
    intensity=0.0;
    for (i=0; i <= (ssize_t) MaxMap; i++)
    {
      intensity+=histogram[i].index;
      if (intensity > black_point)
        break;
    }
    black.index=(MagickRealType) i;
    intensity=0.0;
    for (i=(ssize_t) MaxMap; i != 0; i--)
    {
      intensity+=histogram[i].index;
      if (intensity > ((double) image->columns*image->rows-white_point))
        break;
    }
    white.index=(MagickRealType) i;
  }
  */


  stretch_map=(PixelPacket *) AcquireQuantumMemory(MaxMap+1UL,
    sizeof(*stretch_map));

  if (stretch_map == (PixelPacket *) NULL)
    ThrowBinaryException(ResourceLimitError,"MemoryAllocationFailed",
      image->filename);
 
  /*
    Stretch the histogram to create the stretched image mapping.
  */
  (void) ResetMagickMemory(stretch_map,0,(MaxMap+1)*sizeof(*stretch_map));
  for (i=0; i <= (ssize_t) MaxMap; i++)
  {
    if ((image->channel_mask & RedChannel) != 0)
    {
      if (i < (ssize_t) black.red)
        stretch_map[i].red=(Quantum) 0;
      else
        if (i > (ssize_t) white.red)
          stretch_map[i].red=QuantumRange;
        else
          if (black.red != white.red)
            stretch_map[i].red=ScaleMapToQuantum((MagickRealType) (MaxMap*
                  (i-black.red)/(white.red-black.red)));
    }
    if ((image->channel_mask & GreenChannel) != 0)
    {
      if (i < (ssize_t) black.green)
        stretch_map[i].green=0;
      else
        if (i > (ssize_t) white.green)
          stretch_map[i].green=QuantumRange;
        else
          if (black.green != white.green)
            stretch_map[i].green=ScaleMapToQuantum((MagickRealType) (MaxMap*
                  (i-black.green)/(white.green-black.green)));
    }
    if ((image->channel_mask & BlueChannel) != 0)
    {
      if (i < (ssize_t) black.blue)
        stretch_map[i].blue=0;
      else
        if (i > (ssize_t) white.blue)
          stretch_map[i].blue= QuantumRange;
        else
          if (black.blue != white.blue)
            stretch_map[i].blue=ScaleMapToQuantum((MagickRealType) (MaxMap*
                  (i-black.blue)/(white.blue-black.blue)));
    }
    if ((image->channel_mask & AlphaChannel) != 0)
    {
      if (i < (ssize_t) black.alpha)
        stretch_map[i].alpha=0;
      else
        if (i > (ssize_t) white.alpha)
          stretch_map[i].alpha=QuantumRange;
        else
          if (black.alpha != white.alpha)
            stretch_map[i].alpha=ScaleMapToQuantum((MagickRealType) (MaxMap*
                  (i-black.alpha)/(white.alpha-black.alpha)));
    }
    /*
    if (((channel & IndexChannel) != 0) &&
        (image->colorspace == CMYKColorspace))
    {
      if (i < (ssize_t) black.index)
        stretch_map[i].index=0;
      else
        if (i > (ssize_t) white.index)
          stretch_map[i].index=QuantumRange;
        else
          if (black.index != white.index)
            stretch_map[i].index=ScaleMapToQuantum((MagickRealType) (MaxMap*
                  (i-black.index)/(white.index-black.index)));
    }
    */
  }

  /*
    Stretch the image.
  */
  if (((image->channel_mask & AlphaChannel) != 0) || (((image->channel_mask & IndexChannel) != 0) &&
      (image->colorspace == CMYKColorspace)))
    image->storage_class=DirectClass;
  if (image->storage_class == PseudoClass)
  {
    /*
       Stretch colormap.
       */
    for (i=0; i < (ssize_t) image->colors; i++)
    {
      if ((image->channel_mask & RedChannel) != 0)
      {
        if (black.red != white.red)
          image->colormap[i].red=stretch_map[
            ScaleQuantumToMap(image->colormap[i].red)].red;
      }
      if ((image->channel_mask & GreenChannel) != 0)
      {
        if (black.green != white.green)
          image->colormap[i].green=stretch_map[
            ScaleQuantumToMap(image->colormap[i].green)].green;
      }
      if ((image->channel_mask & BlueChannel) != 0)
      {
        if (black.blue != white.blue)
          image->colormap[i].blue=stretch_map[
            ScaleQuantumToMap(image->colormap[i].blue)].blue;
      }
      if ((image->channel_mask & AlphaChannel) != 0)
      {
        if (black.alpha != white.alpha)
          image->colormap[i].alpha=stretch_map[
            ScaleQuantumToMap(image->colormap[i].alpha)].alpha;
      }
    }
  }

  /*
    Stretch image.
  */


  /* GPU can work on this again, image and equalize map as input
    image:        uchar4 (CLPixelPacket)
    stretch_map:  uchar4 (PixelPacket)
    black, white: float4 (FloatPixelPacket) */

#ifdef RECREATEBUFFER 
  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }
#endif

  /* Create and initialize OpenCL buffers. */
  if (ALIGNED(stretch_map, PixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
    hostPtr = stretch_map;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
    hostPtr = stretch_map;
  }
  /* create a CL buffer for stretch_map  */
  length = (MaxMap+1); 
  stretchMapBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(PixelPacket), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  /* get the OpenCL kernel */
  stretchKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "ContrastStretch");
  if (stretchKernel == NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }

  /* set the kernel arguments */
  i = 0;
  clStatus=clEnv->library->clSetKernelArg(stretchKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
  clStatus|=clEnv->library->clSetKernelArg(stretchKernel,i++,sizeof(ChannelType),&image->channel_mask);
  clStatus|=clEnv->library->clSetKernelArg(stretchKernel,i++,sizeof(cl_mem),(void *)&stretchMapBuffer);
  clStatus|=clEnv->library->clSetKernelArg(stretchKernel,i++,sizeof(FloatPixelPacket),&white);
  clStatus|=clEnv->library->clSetKernelArg(stretchKernel,i++,sizeof(FloatPixelPacket),&black);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }

  /* launch the kernel */
  global_work_size[0] = image->columns;
  global_work_size[1] = image->rows;

  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, stretchKernel, 2, NULL, global_work_size, NULL, 0, NULL, &event);

  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);

  RecordProfileData(clEnv,ContrastStretchKernel,event);
  clEnv->library->clReleaseEvent(event);

  /* read the data back */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, imageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, imageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), inputPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(image_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);

  if (imageBuffer!=NULL)		      
    clEnv->library->clReleaseMemObject(imageBuffer);

  if (stretchMapBuffer!=NULL)
    clEnv->library->clReleaseMemObject(stretchMapBuffer);
  if (stretch_map!=NULL)
    stretch_map=(PixelPacket *) RelinquishMagickMemory(stretch_map);


  if (histogramBuffer!=NULL)
    clEnv->library->clReleaseMemObject(histogramBuffer);
  if (histogram!=NULL)
    histogram=(cl_uint4 *) RelinquishMagickMemory(histogram);


  if (histogramKernel!=NULL)                     
    RelinquishOpenCLKernel(clEnv, histogramKernel);
  if (stretchKernel!=NULL)                     
    RelinquishOpenCLKernel(clEnv, stretchKernel);

  if (queue != NULL)                          
    RelinquishOpenCLCommandQueue(clEnv, queue);

  return(outputReady);
}

MagickExport MagickBooleanType AccelerateContrastStretchImage(
  Image *image,const double black_point,const double white_point,
  ExceptionInfo *exception)
{
  MagickBooleanType
    status;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkHistogramCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return(MagickFalse);

  status=ComputeContrastStretchImage(image,black_point,white_point,exception);
  return(status);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e C o n v o l v e I m a g e                           %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static Image *ComputeConvolveImage(const Image* image,const KernelInfo *kernel,
  ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_kernel
    clkernel;

  cl_event
    event;

  cl_int
    clStatus;

  cl_mem
    convolutionKernel,
    filteredImageBuffer,
    imageBuffer;

  cl_mem_flags
    mem_flags;

  cl_ulong
    deviceLocalMemorySize;

  const void
    *inputPixels;

  float
    *kernelBufferPtr;

  Image
    *filteredImage;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  size_t
    global_work_size[3],
    localGroupSize[3],
    localMemoryRequirement;

  unsigned
    kernelSize;

  unsigned int
    filterHeight,
    filterWidth,
    i,
    imageHeight,
    imageWidth,
    matte;

  void
    *filteredPixels,
    *hostPtr;

  /* intialize all CL objects to NULL */
  context = NULL;
  imageBuffer = NULL;
  filteredImageBuffer = NULL;
  convolutionKernel = NULL;
  clkernel = NULL;
  queue = NULL;

  filteredImage = NULL;
  filteredImage_view = NULL;
  outputReady = MagickFalse;
  
  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);

  image_view=AcquireVirtualCacheView(image,exception);
  inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (const void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  /* Create and initialize OpenCL buffers. */

  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  filteredImage = CloneImage(image,image->columns,image->rows,MagickTrue,exception);
  assert(filteredImage != NULL);
  if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
    goto cleanup;
  }
  filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
  filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
  if (filteredPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
    goto cleanup;
  }

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
    hostPtr = filteredPixels;
  }
  else 
  {
    mem_flags = CL_MEM_WRITE_ONLY;
    hostPtr = NULL;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  kernelSize = (unsigned int) (kernel->width * kernel->height);
  convolutionKernel = clEnv->library->clCreateBuffer(context, CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR, kernelSize * sizeof(float), NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  queue = AcquireOpenCLCommandQueue(clEnv);

  kernelBufferPtr = (float*)clEnv->library->clEnqueueMapBuffer(queue, convolutionKernel, CL_TRUE, CL_MAP_WRITE, 0, kernelSize * sizeof(float)
          , 0, NULL, NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueMapBuffer failed.",".");
    goto cleanup;
  }
  for (i = 0; i < kernelSize; i++)
  {
    kernelBufferPtr[i] = (float) kernel->values[i];
  }
  clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, convolutionKernel, kernelBufferPtr, 0, NULL, NULL);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);

  deviceLocalMemorySize = GetOpenCLDeviceLocalMemorySize(clEnv);

  /* Compute the local memory requirement for a 16x16 workgroup.
     If it's larger than 16k, reduce the workgroup size to 8x8 */
  localGroupSize[0] = 16;
  localGroupSize[1] = 16;
  localMemoryRequirement = (localGroupSize[0]+kernel->width-1) * (localGroupSize[1]+kernel->height-1) * sizeof(CLPixelPacket)
    + kernel->width*kernel->height*sizeof(float);

  if (localMemoryRequirement > deviceLocalMemorySize)
  {
    localGroupSize[0] = 8;
    localGroupSize[1] = 8;
    localMemoryRequirement = (localGroupSize[0]+kernel->width-1) * (localGroupSize[1]+kernel->height-1) * sizeof(CLPixelPacket)
      + kernel->width*kernel->height*sizeof(float);
  }
  if (localMemoryRequirement <= deviceLocalMemorySize) 
  {
    /* get the OpenCL kernel */
    clkernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "ConvolveOptimized");
    if (clkernel == NULL)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
      goto cleanup;
    }

    /* set the kernel arguments */
    i = 0;
    clStatus =clEnv->library->clSetKernelArg(clkernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(cl_mem),(void *)&filteredImageBuffer);
    imageWidth = (unsigned int) image->columns;
    imageHeight = (unsigned int) image->rows;
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&imageWidth);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&imageHeight);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(cl_mem),(void *)&convolutionKernel);
    filterWidth = (unsigned int) kernel->width;
    filterHeight = (unsigned int) kernel->height;
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&filterWidth);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&filterHeight);
    matte = (image->alpha_trait > CopyPixelTrait)?1:0;
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&matte);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(ChannelType),(void *)&image->channel_mask);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++, (localGroupSize[0] + kernel->width-1)*(localGroupSize[1] + kernel->height-1)*sizeof(CLPixelPacket),NULL);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++, kernel->width*kernel->height*sizeof(float),NULL);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
      goto cleanup;
    }

    /* pad the global size to a multiple of the local work size dimension */
    global_work_size[0] = ((image->columns + localGroupSize[0]  - 1)/localGroupSize[0] ) * localGroupSize[0] ;
    global_work_size[1] = ((image->rows + localGroupSize[1] - 1)/localGroupSize[1]) * localGroupSize[1];

    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, clkernel, 2, NULL, global_work_size, localGroupSize, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    RecordProfileData(clEnv,ConvolveKernel,event);
    clEnv->library->clReleaseEvent(event);
  }
  else
  {
    /* get the OpenCL kernel */
    clkernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "Convolve");
    if (clkernel == NULL)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
      goto cleanup;
    }

    /* set the kernel arguments */
    i = 0;
    clStatus =clEnv->library->clSetKernelArg(clkernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(cl_mem),(void *)&filteredImageBuffer);
    imageWidth = (unsigned int) image->columns;
    imageHeight = (unsigned int) image->rows;
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&imageWidth);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&imageHeight);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(cl_mem),(void *)&convolutionKernel);
    filterWidth = (unsigned int) kernel->width;
    filterHeight = (unsigned int) kernel->height;
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&filterWidth);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&filterHeight);
    matte = (image->alpha_trait > CopyPixelTrait)?1:0;
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&matte);
    clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(ChannelType),(void *)&image->channel_mask);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
      goto cleanup;
    }

    localGroupSize[0] = 8;
    localGroupSize[1] = 8;
    global_work_size[0] = (image->columns + (localGroupSize[0]-1))/localGroupSize[0] * localGroupSize[0];
    global_work_size[1] = (image->rows    + (localGroupSize[1]-1))/localGroupSize[1] * localGroupSize[1];
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, clkernel, 2, NULL, global_work_size, localGroupSize, 0, NULL, &event);
    
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
  }
  clEnv->library->clFlush(queue);
  RecordProfileData(clEnv,ConvolveKernel,event);
  clEnv->library->clReleaseEvent(event);

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (imageBuffer != NULL)
    clEnv->library->clReleaseMemObject(imageBuffer);

  if (filteredImageBuffer != NULL)
    clEnv->library->clReleaseMemObject(filteredImageBuffer);

  if (convolutionKernel != NULL)
    clEnv->library->clReleaseMemObject(convolutionKernel);

  if (clkernel != NULL)
    RelinquishOpenCLKernel(clEnv, clkernel);

  if (queue != NULL)
    RelinquishOpenCLCommandQueue(clEnv, queue);

  if (outputReady == MagickFalse)
  {
    if (filteredImage != NULL)
    {
      DestroyImage(filteredImage);
      filteredImage = NULL;
    }
  }

  return(filteredImage);
}

MagickExport Image *AccelerateConvolveImage(const Image *image,
  const KernelInfo *kernel,ExceptionInfo *exception)
{
  /* Temporary disabled due to access violation

  Image
    *filteredImage;

  assert(image != NULL);
  assert(kernel != (KernelInfo *) NULL);
  assert(exception != (ExceptionInfo *) NULL);
  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return((Image *) NULL);

  filteredImage=ComputeConvolveImage(image,kernel,exception);
  return(filteredImage);
  */
  magick_unreferenced(image);
  magick_unreferenced(kernel);
  magick_unreferenced(exception);
  return((Image *)NULL);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e D e s p e c k l e I m a g e                         %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static Image *ComputeDespeckleImage(const Image *image,
  ExceptionInfo*exception)
{
  static const int 
    X[4] = {0, 1, 1,-1},
    Y[4] = {1, 0, 1, 1};

  CacheView
    *filteredImage_view,
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus;

  cl_kernel
    hullPass1,
    hullPass2;

  cl_event
    event;

  cl_mem_flags
    mem_flags;

  cl_mem
    filteredImageBuffer,
    imageBuffer,
    tempImageBuffer[2];

  const void
    *inputPixels;

  Image
    *filteredImage;

  int
    k,
    matte;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  size_t
    global_work_size[2];

  unsigned int
    imageHeight,
    imageWidth;

  void
    *filteredPixels,
    *hostPtr;

  outputReady = MagickFalse;
  clEnv = NULL;
  inputPixels = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  filteredPixels = NULL;
  context = NULL;
  imageBuffer = NULL;
  filteredImageBuffer = NULL;
  hullPass1 = NULL;
  hullPass2 = NULL;
  queue = NULL;
  tempImageBuffer[0] = tempImageBuffer[1] = NULL;
  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);
 
  image_view=AcquireVirtualCacheView(image,exception);
  inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  mem_flags = CL_MEM_READ_WRITE;
  length = image->columns * image->rows;
  for (k = 0; k < 2; k++)
  {
    tempImageBuffer[k] = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  filteredImage = CloneImage(image,image->columns,image->rows,MagickTrue,exception);
  assert(filteredImage != NULL);
  if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
    goto cleanup;
  }
  filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
  filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
  if (filteredPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
    goto cleanup;
  }

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
    hostPtr = filteredPixels;
  }
  else 
  {
    mem_flags = CL_MEM_WRITE_ONLY;
    hostPtr = NULL;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  hullPass1 = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "HullPass1");
  hullPass2 = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "HullPass2");

  clStatus =clEnv->library->clSetKernelArg(hullPass1,0,sizeof(cl_mem),(void *)&imageBuffer);
  clStatus |=clEnv->library->clSetKernelArg(hullPass1,1,sizeof(cl_mem),(void *)(tempImageBuffer+1));
  imageWidth = (unsigned int) image->columns;
  clStatus |=clEnv->library->clSetKernelArg(hullPass1,2,sizeof(unsigned int),(void *)&imageWidth);
  imageHeight = (unsigned int) image->rows;
  clStatus |=clEnv->library->clSetKernelArg(hullPass1,3,sizeof(unsigned int),(void *)&imageHeight);
  matte = (image->alpha_trait > CopyPixelTrait)?1:0;
  clStatus |=clEnv->library->clSetKernelArg(hullPass1,6,sizeof(int),(void *)&matte);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }

  clStatus = clEnv->library->clSetKernelArg(hullPass2,0,sizeof(cl_mem),(void *)(tempImageBuffer+1));
  clStatus |=clEnv->library->clSetKernelArg(hullPass2,1,sizeof(cl_mem),(void *)tempImageBuffer);
  imageWidth = (unsigned int) image->columns;
  clStatus |=clEnv->library->clSetKernelArg(hullPass2,2,sizeof(unsigned int),(void *)&imageWidth);
  imageHeight = (unsigned int) image->rows;
  clStatus |=clEnv->library->clSetKernelArg(hullPass2,3,sizeof(unsigned int),(void *)&imageHeight);
  matte = (image->alpha_trait > CopyPixelTrait)?1:0;
  clStatus |=clEnv->library->clSetKernelArg(hullPass2,6,sizeof(int),(void *)&matte);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }


  global_work_size[0] = image->columns;
  global_work_size[1] = image->rows;

  
  for (k = 0; k < 4; k++)
  {
    cl_int2 offset;
    int polarity;

    
    offset.s[0] = X[k];
    offset.s[1] = Y[k];
    polarity = 1;
    clStatus = clEnv->library->clSetKernelArg(hullPass1,4,sizeof(cl_int2),(void *)&offset);
    clStatus|= clEnv->library->clSetKernelArg(hullPass1,5,sizeof(int),(void *)&polarity);
    clStatus|=clEnv->library->clSetKernelArg(hullPass2,4,sizeof(cl_int2),(void *)&offset);
    clStatus|=clEnv->library->clSetKernelArg(hullPass2,5,sizeof(int),(void *)&polarity);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
      goto cleanup;
    }
    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, hullPass1, 2, NULL, global_work_size, NULL, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    RecordProfileData(clEnv,HullPass1Kernel,event);
    clEnv->library->clReleaseEvent(event);

    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, hullPass2, 2, NULL, global_work_size, NULL, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    RecordProfileData(clEnv,HullPass2Kernel,event);
    clEnv->library->clReleaseEvent(event);

    if (k == 0)
      clStatus =clEnv->library->clSetKernelArg(hullPass1,0,sizeof(cl_mem),(void *)(tempImageBuffer));
    offset.s[0] = -X[k];
    offset.s[1] = -Y[k];
    polarity = 1;
    clStatus = clEnv->library->clSetKernelArg(hullPass1,4,sizeof(cl_int2),(void *)&offset);
    clStatus|= clEnv->library->clSetKernelArg(hullPass1,5,sizeof(int),(void *)&polarity);
    clStatus|=clEnv->library->clSetKernelArg(hullPass2,4,sizeof(cl_int2),(void *)&offset);
    clStatus|=clEnv->library->clSetKernelArg(hullPass2,5,sizeof(int),(void *)&polarity);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
      goto cleanup;
    }
    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, hullPass1, 2, NULL, global_work_size, NULL, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    RecordProfileData(clEnv,HullPass1Kernel,event);
    clEnv->library->clReleaseEvent(event);

    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, hullPass2, 2, NULL, global_work_size, NULL, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    RecordProfileData(clEnv,HullPass2Kernel,event);
    clEnv->library->clReleaseEvent(event);

    offset.s[0] = -X[k];
    offset.s[1] = -Y[k];
    polarity = -1;
    clStatus = clEnv->library->clSetKernelArg(hullPass1,4,sizeof(cl_int2),(void *)&offset);
    clStatus|= clEnv->library->clSetKernelArg(hullPass1,5,sizeof(int),(void *)&polarity);
    clStatus|=clEnv->library->clSetKernelArg(hullPass2,4,sizeof(cl_int2),(void *)&offset);
    clStatus|=clEnv->library->clSetKernelArg(hullPass2,5,sizeof(int),(void *)&polarity);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
      goto cleanup;
    }
    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, hullPass1, 2, NULL, global_work_size, NULL, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    RecordProfileData(clEnv,HullPass1Kernel,event);
    clEnv->library->clReleaseEvent(event);

    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, hullPass2, 2, NULL, global_work_size, NULL, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    RecordProfileData(clEnv,HullPass2Kernel,event);
    clEnv->library->clReleaseEvent(event);

    offset.s[0] = X[k];
    offset.s[1] = Y[k];
    polarity = -1;
    clStatus = clEnv->library->clSetKernelArg(hullPass1,4,sizeof(cl_int2),(void *)&offset);
    clStatus|= clEnv->library->clSetKernelArg(hullPass1,5,sizeof(int),(void *)&polarity);
    clStatus|=clEnv->library->clSetKernelArg(hullPass2,4,sizeof(cl_int2),(void *)&offset);
    clStatus|=clEnv->library->clSetKernelArg(hullPass2,5,sizeof(int),(void *)&polarity);

    if (k == 3)
      clStatus |=clEnv->library->clSetKernelArg(hullPass2,1,sizeof(cl_mem),(void *)&filteredImageBuffer);

    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
      goto cleanup;
    }
    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, hullPass1, 2, NULL, global_work_size, NULL, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    RecordProfileData(clEnv,HullPass1Kernel,event);
    clEnv->library->clReleaseEvent(event);

    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, hullPass2, 2, NULL, global_work_size, NULL, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    RecordProfileData(clEnv,HullPass2Kernel,event);
    clEnv->library->clReleaseEvent(event);
  }

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (queue != NULL)                          RelinquishOpenCLCommandQueue(clEnv, queue);
  if (imageBuffer!=NULL)		      clEnv->library->clReleaseMemObject(imageBuffer);
  for (k = 0; k < 2; k++)
  {
    if (tempImageBuffer[k]!=NULL)	      clEnv->library->clReleaseMemObject(tempImageBuffer[k]);
  }
  if (filteredImageBuffer!=NULL)	      clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (hullPass1!=NULL)			      RelinquishOpenCLKernel(clEnv, hullPass1);
  if (hullPass2!=NULL)			      RelinquishOpenCLKernel(clEnv, hullPass2);
  if (outputReady == MagickFalse && filteredImage != NULL)
    filteredImage=DestroyImage(filteredImage);
  return(filteredImage);
}

MagickExport Image *AccelerateDespeckleImage(const Image* image,
  ExceptionInfo* exception)
{
  Image
    *filteredImage;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return NULL;

  filteredImage=ComputeDespeckleImage(image,exception);
  return(filteredImage);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e E q u a l i z e I m a g e                           %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static MagickBooleanType ComputeEqualizeImage(Image *image,
  ExceptionInfo *exception)
{
#define EqualizeImageTag  "Equalize/Image"

  CacheView
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus;

  cl_mem_flags
    mem_flags;

  cl_mem
    equalizeMapBuffer,
    histogramBuffer,
    imageBuffer;

  cl_kernel
    equalizeKernel,
    histogramKernel;

  cl_event
    event;

  cl_uint4
    *histogram;

  FloatPixelPacket
    white,
    black,
    intensity,
    *map;

  MagickBooleanType
    outputReady,
    status;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  PixelPacket
    *equalize_map;

  register ssize_t
    i;

  size_t
    global_work_size[2];

  void
    *hostPtr,
    *inputPixels;

  map=NULL;
  histogram=NULL;
  equalize_map=NULL;
  inputPixels = NULL;
  imageBuffer = NULL;
  histogramBuffer = NULL;
  equalizeMapBuffer = NULL;
  histogramKernel = NULL; 
  equalizeKernel = NULL; 
  context = NULL;
  queue = NULL;
  outputReady = MagickFalse;

  assert(image != (Image *) NULL);
  assert(image->signature == MagickCoreSignature);
  if (image->debug != MagickFalse)
    (void) LogMagickEvent(TraceEvent,GetMagickModule(),"%s",image->filename);

  /*
   * initialize opencl env
   */
  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  /*
    Allocate and initialize histogram arrays.
  */
  histogram=(cl_uint4 *) AcquireQuantumMemory(MaxMap+1UL, sizeof(*histogram));
  if (histogram == (cl_uint4 *) NULL)
      ThrowBinaryException(ResourceLimitWarning,"MemoryAllocationFailed", image->filename);

  /* reset histogram */
  (void) ResetMagickMemory(histogram,0,(MaxMap+1)*sizeof(*histogram));

  /* Create and initialize OpenCL buffers. */
  /* inputPixels = AcquirePixelCachePixels(image, &length, exception); */
  /* assume this  will get a writable image */
  image_view=AcquireAuthenticCacheView(image,exception);
  inputPixels=GetCacheViewAuthenticPixels(image_view,0,0,image->columns,image->rows,exception);

  if (inputPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }
  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of cl_uint, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(histogram,cl_uint4)) 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_USE_HOST_PTR;
    hostPtr = histogram;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
    hostPtr = histogram;
  }
  /* create a CL buffer for histogram  */
  length = (MaxMap+1); 
  histogramBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(cl_uint4), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  status = LaunchHistogramKernel(clEnv, queue, imageBuffer, histogramBuffer, image, image->channel_mask, exception);
  if (status == MagickFalse)
    goto cleanup;

  /* read from the kenel output */
  if (ALIGNED(histogram,cl_uint4)) 
  {
    length = (MaxMap+1); 
    clEnv->library->clEnqueueMapBuffer(queue, histogramBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(cl_uint4), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = (MaxMap+1); 
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, histogramBuffer, CL_TRUE, 0, length * sizeof(cl_uint4), histogram, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  /* unmap, don't block gpu to use this buffer again.  */
  if (ALIGNED(histogram,cl_uint4))
  {
    clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, histogramBuffer, histogram, 0, NULL, NULL);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
      goto cleanup;
    }
  }

  /* recreate input buffer later, in case image updated */
#ifdef RECREATEBUFFER 
  if (imageBuffer!=NULL)		      
    clEnv->library->clReleaseMemObject(imageBuffer);
#endif
 
  /* CPU stuff */
  equalize_map=(PixelPacket *) AcquireQuantumMemory(MaxMap+1UL, sizeof(*equalize_map));
  if (equalize_map == (PixelPacket *) NULL)
    ThrowBinaryException(ResourceLimitWarning,"MemoryAllocationFailed", image->filename);

  map=(FloatPixelPacket *) AcquireQuantumMemory(MaxMap+1UL,sizeof(*map));
  if (map == (FloatPixelPacket *) NULL)
    ThrowBinaryException(ResourceLimitWarning,"MemoryAllocationFailed", image->filename);

  /*
    Integrate the histogram to get the equalization map.
  */
  (void) ResetMagickMemory(&intensity,0,sizeof(intensity));
  for (i=0; i <= (ssize_t) MaxMap; i++)
  {
    if ((image->channel_mask & SyncChannels) != 0)
    {
      intensity.red+=histogram[i].s[2];
      map[i]=intensity;
      continue;
    }
    if ((image->channel_mask & RedChannel) != 0)
      intensity.red+=histogram[i].s[2];
    if ((image->channel_mask & GreenChannel) != 0)
      intensity.green+=histogram[i].s[1];
    if ((image->channel_mask & BlueChannel) != 0)
      intensity.blue+=histogram[i].s[0];
    if ((image->channel_mask & AlphaChannel) != 0)
      intensity.alpha+=histogram[i].s[3];
    /*
    if (((channel & IndexChannel) != 0) &&
        (image->colorspace == CMYKColorspace))
    {
      intensity.index+=histogram[i].index; 
    }
    */
    map[i]=intensity;
  }
  black=map[0];
  white=map[(int) MaxMap];
  (void) ResetMagickMemory(equalize_map,0,(MaxMap+1)*sizeof(*equalize_map));
  for (i=0; i <= (ssize_t) MaxMap; i++)
  {
    if ((image->channel_mask & SyncChannels) != 0)
    {
      if (white.red != black.red)
        equalize_map[i].red=ScaleMapToQuantum((MagickRealType) ((MaxMap*
                (map[i].red-black.red))/(white.red-black.red)));
      continue;
    }
    if (((image->channel_mask & RedChannel) != 0) && (white.red != black.red))
      equalize_map[i].red=ScaleMapToQuantum((MagickRealType) ((MaxMap*
              (map[i].red-black.red))/(white.red-black.red)));
    if (((image->channel_mask & GreenChannel) != 0) && (white.green != black.green))
      equalize_map[i].green=ScaleMapToQuantum((MagickRealType) ((MaxMap*
              (map[i].green-black.green))/(white.green-black.green)));
    if (((image->channel_mask & BlueChannel) != 0) && (white.blue != black.blue))
      equalize_map[i].blue=ScaleMapToQuantum((MagickRealType) ((MaxMap*
              (map[i].blue-black.blue))/(white.blue-black.blue)));
    if (((image->channel_mask & AlphaChannel) != 0) && (white.alpha != black.alpha))
      equalize_map[i].alpha=ScaleMapToQuantum((MagickRealType) ((MaxMap*
              (map[i].alpha-black.alpha))/(white.alpha-black.alpha)));
    /*
    if ((((channel & IndexChannel) != 0) &&
          (image->colorspace == CMYKColorspace)) &&
        (white.index != black.index))
      equalize_map[i].index=ScaleMapToQuantum((MagickRealType) ((MaxMap*
              (map[i].index-black.index))/(white.index-black.index)));
    */
  }

  if (image->storage_class == PseudoClass)
  {
    /*
       Equalize colormap.
       */
    for (i=0; i < (ssize_t) image->colors; i++)
    {
      if ((image->channel_mask & SyncChannels) != 0)
      {
        if (white.red != black.red)
        {
          image->colormap[i].red=equalize_map[
            ScaleQuantumToMap(image->colormap[i].red)].red;
          image->colormap[i].green=equalize_map[
            ScaleQuantumToMap(image->colormap[i].green)].red;
          image->colormap[i].blue=equalize_map[
            ScaleQuantumToMap(image->colormap[i].blue)].red;
          image->colormap[i].alpha=equalize_map[
            ScaleQuantumToMap(image->colormap[i].alpha)].red;
        }
        continue;
      }
      if (((image->channel_mask & RedChannel) != 0) && (white.red != black.red))
        image->colormap[i].red=equalize_map[
          ScaleQuantumToMap(image->colormap[i].red)].red;
      if (((image->channel_mask & GreenChannel) != 0) && (white.green != black.green))
        image->colormap[i].green=equalize_map[
          ScaleQuantumToMap(image->colormap[i].green)].green;
      if (((image->channel_mask & BlueChannel) != 0) && (white.blue != black.blue))
        image->colormap[i].blue=equalize_map[
          ScaleQuantumToMap(image->colormap[i].blue)].blue;
      if (((image->channel_mask & AlphaChannel) != 0) &&
          (white.alpha != black.alpha))
        image->colormap[i].alpha=equalize_map[
          ScaleQuantumToMap(image->colormap[i].alpha)].alpha;
    }
  }

  /*
    Equalize image.
  */

  /* GPU can work on this again, image and equalize map as input
    image:        uchar4 (CLPixelPacket)
    equalize_map: uchar4 (PixelPacket)
    black, white: float4 (FloatPixelPacket) */

#ifdef RECREATEBUFFER 
  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }
#endif

  /* Create and initialize OpenCL buffers. */
  if (ALIGNED(equalize_map, PixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
    hostPtr = equalize_map;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
    hostPtr = equalize_map;
  }
  /* create a CL buffer for eqaulize_map  */
  length = (MaxMap+1); 
  equalizeMapBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(PixelPacket), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  /* get the OpenCL kernel */
  equalizeKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "Equalize");
  if (equalizeKernel == NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }

  /* set the kernel arguments */
  i = 0;
  clStatus=clEnv->library->clSetKernelArg(equalizeKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
  clStatus|=clEnv->library->clSetKernelArg(equalizeKernel,i++,sizeof(ChannelType),&image->channel_mask);
  clStatus|=clEnv->library->clSetKernelArg(equalizeKernel,i++,sizeof(cl_mem),(void *)&equalizeMapBuffer);
  clStatus|=clEnv->library->clSetKernelArg(equalizeKernel,i++,sizeof(FloatPixelPacket),&white);
  clStatus|=clEnv->library->clSetKernelArg(equalizeKernel,i++,sizeof(FloatPixelPacket),&black);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }

  /* launch the kernel */
  global_work_size[0] = image->columns;
  global_work_size[1] = image->rows;

  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, equalizeKernel, 2, NULL, global_work_size, NULL, 0, NULL, &event);

  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);
  RecordProfileData(clEnv,EqualizeKernel,event);
  clEnv->library->clReleaseEvent(event);

  /* read the data back */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, imageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, imageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), inputPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(image_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);

  if (imageBuffer!=NULL)		      
    clEnv->library->clReleaseMemObject(imageBuffer);

  if (map!=NULL)
    map=(FloatPixelPacket *) RelinquishMagickMemory(map);

  if (equalizeMapBuffer!=NULL)
    clEnv->library->clReleaseMemObject(equalizeMapBuffer);
  if (equalize_map!=NULL)
    equalize_map=(PixelPacket *) RelinquishMagickMemory(equalize_map);

  if (histogramBuffer!=NULL)		      
    clEnv->library->clReleaseMemObject(histogramBuffer);
  if (histogram!=NULL)
    histogram=(cl_uint4 *) RelinquishMagickMemory(histogram);

  if (histogramKernel!=NULL)                     
    RelinquishOpenCLKernel(clEnv, histogramKernel);
  if (equalizeKernel!=NULL)                     
    RelinquishOpenCLKernel(clEnv, equalizeKernel);

  if (queue != NULL)                          
    RelinquishOpenCLCommandQueue(clEnv, queue);

  return(outputReady);
}

MagickExport MagickBooleanType AccelerateEqualizeImage(Image *image,
  ExceptionInfo *exception)
{
  MagickBooleanType
    status;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkHistogramCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return(MagickFalse);

  status=ComputeEqualizeImage(image,exception);
  return(status);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e F u n c t i o n I m a g e                           %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static MagickBooleanType ComputeFunctionImage(Image *image,
  const MagickFunction function,const size_t number_parameters,
  const double *parameters,ExceptionInfo *exception)
{
  CacheView
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus;

  cl_kernel
    clkernel;

  cl_event
    event;

  cl_mem
    imageBuffer,
    parametersBuffer;

  cl_mem_flags
    mem_flags;

  float
    *parametersBufferPtr;

  MagickBooleanType
    status;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  size_t
    globalWorkSize[2];

  unsigned int
    i;

  void
    *pixels;

  status = MagickFalse;

  context = NULL;
  clkernel = NULL;
  queue = NULL;
  imageBuffer = NULL;
  parametersBuffer = NULL;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);

  image_view=AcquireAuthenticCacheView(image,exception);
  pixels=GetCacheViewAuthenticPixels(image_view,0,0,image->columns,image->rows,exception);
  if (pixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), CacheWarning,
      "GetPixelCachePixels failed.",
      "'%s'", image->filename);
    goto cleanup;
  }


  if (ALIGNED(pixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)pixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  parametersBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR, number_parameters * sizeof(float), NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  queue = AcquireOpenCLCommandQueue(clEnv);

  parametersBufferPtr = (float*)clEnv->library->clEnqueueMapBuffer(queue, parametersBuffer, CL_TRUE, CL_MAP_WRITE, 0, number_parameters * sizeof(float)
                , 0, NULL, NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueMapBuffer failed.",".");
    goto cleanup;
  }
  for (i = 0; i < number_parameters; i++)
  {
    parametersBufferPtr[i] = (float)parameters[i];
  }
  clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, parametersBuffer, parametersBufferPtr, 0, NULL, NULL);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);

  clkernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "ComputeFunction");
  if (clkernel == NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }

  /* set the kernel arguments */
  i = 0;
  clStatus =clEnv->library->clSetKernelArg(clkernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
  clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(ChannelType),(void *)&image->channel_mask);
  clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(MagickFunction),(void *)&function);
  clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(unsigned int),(void *)&number_parameters);
  clStatus|=clEnv->library->clSetKernelArg(clkernel,i++,sizeof(cl_mem),(void *)&parametersBuffer);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }

  globalWorkSize[0] = image->columns;
  globalWorkSize[1] = image->rows;
  /* launch the kernel */
  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, clkernel, 2, NULL, globalWorkSize, NULL, 0, NULL, &event);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);
  RecordProfileData(clEnv,ComputeFunctionKernel,event);
  clEnv->library->clReleaseEvent(event);

  if (ALIGNED(pixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, imageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, imageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), pixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }
  status=SyncCacheViewAuthenticPixels(image_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  
  if (clkernel != NULL) RelinquishOpenCLKernel(clEnv, clkernel);
  if (queue != NULL) RelinquishOpenCLCommandQueue(clEnv, queue);
  if (imageBuffer != NULL) clEnv->library->clReleaseMemObject(imageBuffer);
  if (parametersBuffer != NULL) clEnv->library->clReleaseMemObject(parametersBuffer);

  return(status);
}

MagickExport MagickBooleanType AccelerateFunctionImage(Image *image,
  const MagickFunction function,const size_t number_parameters,
  const double *parameters,ExceptionInfo *exception)
{
  MagickBooleanType
    status;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return(MagickFalse);

  status=ComputeFunctionImage(image,function,number_parameters,parameters,
    exception);
  return(status);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e G r a y s c a l e I m a g e                         %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static MagickBooleanType ComputeGrayscaleImage(Image *image,
  const PixelIntensityMethod method,ExceptionInfo *exception)
{
  CacheView
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus,
    intensityMethod;

  cl_int
    colorspace;

  cl_kernel
    grayscaleKernel;

  cl_event
    event;

  cl_mem
    imageBuffer;

  cl_mem_flags
    mem_flags;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  register ssize_t
    i;

  void
    *inputPixels;

  inputPixels = NULL;
  imageBuffer = NULL;
  grayscaleKernel = NULL; 

  assert(image != (Image *) NULL);
  assert(image->signature == MagickCoreSignature);
  if (image->debug != MagickFalse)
    (void) LogMagickEvent(TraceEvent,GetMagickModule(),"%s",image->filename);

  /*
   * initialize opencl env
   */
  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  outputReady = MagickFalse;

  /* Create and initialize OpenCL buffers.
   inputPixels = AcquirePixelCachePixels(image, &length, exception);
   assume this  will get a writable image
   */
  image_view=AcquireAuthenticCacheView(image,exception);
  inputPixels=GetCacheViewAuthenticPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of CLPixelPacket, 
   then use the host buffer directly from the GPU; otherwise, 
   create a buffer on the GPU and copy the data over
   */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  intensityMethod = method;
  colorspace = image->colorspace;

  grayscaleKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "Grayscale");
  if (grayscaleKernel == NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }

  i = 0;
  clStatus=clEnv->library->clSetKernelArg(grayscaleKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
  clStatus|=clEnv->library->clSetKernelArg(grayscaleKernel,i++,sizeof(cl_int),&intensityMethod);
  clStatus|=clEnv->library->clSetKernelArg(grayscaleKernel,i++,sizeof(cl_int),&colorspace);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    printf("no kernel\n");
    goto cleanup;
  }

  {
    size_t global_work_size[2];
    global_work_size[0] = image->columns;
    global_work_size[1] = image->rows;
    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, grayscaleKernel, 2, NULL, global_work_size, NULL, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    clEnv->library->clFlush(queue);
    RecordProfileData(clEnv,GrayScaleKernel,event);
    clEnv->library->clReleaseEvent(event);
  }

  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, imageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, imageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), inputPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(image_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);

  if (imageBuffer!=NULL)		      
    clEnv->library->clReleaseMemObject(imageBuffer);
  if (grayscaleKernel!=NULL)                     
    RelinquishOpenCLKernel(clEnv, grayscaleKernel);
  if (queue != NULL)                          
    RelinquishOpenCLCommandQueue(clEnv, queue);

  return( outputReady);
}

MagickExport MagickBooleanType AccelerateGrayscaleImage(Image* image,
  const PixelIntensityMethod method,ExceptionInfo *exception)
{
  MagickBooleanType
    status;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return(MagickFalse);

  if ((method == Rec601LuminancePixelIntensityMethod) ||
      (method == Rec709LuminancePixelIntensityMethod))
    return(MagickFalse);

  if (image->colorspace != sRGBColorspace)
    return(MagickFalse);

  status=ComputeGrayscaleImage(image,method,exception);
  return(status);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e L o c a l C o n t r a s t I m a g e                 %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static Image *ComputeLocalContrastImage(const Image *image,
  const double radius,const double strength,ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus,
    iRadius;

  cl_kernel
    blurRowKernel,
    blurColumnKernel;

  cl_event
    event;

  cl_mem
    filteredImageBuffer,
    imageBuffer,
    imageKernelBuffer,
    tempImageBuffer;

  cl_mem_flags
    mem_flags;

  const void
    *inputPixels;

  Image
    *filteredImage;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  void
    *filteredPixels,
    *hostPtr;

  unsigned int
    i,
    imageColumns,
    imageRows,
    passes;

  clEnv = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  context = NULL;
  imageBuffer = NULL;
  filteredImageBuffer = NULL;
  tempImageBuffer = NULL;
  imageKernelBuffer = NULL;
  blurRowKernel = NULL;
  blurColumnKernel = NULL;
  queue = NULL;
  outputReady = MagickFalse;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  /* Create and initialize OpenCL buffers. */
  {
    image_view=AcquireVirtualCacheView(image,exception);
    inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
    if (inputPixels == (const void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
      goto cleanup;
    }

    /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
    if (ALIGNED(inputPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
    }
    else 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
    }
    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create output */
  {
    filteredImage = CloneImage(image,image->columns,image->rows,MagickTrue,exception);
    assert(filteredImage != NULL);
    if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
      goto cleanup;
    }
    filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
    filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
    if (filteredPixels == (void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
      goto cleanup;
    }

    if (ALIGNED(filteredPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
      hostPtr = filteredPixels;
    }
    else 
    {
      mem_flags = CL_MEM_WRITE_ONLY;
      hostPtr = NULL;
    }

    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  {
    /* create temp buffer */
    {
      length = image->columns * image->rows;
      tempImageBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_WRITE, length * sizeof(float), NULL, &clStatus);
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
        goto cleanup;
      }
    }

    /* get the opencl kernel */
    {
      blurRowKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "LocalContrastBlurRow");
      if (blurRowKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };

      blurColumnKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "LocalContrastBlurApplyColumn");
      if (blurColumnKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };
    }

    {
      imageColumns = (unsigned int) image->columns;
      imageRows = (unsigned int) image->rows;
      iRadius = (cl_int) fabs(radius);

      passes = ((1.0f * imageColumns) * imageColumns * iRadius) / 4000000000.0f;
      passes = (passes < 1) ? 1: passes;

      /* set the kernel arguments */
      i = 0;
      clStatus=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&filteredImageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&tempImageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_int),(void *)&iRadius);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&imageRows);
      
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
        goto cleanup;
      }
    }

    /* launch the kernel */
    {
      int x;
      for (x = 0; x < passes; ++x) {
        size_t gsize[2];
        size_t wsize[2];
        size_t goffset[2];

        gsize[0] = 256;
        gsize[1] = image->rows / passes;
        wsize[0] = 256;
        wsize[1] = 1;
        goffset[0] = 0;
        goffset[1] = x * gsize[1];

        clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, blurRowKernel, 2, goffset, gsize, wsize, 0, NULL, &event);
        if (clStatus != CL_SUCCESS)
        {
          (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
          goto cleanup;
        }
        RecordProfileData(clEnv,LocalContrastBlurRowKernel,event);
        clEnv->library->clReleaseEvent(event);
      }
    }

    {
      cl_float FStrength = strength;
      i = 0;
      clStatus=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_mem),(void *)&filteredImageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_mem),(void *)&tempImageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&iRadius);
      clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(cl_float),(void *)&FStrength);
      clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
      clStatus|=clEnv->library->clSetKernelArg(blurColumnKernel,i++,sizeof(unsigned int),(void *)&imageRows);

      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
        goto cleanup;
      }
    }

    /* launch the kernel */
    {
      int x;
      for (x = 0; x < passes; ++x) {
        size_t gsize[2];
        size_t wsize[2];
        size_t goffset[2];

        gsize[0] = ((image->columns + 3) / 4) * 4;
        gsize[1] = ((((image->rows + 63) / 64) + (passes + 1)) / passes) * 64;
        wsize[0] = 4;
        wsize[1] = 64;
        goffset[0] = 0;
        goffset[1] = x * gsize[1];

        clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, blurColumnKernel, 2, goffset, gsize, wsize, 0, NULL, &event);
        if (clStatus != CL_SUCCESS)
        {
          (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
          goto cleanup;
        }
        RecordProfileData(clEnv,LocalContrastBlurApplyColumnKernel,event);
        clEnv->library->clReleaseEvent(event);
      }
    }
  }

  /* get result */
  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (imageBuffer!=NULL)                      clEnv->library->clReleaseMemObject(imageBuffer);
  if (filteredImageBuffer!=NULL)              clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (tempImageBuffer!=NULL)                  clEnv->library->clReleaseMemObject(tempImageBuffer);
  if (imageKernelBuffer!=NULL)                clEnv->library->clReleaseMemObject(imageKernelBuffer);
  if (blurRowKernel!=NULL)                    RelinquishOpenCLKernel(clEnv, blurRowKernel);
  if (blurColumnKernel!=NULL)                 RelinquishOpenCLKernel(clEnv, blurColumnKernel);
  if (queue != NULL)                          RelinquishOpenCLCommandQueue(clEnv, queue);
  if (outputReady == MagickFalse)
  {
    if (filteredImage != NULL)
    {
      DestroyImage(filteredImage);
      filteredImage = NULL;
    }
  }
  return(filteredImage);
}

MagickExport Image *AccelerateLocalContrastImage(const Image *image,
  const double radius,const double strength,ExceptionInfo *exception)
{
  Image
    *filteredImage;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return NULL;

  filteredImage=ComputeLocalContrastImage(image,radius,strength,exception);
  return(filteredImage);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e M o d u l a t e I m a g e                           %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static MagickBooleanType ComputeModulateImage(Image *image,
  const double percent_brightness,const double percent_hue,
  const double percent_saturation,const ColorspaceType colorspace,
  ExceptionInfo *exception)
{
  CacheView
    *image_view;

  cl_float
    bright,
    hue,
    saturation;

  cl_context
    context;

  cl_command_queue
    queue;

  cl_int
    color,
    clStatus;

  cl_kernel
    modulateKernel;

  cl_event
    event;

  cl_mem
    imageBuffer;

  cl_mem_flags
    mem_flags;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  register ssize_t
    i;

  void
    *inputPixels;

  inputPixels = NULL;
  imageBuffer = NULL;
  modulateKernel = NULL; 

  assert(image != (Image *) NULL);
  assert(image->signature == MagickCoreSignature);
  if (image->debug != MagickFalse)
    (void) LogMagickEvent(TraceEvent,GetMagickModule(),"%s",image->filename);

  /*
   * initialize opencl env
   */
  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  outputReady = MagickFalse;

  /* Create and initialize OpenCL buffers.
   inputPixels = AcquirePixelCachePixels(image, &length, exception);
   assume this  will get a writable image
   */
  image_view=AcquireAuthenticCacheView(image,exception);
  inputPixels=GetCacheViewAuthenticPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of CLPixelPacket, 
   then use the host buffer directly from the GPU; otherwise, 
   create a buffer on the GPU and copy the data over
   */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  modulateKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "Modulate");
  if (modulateKernel == NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }

  bright=percent_brightness;
  hue=percent_hue;
  saturation=percent_saturation;
  color=colorspace;

  i = 0;
  clStatus=clEnv->library->clSetKernelArg(modulateKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
  clStatus|=clEnv->library->clSetKernelArg(modulateKernel,i++,sizeof(cl_float),&bright);
  clStatus|=clEnv->library->clSetKernelArg(modulateKernel,i++,sizeof(cl_float),&hue);
  clStatus|=clEnv->library->clSetKernelArg(modulateKernel,i++,sizeof(cl_float),&saturation);
  clStatus|=clEnv->library->clSetKernelArg(modulateKernel,i++,sizeof(cl_float),&color);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    printf("no kernel\n");
    goto cleanup;
  }

  {
    size_t global_work_size[2];
    global_work_size[0] = image->columns;
    global_work_size[1] = image->rows;
    /* launch the kernel */
	clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, modulateKernel, 2, NULL, global_work_size, NULL, 0, NULL, &event);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
      goto cleanup;
    }
    clEnv->library->clFlush(queue);
    RecordProfileData(clEnv,ModulateKernel,event);
    clEnv->library->clReleaseEvent(event);
  }

  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, imageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, imageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), inputPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(image_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);

  if (imageBuffer!=NULL)		      
    clEnv->library->clReleaseMemObject(imageBuffer);
  if (modulateKernel!=NULL)                     
    RelinquishOpenCLKernel(clEnv, modulateKernel);
  if (queue != NULL)                          
    RelinquishOpenCLCommandQueue(clEnv, queue);

  return outputReady;

}

MagickExport MagickBooleanType AccelerateModulateImage(Image *image,
  const double percent_brightness,const double percent_hue,
  const double percent_saturation,const ColorspaceType colorspace,
  ExceptionInfo *exception)
{
  MagickBooleanType
    status;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return(MagickFalse);

  if ((colorspace != HSLColorspace) && (colorspace != UndefinedColorspace))
    return(MagickFalse);

  status=ComputeModulateImage(image,percent_brightness,percent_hue,
    percent_saturation,colorspace,exception);
  return(status);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e M o t i o n B l u r I m a g e                       %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static Image* ComputeMotionBlurImage(const Image *image,const double *kernel,
  const size_t width,const OffsetInfo *offset,ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_float4
    biasPixel;

  cl_int
    clStatus;

  cl_kernel
    motionBlurKernel;

  cl_event
    event;

  cl_mem
    filteredImageBuffer,
    imageBuffer,
    imageKernelBuffer, 
    offsetBuffer;

  cl_mem_flags
    mem_flags;

  const void
    *inputPixels;

  float
    *kernelBufferPtr;

  Image
    *filteredImage;

  int
    *offsetBufferPtr;

  MagickBooleanType
    outputReady;

  MagickCLEnv
   clEnv;

  PixelInfo
    bias;

  MagickSizeType
    length;

  size_t
    global_work_size[2],
    local_work_size[2];

  unsigned int
    i,
    imageHeight,
    imageWidth,
    matte;

  void
    *filteredPixels,
    *hostPtr;

  outputReady = MagickFalse;
  context = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  imageBuffer = NULL;
  filteredImageBuffer = NULL;
  imageKernelBuffer = NULL;
  motionBlurKernel = NULL;
  queue = NULL;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);

  /* Create and initialize OpenCL buffers. */

  image_view=AcquireVirtualCacheView(image,exception);
  inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (const void *) NULL)
  {
    (void) ThrowMagickException(exception,GetMagickModule(),CacheError,
      "UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  // If the host pointer is aligned to the size of CLPixelPacket, 
  // then use the host buffer directly from the GPU; otherwise, 
  // create a buffer on the GPU and copy the data over
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
  }
  // create a CL buffer from image pixel buffer
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, 
    length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(),
      ResourceLimitError, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }


  filteredImage = CloneImage(image,image->columns,image->rows,
    MagickTrue,exception);
  assert(filteredImage != NULL);
  if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), 
      ResourceLimitError, "CloneImage failed.", "'%s'", ".");
    goto cleanup;
  }
  filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
  filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
  if (filteredPixels == (void *) NULL)
  {
    (void) ThrowMagickException(exception,GetMagickModule(),CacheError, 
      "UnableToReadPixelCache.","`%s'",filteredImage->filename);
    goto cleanup;
  }

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
    hostPtr = filteredPixels;
  }
  else 
  {
    mem_flags = CL_MEM_WRITE_ONLY;
    hostPtr = NULL;
  }
  // create a CL buffer from image pixel buffer
  length = image->columns * image->rows;
  filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, 
    length * sizeof(CLPixelPacket), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), 
      ResourceLimitError, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }


  imageKernelBuffer = clEnv->library->clCreateBuffer(context, 
    CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR, width * sizeof(float), NULL,
    &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), 
      ResourceLimitError, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  queue = AcquireOpenCLCommandQueue(clEnv);
  kernelBufferPtr = (float*)clEnv->library->clEnqueueMapBuffer(queue, imageKernelBuffer, 
    CL_TRUE, CL_MAP_WRITE, 0, width * sizeof(float), 0, NULL, NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), 
      ResourceLimitError, "clEnv->library->clEnqueueMapBuffer failed.",".");
    goto cleanup;
  }
  for (i = 0; i < width; i++)
  {
    kernelBufferPtr[i] = (float) kernel[i];
  }
  clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, imageKernelBuffer, kernelBufferPtr,
    0, NULL, NULL);
 if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleFatalError, 
      "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
    goto cleanup;
  }

  offsetBuffer = clEnv->library->clCreateBuffer(context, 
    CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR, width * sizeof(cl_int2), NULL,
    &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), 
      ResourceLimitError, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  offsetBufferPtr = (int*)clEnv->library->clEnqueueMapBuffer(queue, offsetBuffer, CL_TRUE, 
    CL_MAP_WRITE, 0, width * sizeof(cl_int2), 0, NULL, NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), 
      ResourceLimitError, "clEnv->library->clEnqueueMapBuffer failed.",".");
    goto cleanup;
  }
  for (i = 0; i < width; i++)
  {
    offsetBufferPtr[2*i] = (int)offset[i].x;
    offsetBufferPtr[2*i+1] = (int)offset[i].y;
  }
  clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, offsetBuffer, offsetBufferPtr, 0, 
    NULL, NULL);
 if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleFatalError,
      "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
    goto cleanup;
  }


 // get the OpenCL kernel
  motionBlurKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, 
    "MotionBlur");
  if (motionBlurKernel == NULL)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleFatalError,
      "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }
  
  // set the kernel arguments
  i = 0;
  clStatus=clEnv->library->clSetKernelArg(motionBlurKernel,i++,sizeof(cl_mem),
    (void *)&imageBuffer);
  clStatus|=clEnv->library->clSetKernelArg(motionBlurKernel,i++,sizeof(cl_mem),
    (void *)&filteredImageBuffer);
  imageWidth = (unsigned int) image->columns;
  imageHeight = (unsigned int) image->rows;
  clStatus|=clEnv->library->clSetKernelArg(motionBlurKernel,i++,sizeof(unsigned int),
    &imageWidth);
  clStatus|=clEnv->library->clSetKernelArg(motionBlurKernel,i++,sizeof(unsigned int),
    &imageHeight);
  clStatus|=clEnv->library->clSetKernelArg(motionBlurKernel,i++,sizeof(cl_mem),
    (void *)&imageKernelBuffer);
  clStatus|=clEnv->library->clSetKernelArg(motionBlurKernel,i++,sizeof(unsigned int),
    &width);
  clStatus|=clEnv->library->clSetKernelArg(motionBlurKernel,i++,sizeof(cl_mem),
    (void *)&offsetBuffer);

  GetPixelInfo(image,&bias);
  biasPixel.s[0] = bias.red;
  biasPixel.s[1] = bias.green;
  biasPixel.s[2] = bias.blue;
  biasPixel.s[3] = bias.alpha;
  clStatus|=clEnv->library->clSetKernelArg(motionBlurKernel,i++,sizeof(cl_float4), &biasPixel);

  clStatus|=clEnv->library->clSetKernelArg(motionBlurKernel,i++,sizeof(ChannelType), &image->channel_mask);
  matte = (image->alpha_trait > CopyPixelTrait)?1:0;
  clStatus|=clEnv->library->clSetKernelArg(motionBlurKernel,i++,sizeof(unsigned int), &matte);
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleFatalError,
      "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }

  // launch the kernel
  local_work_size[0] = 16;
  local_work_size[1] = 16;
  global_work_size[0] = (size_t)padGlobalWorkgroupSizeToLocalWorkgroupSize(
                                (unsigned int) image->columns,(unsigned int) local_work_size[0]);
  global_work_size[1] = (size_t)padGlobalWorkgroupSizeToLocalWorkgroupSize(
                                (unsigned int) image->rows,(unsigned int) local_work_size[1]);
  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, motionBlurKernel, 2, NULL, 
	  global_work_size, local_work_size, 0, NULL, &event);

  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleFatalError,
      "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);
  RecordProfileData(clEnv,MotionBlurKernel,event);
  clEnv->library->clReleaseEvent(event);

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, 
      CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, 
      NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, 
      length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) ThrowMagickException(exception, GetMagickModule(), ModuleFatalError,
      "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }
  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (filteredImageBuffer!=NULL)  clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (imageBuffer!=NULL)     clEnv->library->clReleaseMemObject(imageBuffer);
  if (imageKernelBuffer!=NULL)    clEnv->library->clReleaseMemObject(imageKernelBuffer);
  if (motionBlurKernel!=NULL)  RelinquishOpenCLKernel(clEnv, motionBlurKernel);
  if (queue != NULL)           RelinquishOpenCLCommandQueue(clEnv, queue);
  if (outputReady == MagickFalse && filteredImage != NULL)
    filteredImage=DestroyImage(filteredImage);

  return(filteredImage);
}

MagickExport Image *AccelerateMotionBlurImage(const Image *image,
  const double* kernel,const size_t width,const OffsetInfo *offset,
  ExceptionInfo *exception)
{
  Image
    *filteredImage;

  assert(image != NULL);
  assert(kernel != (double *) NULL);
  assert(offset != (OffsetInfo *) NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return NULL;

  filteredImage=ComputeMotionBlurImage(image,kernel,width,offset,exception);
  return(filteredImage);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e R a n d o m I m a g e                               %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static MagickBooleanType LaunchRandomImageKernel(MagickCLEnv clEnv,
  cl_command_queue queue,cl_mem imageBuffer,const unsigned int imageColumns,
  const unsigned int imageRows,cl_mem seedBuffer,
  const unsigned int numGenerators,ExceptionInfo *exception)
{
  int
    k;

  cl_int
    clStatus;

  cl_kernel
    randomImageKernel;

  cl_event
    event;

  MagickBooleanType
    status;

  size_t
    global_work_size,
    local_work_size;

  status = MagickFalse;
  randomImageKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "RandomNumberGenerator");

  k = 0;
  clEnv->library->clSetKernelArg(randomImageKernel,k++,sizeof(cl_mem),(void*)&imageBuffer);
  clEnv->library->clSetKernelArg(randomImageKernel,k++,sizeof(cl_uint),(void*)&imageColumns);
  clEnv->library->clSetKernelArg(randomImageKernel,k++,sizeof(cl_uint),(void*)&imageRows);
  clEnv->library->clSetKernelArg(randomImageKernel,k++,sizeof(cl_mem),(void*)&seedBuffer);
  {
    const float randNormNumerator = 1.0f;
    const unsigned int randNormDenominator = (unsigned int)(~0UL);
    clEnv->library->clSetKernelArg(randomImageKernel,k++,
          sizeof(float),(void*)&randNormNumerator);
    clEnv->library->clSetKernelArg(randomImageKernel,k++,
          sizeof(cl_uint),(void*)&randNormDenominator);
  }


  global_work_size = numGenerators;
  local_work_size = 64;

  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue,randomImageKernel,1,NULL,&global_work_size,
	  &local_work_size, 0, NULL, &event);

  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, 
                                      "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }
  RecordProfileData(clEnv,RandomNumberGeneratorKernel,event);
  clEnv->library->clReleaseEvent(event);

  status = MagickTrue;

cleanup:
  if (randomImageKernel!=NULL) RelinquishOpenCLKernel(clEnv, randomImageKernel);
  return(status);
}

static MagickBooleanType ComputeRandomImage(Image* image,
  ExceptionInfo* exception)
{
  CacheView
    *image_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus;

  /* Don't release this buffer in this function !!! */
  cl_mem
    randomNumberSeedsBuffer;

  cl_mem_flags
    mem_flags;

  cl_mem 
   imageBuffer;

  MagickBooleanType 
    outputReady,
    status;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  void
    *inputPixels;

  status = MagickFalse;
  outputReady = MagickFalse;
  inputPixels = NULL;
  context = NULL;
  imageBuffer = NULL;
  queue = NULL;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);

  /* Create and initialize OpenCL buffers. */
  image_view=AcquireAuthenticCacheView(image,exception);
  inputPixels=GetCacheViewAuthenticPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_WRITE|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }
 
  queue = AcquireOpenCLCommandQueue(clEnv);

  randomNumberSeedsBuffer = GetAndLockRandSeedBuffer(clEnv);
  if (randomNumberSeedsBuffer==NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), 
           ResourceLimitWarning, "Failed to get GPU random number generators.",
           "'%s'", ".");
    goto cleanup;
  }

  status = LaunchRandomImageKernel(clEnv,queue,
                                   imageBuffer,
                                   (unsigned int) image->columns,
                                   (unsigned int) image->rows,
                                   randomNumberSeedsBuffer,
                                   GetNumRandGenerators(clEnv),
                                   exception);
  if (status==MagickFalse)
  {
    goto cleanup;
  }

  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, imageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, imageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), inputPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }
  outputReady=SyncCacheViewAuthenticPixels(image_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);

  UnlockRandSeedBuffer(clEnv);
  if (imageBuffer!=NULL)		      clEnv->library->clReleaseMemObject(imageBuffer);
  if (queue != NULL)                  RelinquishOpenCLCommandQueue(clEnv, queue);
  return outputReady;
}

MagickExport MagickBooleanType AccelerateRandomImage(Image *image,
  ExceptionInfo* exception)
{
  MagickBooleanType
    status;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return(MagickFalse);

  status=ComputeRandomImage(image,exception);
  return(status);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e R e s i z e I m a g e                               %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static MagickBooleanType resizeHorizontalFilter(cl_mem image,
  const unsigned int imageColumns,const unsigned int imageRows,
  const unsigned int matte,cl_mem resizedImage,
  const unsigned int resizedColumns,const unsigned int resizedRows,
  const ResizeFilter *resizeFilter,cl_mem resizeFilterCubicCoefficients,
  const float xFactor,MagickCLEnv clEnv,cl_command_queue queue,
  ExceptionInfo *exception)
{
  cl_kernel
    horizontalKernel;

  cl_event
    event;

  cl_int clStatus;

  const unsigned int
    workgroupSize = 256;

  float
    resizeFilterScale,
    resizeFilterSupport,
    resizeFilterWindowSupport,
    resizeFilterBlur,
    scale,
    support;

  int
    cacheRangeStart,
    cacheRangeEnd,
    numCachedPixels,
    resizeFilterType,
    resizeWindowType;

  MagickBooleanType
    status = MagickFalse;

  size_t
    deviceLocalMemorySize,
    gammaAccumulatorLocalMemorySize,
    global_work_size[2],
    imageCacheLocalMemorySize,
    pixelAccumulatorLocalMemorySize,
    local_work_size[2],
    totalLocalMemorySize,
    weightAccumulatorLocalMemorySize;

  unsigned int
    chunkSize,
    i,
    pixelPerWorkgroup;

  horizontalKernel = NULL;
  status = MagickFalse;

  /*
  Apply filter to resize vertically from image to resize image.
  */
  scale=MAGICK_MAX(1.0/xFactor+MagickEpsilon,1.0);
  support=scale*GetResizeFilterSupport(resizeFilter);
  if (support < 0.5)
  {
    /*
    Support too small even for nearest neighbour: Reduce to point
    sampling.
    */
    support=(MagickRealType) 0.5;
    scale=1.0;
  }
  scale=PerceptibleReciprocal(scale);

  if (resizedColumns < workgroupSize) 
  {
    chunkSize = 32;
    pixelPerWorkgroup = 32;
  }
  else
  {
    chunkSize = workgroupSize;
    pixelPerWorkgroup = workgroupSize;
  }

  /* get the local memory size supported by the device */
  deviceLocalMemorySize = GetOpenCLDeviceLocalMemorySize(clEnv);

DisableMSCWarning(4127)
  while(1)
RestoreMSCWarning
  {
    /* calculate the local memory size needed per workgroup */
    cacheRangeStart = (int) (((0 + 0.5)/xFactor+MagickEpsilon)-support+0.5);
    cacheRangeEnd = (int) ((((pixelPerWorkgroup-1) + 0.5)/xFactor+MagickEpsilon)+support+0.5);
    numCachedPixels = cacheRangeEnd - cacheRangeStart + 1;
    imageCacheLocalMemorySize = numCachedPixels * sizeof(CLPixelPacket);
    totalLocalMemorySize = imageCacheLocalMemorySize;

    /* local size for the pixel accumulator */
    pixelAccumulatorLocalMemorySize = chunkSize * sizeof(cl_float4);
    totalLocalMemorySize+=pixelAccumulatorLocalMemorySize;

    /* local memory size for the weight accumulator */
    weightAccumulatorLocalMemorySize = chunkSize * sizeof(float);
    totalLocalMemorySize+=weightAccumulatorLocalMemorySize;

    /* local memory size for the gamma accumulator */
    if (matte == 0)
      gammaAccumulatorLocalMemorySize = sizeof(float);
    else
      gammaAccumulatorLocalMemorySize = chunkSize * sizeof(float);
    totalLocalMemorySize+=gammaAccumulatorLocalMemorySize;

    if (totalLocalMemorySize <= deviceLocalMemorySize)
      break;
    else
    {
      pixelPerWorkgroup = pixelPerWorkgroup/2;
      chunkSize = chunkSize/2;
      if (pixelPerWorkgroup == 0
          || chunkSize == 0)
      {
        /* quit, fallback to CPU */
        goto cleanup;
      }
    }
  }

  resizeFilterType = (int)GetResizeFilterWeightingType(resizeFilter);
  resizeWindowType = (int)GetResizeFilterWindowWeightingType(resizeFilter);


  if (resizeFilterType == SincFastWeightingFunction
    && resizeWindowType == SincFastWeightingFunction)
  {
    horizontalKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "ResizeHorizontalFilterSinc");
  }
  else
  {
    horizontalKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "ResizeHorizontalFilter");
  }
  if (horizontalKernel == NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }

  i = 0;
  clStatus = clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(cl_mem), (void*)&image);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(unsigned int), (void*)&imageColumns);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(unsigned int), (void*)&imageRows);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(unsigned int), (void*)&matte);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(float), (void*)&xFactor);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(cl_mem), (void*)&resizedImage);

  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(unsigned int), (void*)&resizedColumns);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(unsigned int), (void*)&resizedRows);

  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(int), (void*)&resizeFilterType);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(int), (void*)&resizeWindowType);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(cl_mem), (void*)&resizeFilterCubicCoefficients);

  resizeFilterScale = (float) GetResizeFilterScale(resizeFilter);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(float), (void*)&resizeFilterScale);

  resizeFilterSupport = (float) GetResizeFilterSupport(resizeFilter);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(float), (void*)&resizeFilterSupport);

  resizeFilterWindowSupport = (float) GetResizeFilterWindowSupport(resizeFilter);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(float), (void*)&resizeFilterWindowSupport);

  resizeFilterBlur = (float) GetResizeFilterBlur(resizeFilter);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(float), (void*)&resizeFilterBlur);


  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, imageCacheLocalMemorySize, NULL);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(int), &numCachedPixels);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(unsigned int), &pixelPerWorkgroup);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, sizeof(unsigned int), &chunkSize);
  

  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, pixelAccumulatorLocalMemorySize, NULL);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, weightAccumulatorLocalMemorySize, NULL);
  clStatus |= clEnv->library->clSetKernelArg(horizontalKernel, i++, gammaAccumulatorLocalMemorySize, NULL);

  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }

  global_work_size[0] = (resizedColumns+pixelPerWorkgroup-1)/pixelPerWorkgroup*workgroupSize;
  global_work_size[1] = resizedRows;

  local_work_size[0] = workgroupSize;
  local_work_size[1] = 1;
  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, horizontalKernel, 2, NULL, global_work_size, local_work_size, 0, NULL, &event);
  (void) local_work_size;
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);
  RecordProfileData(clEnv,ResizeHorizontalKernel,event);
  clEnv->library->clReleaseEvent(event);
  status = MagickTrue;


cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  if (horizontalKernel != NULL) RelinquishOpenCLKernel(clEnv, horizontalKernel);

  return(status);
}

static MagickBooleanType resizeVerticalFilter(cl_mem image,
  const unsigned int imageColumns,const unsigned int imageRows,
  const unsigned int matte,cl_mem resizedImage,
  const unsigned int resizedColumns,const unsigned int resizedRows,
  const ResizeFilter *resizeFilter,cl_mem resizeFilterCubicCoefficients,
  const float yFactor,MagickCLEnv clEnv,cl_command_queue queue,
  ExceptionInfo *exception)
{
  cl_kernel
    verticalKernel;

  cl_event
    event;

  cl_int clStatus;

  const unsigned int
    workgroupSize = 256;

  float
    resizeFilterScale,
    resizeFilterSupport,
    resizeFilterWindowSupport,
    resizeFilterBlur,
    scale,
    support;

  int
    cacheRangeStart,
    cacheRangeEnd,
    numCachedPixels,
    resizeFilterType,
    resizeWindowType;

  MagickBooleanType
    status = MagickFalse;

  size_t
    deviceLocalMemorySize,
    gammaAccumulatorLocalMemorySize,
    global_work_size[2],
    imageCacheLocalMemorySize,
    pixelAccumulatorLocalMemorySize,
    local_work_size[2],
    totalLocalMemorySize,
    weightAccumulatorLocalMemorySize;

  unsigned int
    chunkSize,
    i,
    pixelPerWorkgroup;

  verticalKernel = NULL;
  status = MagickFalse;

  /*
  Apply filter to resize vertically from image to resize image.
  */
  scale=MAGICK_MAX(1.0/yFactor+MagickEpsilon,1.0);
  support=scale*GetResizeFilterSupport(resizeFilter);
  if (support < 0.5)
  {
    /*
    Support too small even for nearest neighbour: Reduce to point
    sampling.
    */
    support=(MagickRealType) 0.5;
    scale=1.0;
  }
  scale=PerceptibleReciprocal(scale);

  if (resizedRows < workgroupSize) 
  {
    chunkSize = 32;
    pixelPerWorkgroup = 32;
  }
  else
  {
    chunkSize = workgroupSize;
    pixelPerWorkgroup = workgroupSize;
  }

  /* get the local memory size supported by the device */
  deviceLocalMemorySize = GetOpenCLDeviceLocalMemorySize(clEnv);

DisableMSCWarning(4127)
  while(1)
RestoreMSCWarning
  {
    /* calculate the local memory size needed per workgroup */
    cacheRangeStart = (int) (((0 + 0.5)/yFactor+MagickEpsilon)-support+0.5);
    cacheRangeEnd = (int) ((((pixelPerWorkgroup-1) + 0.5)/yFactor+MagickEpsilon)+support+0.5);
    numCachedPixels = cacheRangeEnd - cacheRangeStart + 1;
    imageCacheLocalMemorySize = numCachedPixels * sizeof(CLPixelPacket);
    totalLocalMemorySize = imageCacheLocalMemorySize;

    /* local size for the pixel accumulator */
    pixelAccumulatorLocalMemorySize = chunkSize * sizeof(cl_float4);
    totalLocalMemorySize+=pixelAccumulatorLocalMemorySize;

    /* local memory size for the weight accumulator */
    weightAccumulatorLocalMemorySize = chunkSize * sizeof(float);
    totalLocalMemorySize+=weightAccumulatorLocalMemorySize;

    /* local memory size for the gamma accumulator */
    if (matte == 0)
      gammaAccumulatorLocalMemorySize = sizeof(float);
    else
      gammaAccumulatorLocalMemorySize = chunkSize * sizeof(float);
    totalLocalMemorySize+=gammaAccumulatorLocalMemorySize;

    if (totalLocalMemorySize <= deviceLocalMemorySize)
      break;
    else
    {
      pixelPerWorkgroup = pixelPerWorkgroup/2;
      chunkSize = chunkSize/2;
      if (pixelPerWorkgroup == 0
          || chunkSize == 0)
      {
        /* quit, fallback to CPU */
        goto cleanup;
      }
    }
  }

  resizeFilterType = (int)GetResizeFilterWeightingType(resizeFilter);
  resizeWindowType = (int)GetResizeFilterWindowWeightingType(resizeFilter);

  if (resizeFilterType == SincFastWeightingFunction
    && resizeWindowType == SincFastWeightingFunction)
    verticalKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "ResizeVerticalFilterSinc");
  else 
    verticalKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "ResizeVerticalFilter");

  if (verticalKernel == NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }

  i = 0;
  clStatus = clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(cl_mem), (void*)&image);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(unsigned int), (void*)&imageColumns);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(unsigned int), (void*)&imageRows);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(unsigned int), (void*)&matte);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(float), (void*)&yFactor);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(cl_mem), (void*)&resizedImage);

  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(unsigned int), (void*)&resizedColumns);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(unsigned int), (void*)&resizedRows);

  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(int), (void*)&resizeFilterType);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(int), (void*)&resizeWindowType);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(cl_mem), (void*)&resizeFilterCubicCoefficients);

  resizeFilterScale = (float) GetResizeFilterScale(resizeFilter);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(float), (void*)&resizeFilterScale);

  resizeFilterSupport = (float) GetResizeFilterSupport(resizeFilter);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(float), (void*)&resizeFilterSupport);

  resizeFilterWindowSupport = (float) GetResizeFilterWindowSupport(resizeFilter);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(float), (void*)&resizeFilterWindowSupport);

  resizeFilterBlur = (float) GetResizeFilterBlur(resizeFilter);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(float), (void*)&resizeFilterBlur);


  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, imageCacheLocalMemorySize, NULL);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(int), &numCachedPixels);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(unsigned int), &pixelPerWorkgroup);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, sizeof(unsigned int), &chunkSize);
  

  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, pixelAccumulatorLocalMemorySize, NULL);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, weightAccumulatorLocalMemorySize, NULL);
  clStatus |= clEnv->library->clSetKernelArg(verticalKernel, i++, gammaAccumulatorLocalMemorySize, NULL);

  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }

  global_work_size[0] = resizedColumns;
  global_work_size[1] = (resizedRows+pixelPerWorkgroup-1)/pixelPerWorkgroup*workgroupSize;

  local_work_size[0] = 1;
  local_work_size[1] = workgroupSize;
  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, verticalKernel, 2, NULL, global_work_size, local_work_size, 0, NULL, &event);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);
  RecordProfileData(clEnv,ResizeVerticalKernel,event);
  clEnv->library->clReleaseEvent(event);
  status = MagickTrue;


cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  if (verticalKernel != NULL) RelinquishOpenCLKernel(clEnv, verticalKernel);

  return(status);
}

static Image *ComputeResizeImage(const Image* image,
  const size_t resizedColumns,const size_t resizedRows,
  const ResizeFilter *resizeFilter,ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  cl_command_queue
    queue;

  cl_int
    clStatus;

  cl_context
    context;

  cl_mem
    cubicCoefficientsBuffer,
    filteredImageBuffer,
    imageBuffer,
    tempImageBuffer;

  cl_mem_flags
    mem_flags;

  const double
    *resizeFilterCoefficient;

  const void
    *inputPixels;

  float
    *mappedCoefficientBuffer,
    xFactor,
    yFactor;

  MagickBooleanType
    outputReady,
    status;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  Image
    *filteredImage;

  unsigned int
    i,
    matte;

  void
    *filteredPixels,
    *hostPtr;

  outputReady = MagickFalse;
  filteredImage = NULL;
  filteredImage_view = NULL;
  clEnv = NULL;
  context = NULL;
  imageBuffer = NULL;
  tempImageBuffer = NULL;
  filteredImageBuffer = NULL;
  cubicCoefficientsBuffer = NULL;
  queue = NULL;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);

  /* Create and initialize OpenCL buffers. */
  image_view=AcquireVirtualCacheView(image,exception);
  inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (const void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  cubicCoefficientsBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_ONLY, 7 * sizeof(float), NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }
  queue = AcquireOpenCLCommandQueue(clEnv);
  mappedCoefficientBuffer = (float*)clEnv->library->clEnqueueMapBuffer(queue, cubicCoefficientsBuffer, CL_TRUE, CL_MAP_WRITE, 0, 7 * sizeof(float)
          , 0, NULL, NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueMapBuffer failed.",".");
    goto cleanup;
  }
  resizeFilterCoefficient = GetResizeFilterCoefficient(resizeFilter);
  for (i = 0; i < 7; i++)
  {
    mappedCoefficientBuffer[i] = (float) resizeFilterCoefficient[i];
  }
  clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, cubicCoefficientsBuffer, mappedCoefficientBuffer, 0, NULL, NULL);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
    goto cleanup;
  }

  filteredImage = CloneImage(image,resizedColumns,resizedRows,MagickTrue,exception);
  if (filteredImage == NULL)
    goto cleanup;

  if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
    goto cleanup;
  }
  filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
  filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
  if (filteredPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
    goto cleanup;
  }

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
    hostPtr = filteredPixels;
  }
  else 
  {
    mem_flags = CL_MEM_WRITE_ONLY;
    hostPtr = NULL;
  }

  /* create a CL buffer from image pixel buffer */
  length = filteredImage->columns * filteredImage->rows;
  filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  xFactor=(float) resizedColumns/(float) image->columns;
  yFactor=(float) resizedRows/(float) image->rows;
  matte=(image->alpha_trait > CopyPixelTrait)?1:0;
  if (xFactor > yFactor)
  {

    length = resizedColumns*image->rows;
    tempImageBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_WRITE, length*sizeof(CLPixelPacket), NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
    
    status = resizeHorizontalFilter(imageBuffer, (unsigned int) image->columns, (unsigned int) image->rows, matte
          , tempImageBuffer, (unsigned int) resizedColumns, (unsigned int) image->rows
          , resizeFilter, cubicCoefficientsBuffer
          , xFactor, clEnv, queue, exception);
    if (status != MagickTrue)
      goto cleanup;
    
    status = resizeVerticalFilter(tempImageBuffer, (unsigned int) resizedColumns, (unsigned int) image->rows, matte
       , filteredImageBuffer, (unsigned int) resizedColumns, (unsigned int) resizedRows
       , resizeFilter, cubicCoefficientsBuffer
       , yFactor, clEnv, queue, exception);
    if (status != MagickTrue)
      goto cleanup;
  }
  else
  {
    length = image->columns*resizedRows;
    tempImageBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_WRITE, length*sizeof(CLPixelPacket), NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }

    status = resizeVerticalFilter(imageBuffer, (unsigned int) image->columns, (unsigned int) image->rows, matte
       , tempImageBuffer, (unsigned int) image->columns, (unsigned int) resizedRows
       , resizeFilter, cubicCoefficientsBuffer
       , yFactor, clEnv, queue, exception);
    if (status != MagickTrue)
      goto cleanup;

    status = resizeHorizontalFilter(tempImageBuffer, (unsigned int) image->columns, (unsigned int) resizedRows, matte
       , filteredImageBuffer, (unsigned int) resizedColumns, (unsigned int) resizedRows
       , resizeFilter, cubicCoefficientsBuffer
       , xFactor, clEnv, queue, exception);
    if (status != MagickTrue)
      goto cleanup;
  }
  length = resizedColumns*resizedRows;
  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }
  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (imageBuffer!=NULL)		  clEnv->library->clReleaseMemObject(imageBuffer);
  if (tempImageBuffer!=NULL)		  clEnv->library->clReleaseMemObject(tempImageBuffer);
  if (filteredImageBuffer!=NULL)	  clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (cubicCoefficientsBuffer!=NULL)      clEnv->library->clReleaseMemObject(cubicCoefficientsBuffer);
  if (queue != NULL)  	                  RelinquishOpenCLCommandQueue(clEnv, queue);
  if (outputReady == MagickFalse && filteredImage != NULL)
    filteredImage=DestroyImage(filteredImage);
  return(filteredImage);
}

static MagickBooleanType gpuSupportedResizeWeighting(
  ResizeWeightingFunctionType f)
{
  unsigned int
    i;

  for (i = 0; ;i++)
  {
    if (supportedResizeWeighting[i] == LastWeightingFunction)
      break;
    if (supportedResizeWeighting[i] == f)
      return(MagickTrue);
  }
  return(MagickFalse);
}

MagickExport Image *AccelerateResizeImage(const Image *image,
  const size_t resizedColumns,const size_t resizedRows,
  const ResizeFilter *resizeFilter,ExceptionInfo *exception) 
{
  Image
    *filteredImage;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return NULL;

  if ((gpuSupportedResizeWeighting(GetResizeFilterWeightingType(
         resizeFilter)) == MagickFalse) ||
      (gpuSupportedResizeWeighting(GetResizeFilterWindowWeightingType(
         resizeFilter)) == MagickFalse))
    return NULL;

  filteredImage=ComputeResizeImage(image,resizedColumns,resizedRows,
    resizeFilter,exception);
  return(filteredImage);
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e R o t a t i o n a l B l u r I m a g e               %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static Image* ComputeRotationalBlurImage(const Image *image,const double angle,
  ExceptionInfo *exception)
{
  CacheView
    *image_view,
    *filteredImage_view;

  cl_command_queue
    queue;

  cl_context
    context;

  cl_float2
    blurCenter;

  cl_float4
    biasPixel;

  cl_int
    clStatus;

  cl_mem
    cosThetaBuffer,
    filteredImageBuffer,
    imageBuffer,
    sinThetaBuffer;

  cl_mem_flags
    mem_flags;

  cl_kernel
    rotationalBlurKernel;

  cl_event
    event;

  const void
    *inputPixels;

  float
    blurRadius,
    *cosThetaPtr,
    offset,
    *sinThetaPtr,
    theta;

  Image
    *filteredImage;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  PixelInfo
    bias;

  MagickSizeType
    length;

  size_t
    global_work_size[2];

  unsigned int
    cossin_theta_size,
    i,
    matte;

  void
    *filteredPixels,
    *hostPtr;

  outputReady = MagickFalse;
  context = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  imageBuffer = NULL;
  filteredImageBuffer = NULL;
  sinThetaBuffer = NULL;
  cosThetaBuffer = NULL;
  queue = NULL;
  rotationalBlurKernel = NULL;


  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);


  /* Create and initialize OpenCL buffers. */

  image_view=AcquireVirtualCacheView(image,exception);
  inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
  if (inputPixels == (const void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
  }
  else 
  {
    mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }


  filteredImage = CloneImage(image,image->columns,image->rows,MagickTrue,exception);
  assert(filteredImage != NULL);
  if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
    goto cleanup;
  }
  filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
  filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
  if (filteredPixels == (void *) NULL)
  {
    (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
    goto cleanup;
  }

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
    hostPtr = filteredPixels;
  }
  else 
  {
    mem_flags = CL_MEM_WRITE_ONLY;
    hostPtr = NULL;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }

  blurCenter.s[0] = (float) (image->columns-1)/2.0;
  blurCenter.s[1] = (float) (image->rows-1)/2.0;
  blurRadius=hypot(blurCenter.s[0],blurCenter.s[1]);
  cossin_theta_size=(unsigned int) fabs(4.0*DegreesToRadians(angle)*sqrt((double)blurRadius)+2UL);

  /* create a buffer for sin_theta and cos_theta */
  sinThetaBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR, cossin_theta_size * sizeof(float), NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }
  cosThetaBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_ONLY|CL_MEM_ALLOC_HOST_PTR, cossin_theta_size * sizeof(float), NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
    goto cleanup;
  }


  queue = AcquireOpenCLCommandQueue(clEnv);
  sinThetaPtr = (float*) clEnv->library->clEnqueueMapBuffer(queue, sinThetaBuffer, CL_TRUE, CL_MAP_WRITE, 0, cossin_theta_size*sizeof(float), 0, NULL, NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnqueuemapBuffer failed.",".");
    goto cleanup;
  }

  cosThetaPtr = (float*) clEnv->library->clEnqueueMapBuffer(queue, cosThetaBuffer, CL_TRUE, CL_MAP_WRITE, 0, cossin_theta_size*sizeof(float), 0, NULL, NULL, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnqueuemapBuffer failed.",".");
    goto cleanup;
  }

  theta=DegreesToRadians(angle)/(MagickRealType) (cossin_theta_size-1);
  offset=theta*(MagickRealType) (cossin_theta_size-1)/2.0;
  for (i=0; i < (ssize_t) cossin_theta_size; i++)
  {
    cosThetaPtr[i]=(float)cos((double) (theta*i-offset));
    sinThetaPtr[i]=(float)sin((double) (theta*i-offset));
  }
 
  clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, sinThetaBuffer, sinThetaPtr, 0, NULL, NULL);
  clStatus |= clEnv->library->clEnqueueUnmapMemObject(queue, cosThetaBuffer, cosThetaPtr, 0, NULL, NULL);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
    goto cleanup;
  }

  /* get the OpenCL kernel */
  rotationalBlurKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "RotationalBlur");
  if (rotationalBlurKernel == NULL)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  }

  
  /* set the kernel arguments */
  i = 0;
  clStatus=clEnv->library->clSetKernelArg(rotationalBlurKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
  clStatus|=clEnv->library->clSetKernelArg(rotationalBlurKernel,i++,sizeof(cl_mem),(void *)&filteredImageBuffer);

  GetPixelInfo(image,&bias);
  biasPixel.s[0] = bias.red;
  biasPixel.s[1] = bias.green;
  biasPixel.s[2] = bias.blue;
  biasPixel.s[3] = bias.alpha;
  clStatus|=clEnv->library->clSetKernelArg(rotationalBlurKernel,i++,sizeof(cl_float4), &biasPixel);
  clStatus|=clEnv->library->clSetKernelArg(rotationalBlurKernel,i++,sizeof(ChannelType), &image->channel_mask);

  matte = (image->alpha_trait > CopyPixelTrait)?1:0;
  clStatus|=clEnv->library->clSetKernelArg(rotationalBlurKernel,i++,sizeof(unsigned int), &matte);

  clStatus=clEnv->library->clSetKernelArg(rotationalBlurKernel,i++,sizeof(cl_float2), &blurCenter);

  clStatus|=clEnv->library->clSetKernelArg(rotationalBlurKernel,i++,sizeof(cl_mem),(void *)&cosThetaBuffer);
  clStatus|=clEnv->library->clSetKernelArg(rotationalBlurKernel,i++,sizeof(cl_mem),(void *)&sinThetaBuffer);
  clStatus|=clEnv->library->clSetKernelArg(rotationalBlurKernel,i++,sizeof(unsigned int), &cossin_theta_size);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
    goto cleanup;
  }


  global_work_size[0] = image->columns;
  global_work_size[1] = image->rows;
  /* launch the kernel */
  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, rotationalBlurKernel, 2, NULL, global_work_size, NULL, 0, NULL, &event);
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
    goto cleanup;
  }
  clEnv->library->clFlush(queue);
  RecordProfileData(clEnv,RotationalBlurKernel,event);
  clEnv->library->clReleaseEvent(event);

  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }
  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (filteredImageBuffer!=NULL)  clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (imageBuffer!=NULL)     clEnv->library->clReleaseMemObject(imageBuffer);
  if (sinThetaBuffer!=NULL)       clEnv->library->clReleaseMemObject(sinThetaBuffer);
  if (cosThetaBuffer!=NULL)       clEnv->library->clReleaseMemObject(cosThetaBuffer);
  if (rotationalBlurKernel!=NULL) RelinquishOpenCLKernel(clEnv, rotationalBlurKernel);
  if (queue != NULL)              RelinquishOpenCLCommandQueue(clEnv, queue);
  if (outputReady == MagickFalse)
  {
    if (filteredImage != NULL)
    {
      DestroyImage(filteredImage);
      filteredImage = NULL;
    }
  }
  return filteredImage;
}

MagickExport Image* AccelerateRotationalBlurImage(const Image *image,
  const double angle,ExceptionInfo *exception)
{
  Image
    *filteredImage;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return NULL;

  filteredImage=ComputeRotationalBlurImage(image,angle,exception);
  return filteredImage;
}

/*
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
%                                                                             %
%                                                                             %
%                                                                             %
%     A c c e l e r a t e U n s h a r p M a s k I m a g e                     %
%                                                                             %
%                                                                             %
%                                                                             %
%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
*/

static Image *ComputeUnsharpMaskImage(const Image *image,const double radius,
  const double sigma,const double gain,const double threshold,
  ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  char
    geometry[MagickPathExtent];

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus;

  cl_kernel
    blurRowKernel,
    unsharpMaskBlurColumnKernel;

  cl_event
    event;

  cl_mem
    filteredImageBuffer,
    imageBuffer,
    imageKernelBuffer,
    tempImageBuffer;

  cl_mem_flags
    mem_flags;

  const void
    *inputPixels;

  float
    fGain,
    fThreshold,
    *kernelBufferPtr;

  Image
    *filteredImage;

  int
    chunkSize;

  KernelInfo
    *kernel;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  void
    *filteredPixels,
    *hostPtr;

  unsigned int
    i,
    imageColumns,
    imageRows,
    kernelWidth;

  clEnv = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  kernel = NULL;
  context = NULL;
  imageBuffer = NULL;
  filteredImageBuffer = NULL;
  tempImageBuffer = NULL;
  imageKernelBuffer = NULL;
  blurRowKernel = NULL;
  unsharpMaskBlurColumnKernel = NULL;
  queue = NULL;
  outputReady = MagickFalse;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  /* Create and initialize OpenCL buffers. */
  {
    image_view=AcquireVirtualCacheView(image,exception);
    inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
    if (inputPixels == (const void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
      goto cleanup;
    }

    /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
    if (ALIGNED(inputPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
    }
    else 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
    }
    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create output */
  {
    filteredImage = CloneImage(image,image->columns,image->rows,MagickTrue,exception);
    assert(filteredImage != NULL);
    if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
      goto cleanup;
    }
    filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
    filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
    if (filteredPixels == (void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
      goto cleanup;
    }

    if (ALIGNED(filteredPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
      hostPtr = filteredPixels;
    }
    else 
    {
      mem_flags = CL_MEM_WRITE_ONLY;
      hostPtr = NULL;
    }

    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create the blur kernel */
  {
    (void) FormatLocaleString(geometry,MagickPathExtent,"blur:%.20gx%.20g;blur:%.20gx%.20g+90",radius,sigma,radius,sigma);
    kernel=AcquireKernelInfo(geometry,exception);
    if (kernel == (KernelInfo *) NULL)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireKernelInfo failed.",".");
      goto cleanup;
    }

    imageKernelBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_ONLY, kernel->width * sizeof(float), NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }


    kernelBufferPtr = (float*)clEnv->library->clEnqueueMapBuffer(queue, imageKernelBuffer, CL_TRUE, CL_MAP_WRITE, 0, kernel->width * sizeof(float), 0, NULL, NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueMapBuffer failed.",".");
      goto cleanup;
    }
    for (i = 0; i < kernel->width; i++)
    {
      kernelBufferPtr[i] = (float) kernel->values[i];
    }
    clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, imageKernelBuffer, kernelBufferPtr, 0, NULL, NULL);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
      goto cleanup;
    }
  }

  {
    /* create temp buffer */
    {
      length = image->columns * image->rows;
      tempImageBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_WRITE, length * 4 * sizeof(float), NULL, &clStatus);
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
        goto cleanup;
      }
    }

    /* get the opencl kernel */
    {
      blurRowKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "BlurRow");
      if (blurRowKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };

      unsharpMaskBlurColumnKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "UnsharpMaskBlurColumn");
      if (unsharpMaskBlurColumnKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };
    }

    {
      chunkSize = 256;

      imageColumns = (unsigned int) image->columns;
      imageRows = (unsigned int) image->rows;

      kernelWidth = (unsigned int) kernel->width;

      /* set the kernel arguments */
      i = 0;
      clStatus=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&tempImageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(ChannelType),&image->channel_mask);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&imageKernelBuffer);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&kernelWidth);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&imageRows);
      clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(CLPixelPacket)*(chunkSize+kernel->width),(void *) NULL);
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
        goto cleanup;
      }
    }

    /* launch the kernel */
    {
      size_t gsize[2];
      size_t wsize[2];

      gsize[0] = chunkSize*((image->columns+chunkSize-1)/chunkSize);
      gsize[1] = image->rows;
      wsize[0] = chunkSize;
      wsize[1] = 1;

	  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, blurRowKernel, 2, NULL, gsize, wsize, 0, NULL, &event);
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
        goto cleanup;
      }
      clEnv->library->clFlush(queue);
      RecordProfileData(clEnv,BlurRowKernel,event);
      clEnv->library->clReleaseEvent(event);
    }


    {
      chunkSize = 256;
      imageColumns = (unsigned int) image->columns;
      imageRows = (unsigned int) image->rows;
      kernelWidth = (unsigned int) kernel->width;
      fGain = (float) gain;
      fThreshold = (float) threshold;

      i = 0;
      clStatus=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(cl_mem),(void *)&tempImageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(cl_mem),(void *)&filteredImageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(unsigned int),(void *)&imageRows);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++, (chunkSize+kernelWidth-1)*sizeof(cl_float4),NULL);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++, kernelWidth*sizeof(float),NULL);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(ChannelType),&image->channel_mask);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(cl_mem),(void *)&imageKernelBuffer);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(unsigned int),(void *)&kernelWidth);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(float),(void *)&fGain);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(float),(void *)&fThreshold);

      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
        goto cleanup;
      }
    }

    /* launch the kernel */
    {
      size_t gsize[2];
      size_t wsize[2];

      gsize[0] = image->columns;
      gsize[1] = chunkSize*((image->rows+chunkSize-1)/chunkSize);
      wsize[0] = 1;
      wsize[1] = chunkSize;

	  clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, unsharpMaskBlurColumnKernel, 2, NULL, gsize, wsize, 0, NULL, &event);
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
        goto cleanup;
      }
      clEnv->library->clFlush(queue);
      RecordProfileData(clEnv,UnsharpMaskBlurColumnKernel,event);
      clEnv->library->clReleaseEvent(event);
    }

  }

  /* get result */
  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (kernel != NULL)			      kernel=DestroyKernelInfo(kernel);
  if (imageBuffer!=NULL)		      clEnv->library->clReleaseMemObject(imageBuffer);
  if (filteredImageBuffer!=NULL)              clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (tempImageBuffer!=NULL)                  clEnv->library->clReleaseMemObject(tempImageBuffer);
  if (imageKernelBuffer!=NULL)                clEnv->library->clReleaseMemObject(imageKernelBuffer);
  if (blurRowKernel!=NULL)                    RelinquishOpenCLKernel(clEnv, blurRowKernel);
  if (unsharpMaskBlurColumnKernel!=NULL)      RelinquishOpenCLKernel(clEnv, unsharpMaskBlurColumnKernel);
  if (queue != NULL)                          RelinquishOpenCLCommandQueue(clEnv, queue);
  if (outputReady == MagickFalse)
  {
    if (filteredImage != NULL)
    {
      DestroyImage(filteredImage);
      filteredImage = NULL;
    }
  }
  return(filteredImage);
}

static Image *ComputeUnsharpMaskImageSection(const Image *image,
  const double radius,const double sigma,const double gain,
  const double threshold,ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  char
    geometry[MagickPathExtent];

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    clStatus;

  cl_kernel
    blurRowKernel,
    unsharpMaskBlurColumnKernel;

  cl_event
    event;

  cl_mem
    filteredImageBuffer,
    imageBuffer,
    imageKernelBuffer,
    tempImageBuffer;

  cl_mem_flags
    mem_flags;

  const void
    *inputPixels;

  float
    fGain,
    fThreshold,
    *kernelBufferPtr;

  Image
    *filteredImage;

  int
    chunkSize;

  KernelInfo
    *kernel;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  void
    *filteredPixels,
    *hostPtr;

  unsigned int
    i,
    imageColumns,
    imageRows,
    kernelWidth;

  clEnv = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  kernel = NULL;
  context = NULL;
  imageBuffer = NULL;
  filteredImageBuffer = NULL;
  tempImageBuffer = NULL;
  imageKernelBuffer = NULL;
  blurRowKernel = NULL;
  unsharpMaskBlurColumnKernel = NULL;
  queue = NULL;
  outputReady = MagickFalse;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  /* Create and initialize OpenCL buffers. */
  {
    image_view=AcquireVirtualCacheView(image,exception);
    inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
    if (inputPixels == (const void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
      goto cleanup;
    }

    /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
    if (ALIGNED(inputPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
    }
    else 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
    }
    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create output */
  {
    filteredImage = CloneImage(image,image->columns,image->rows,MagickTrue,exception);
    assert(filteredImage != NULL);
    if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
      goto cleanup;
    }
    filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
    filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
    if (filteredPixels == (void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
      goto cleanup;
    }

    if (ALIGNED(filteredPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
      hostPtr = filteredPixels;
    }
    else 
    {
      mem_flags = CL_MEM_WRITE_ONLY;
      hostPtr = NULL;
    }

    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create the blur kernel */
  {
    (void) FormatLocaleString(geometry,MagickPathExtent,"blur:%.20gx%.20g;blur:%.20gx%.20g+90",radius,sigma,radius,sigma);
    kernel=AcquireKernelInfo(geometry,exception);
    if (kernel == (KernelInfo *) NULL)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireKernelInfo failed.",".");
      goto cleanup;
    }

    imageKernelBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_ONLY, kernel->width * sizeof(float), NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }


    kernelBufferPtr = (float*)clEnv->library->clEnqueueMapBuffer(queue, imageKernelBuffer, CL_TRUE, CL_MAP_WRITE, 0, kernel->width * sizeof(float), 0, NULL, NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueMapBuffer failed.",".");
      goto cleanup;
    }
    for (i = 0; i < kernel->width; i++)
    {
      kernelBufferPtr[i] = (float) kernel->values[i];
    }
    clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, imageKernelBuffer, kernelBufferPtr, 0, NULL, NULL);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
      goto cleanup;
    }
  }

  {
    unsigned int offsetRows;
    unsigned int sec;

    /* create temp buffer */
    {
      length = image->columns * (image->rows / 2 + 1 + (kernel->width-1) / 2);
      tempImageBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_WRITE, length * 4 * sizeof(float), NULL, &clStatus);
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
        goto cleanup;
      }
    }

    /* get the opencl kernel */
    {
      blurRowKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "BlurRowSection");
      if (blurRowKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };

      unsharpMaskBlurColumnKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "UnsharpMaskBlurColumnSection");
      if (unsharpMaskBlurColumnKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };
    }

    for (sec = 0; sec < 2; sec++)
    {
      {
        chunkSize = 256;

        imageColumns = (unsigned int) image->columns;
        if (sec == 0)
          imageRows = (unsigned int) (image->rows / 2 + (kernel->width-1) / 2);
        else
          imageRows = (unsigned int) ((image->rows - image->rows / 2) + (kernel->width-1) / 2);

        offsetRows = (unsigned int) (sec * image->rows / 2);

        kernelWidth = (unsigned int) kernel->width;

        /* set the kernel arguments */
        i = 0;
        clStatus=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&tempImageBuffer);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(ChannelType),&image->channel_mask);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(cl_mem),(void *)&imageKernelBuffer);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&kernelWidth);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&imageRows);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(CLPixelPacket)*(chunkSize+kernel->width),(void *) NULL);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&offsetRows);
        clStatus|=clEnv->library->clSetKernelArg(blurRowKernel,i++,sizeof(unsigned int),(void *)&sec);
        if (clStatus != CL_SUCCESS)
        {
          (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
          goto cleanup;
        }
      }
      /* launch the kernel */
      {
        size_t gsize[2];
        size_t wsize[2];

        gsize[0] = chunkSize*((imageColumns+chunkSize-1)/chunkSize);
        gsize[1] = imageRows;
        wsize[0] = chunkSize;
        wsize[1] = 1;

		clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, blurRowKernel, 2, NULL, gsize, wsize, 0, NULL, &event);
        if (clStatus != CL_SUCCESS)
        {
          (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
          goto cleanup;
        }
        clEnv->library->clFlush(queue);
        RecordProfileData(clEnv,BlurRowKernel,event);
        clEnv->library->clReleaseEvent(event);
      }


      {
        chunkSize = 256;

        imageColumns = (unsigned int) image->columns;
        if (sec == 0)
          imageRows = (unsigned int) (image->rows / 2);
        else
          imageRows = (unsigned int) (image->rows - image->rows / 2);

        offsetRows = (unsigned int) (sec * image->rows / 2);

        kernelWidth = (unsigned int) kernel->width;

        fGain = (float) gain;
        fThreshold = (float) threshold;

        i = 0;
        clStatus=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(cl_mem),(void *)&tempImageBuffer);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(cl_mem),(void *)&filteredImageBuffer);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(unsigned int),(void *)&imageRows);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++, (chunkSize+kernelWidth-1)*sizeof(cl_float4),NULL);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++, kernelWidth*sizeof(float),NULL);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(ChannelType),&image->channel_mask);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(cl_mem),(void *)&imageKernelBuffer);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(unsigned int),(void *)&kernelWidth);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(float),(void *)&fGain);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(float),(void *)&fThreshold);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(unsigned int),(void *)&offsetRows);
        clStatus|=clEnv->library->clSetKernelArg(unsharpMaskBlurColumnKernel,i++,sizeof(unsigned int),(void *)&sec);

        if (clStatus != CL_SUCCESS)
        {
          (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
          goto cleanup;
        }
      }

      /* launch the kernel */
      {
        size_t gsize[2];
        size_t wsize[2];

        gsize[0] = imageColumns;
        gsize[1] = chunkSize*((imageRows+chunkSize-1)/chunkSize);
        wsize[0] = 1;
        wsize[1] = chunkSize;

		clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, unsharpMaskBlurColumnKernel, 2, NULL, gsize, wsize, 0, NULL, &event);
        if (clStatus != CL_SUCCESS)
        {
          (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
          goto cleanup;
        }
        clEnv->library->clFlush(queue);
        RecordProfileData(clEnv,UnsharpMaskBlurColumnKernel,event);
        clEnv->library->clReleaseEvent(event);
      }
    }
  }

  /* get result */
  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (kernel != NULL)			      kernel=DestroyKernelInfo(kernel);
  if (imageBuffer!=NULL)		      clEnv->library->clReleaseMemObject(imageBuffer);
  if (filteredImageBuffer!=NULL)              clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (tempImageBuffer!=NULL)                  clEnv->library->clReleaseMemObject(tempImageBuffer);
  if (imageKernelBuffer!=NULL)                clEnv->library->clReleaseMemObject(imageKernelBuffer);
  if (blurRowKernel!=NULL)                    RelinquishOpenCLKernel(clEnv, blurRowKernel);
  if (unsharpMaskBlurColumnKernel!=NULL)      RelinquishOpenCLKernel(clEnv, unsharpMaskBlurColumnKernel);
  if (queue != NULL)                          RelinquishOpenCLCommandQueue(clEnv, queue);
  if (outputReady == MagickFalse)
  {
    if (filteredImage != NULL)
    {
      DestroyImage(filteredImage);
      filteredImage = NULL;
    }
  }
  return filteredImage;
}

static Image *ComputeUnsharpMaskImageSingle(const Image *image,
  const double radius,const double sigma,const double gain,
  const double threshold,int blurOnly, ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  char
    geometry[MagickPathExtent];

  cl_command_queue
    queue;

  cl_context
    context;

  cl_int
    justBlur,
    clStatus;

  cl_kernel
    unsharpMaskKernel;

  cl_event
    event;

  cl_mem
    filteredImageBuffer,
    imageBuffer,
    imageKernelBuffer;

  cl_mem_flags
    mem_flags;

  const void
    *inputPixels;

  float
    fGain,
    fThreshold,
    *kernelBufferPtr;

  Image
    *filteredImage;

  KernelInfo
    *kernel;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  void
    *filteredPixels,
    *hostPtr;

  unsigned int
    i,
    imageColumns,
    imageRows,
    kernelWidth;

  clEnv = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  kernel = NULL;
  context = NULL;
  imageBuffer = NULL;
  filteredImageBuffer = NULL;
  imageKernelBuffer = NULL;
  unsharpMaskKernel = NULL;
  queue = NULL;
  outputReady = MagickFalse;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  /* Create and initialize OpenCL buffers. */
  {
    image_view=AcquireVirtualCacheView(image,exception);
    inputPixels=GetCacheViewVirtualPixels(image_view,0,0,image->columns,image->rows,exception);
    if (inputPixels == (const void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning,"UnableToReadPixelCache.","`%s'",image->filename);
      goto cleanup;
    }

    /* If the host pointer is aligned to the size of CLPixelPacket, 
     then use the host buffer directly from the GPU; otherwise, 
     create a buffer on the GPU and copy the data over */
    if (ALIGNED(inputPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_USE_HOST_PTR;
    }
    else 
    {
      mem_flags = CL_MEM_READ_ONLY|CL_MEM_COPY_HOST_PTR;
    }
    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create output */
  {
    filteredImage = CloneImage(image,image->columns,image->rows,MagickTrue,exception);
    assert(filteredImage != NULL);
    if (SetImageStorageClass(filteredImage,DirectClass,exception) != MagickTrue)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
      goto cleanup;
    }
    filteredImage_view=AcquireAuthenticCacheView(filteredImage,exception);
    filteredPixels=GetCacheViewAuthenticPixels(filteredImage_view,0,0,filteredImage->columns,filteredImage->rows,exception);
    if (filteredPixels == (void *) NULL)
    {
      (void) OpenCLThrowMagickException(exception,GetMagickModule(),CacheWarning, "UnableToReadPixelCache.","`%s'",filteredImage->filename);
      goto cleanup;
    }

    if (ALIGNED(filteredPixels,CLPixelPacket)) 
    {
      mem_flags = CL_MEM_WRITE_ONLY|CL_MEM_USE_HOST_PTR;
      hostPtr = filteredPixels;
    }
    else 
    {
      mem_flags = CL_MEM_WRITE_ONLY;
      hostPtr = NULL;
    }

    /* create a CL buffer from image pixel buffer */
    length = image->columns * image->rows;
    filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }
  }

  /* create the blur kernel */
  {
    (void) FormatLocaleString(geometry,MagickPathExtent,"blur:%.20gx%.20g;blur:%.20gx%.20g+90",radius,sigma,radius,sigma);
    kernel=AcquireKernelInfo(geometry,exception);
    if (kernel == (KernelInfo *) NULL)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireKernelInfo failed.",".");
      goto cleanup;
    }

    imageKernelBuffer = clEnv->library->clCreateBuffer(context, CL_MEM_READ_ONLY, kernel->width * sizeof(float), NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.",".");
      goto cleanup;
    }


    kernelBufferPtr = (float*)clEnv->library->clEnqueueMapBuffer(queue, imageKernelBuffer, CL_TRUE, CL_MAP_WRITE, 0, kernel->width * sizeof(float), 0, NULL, NULL, &clStatus);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueMapBuffer failed.",".");
      goto cleanup;
    }
    for (i = 0; i < kernel->width; i++)
    {
      kernelBufferPtr[i] = (float) kernel->values[i];
    }
    clStatus = clEnv->library->clEnqueueUnmapMemObject(queue, imageKernelBuffer, kernelBufferPtr, 0, NULL, NULL);
    if (clStatus != CL_SUCCESS)
    {
      (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueUnmapMemObject failed.", "'%s'", ".");
      goto cleanup;
    }
  }

  {
    /* get the opencl kernel */
    {
      unsharpMaskKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "UnsharpMask");
      if (unsharpMaskKernel == NULL)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
        goto cleanup;
      };
    }

    {
      imageColumns = (unsigned int) image->columns;
      imageRows = (unsigned int) image->rows;
      kernelWidth = (unsigned int) kernel->width;
      fGain = (float) gain;
      fThreshold = (float) threshold;
      justBlur = blurOnly;

      /* set the kernel arguments */
      i = 0;
      clStatus=clEnv->library->clSetKernelArg(unsharpMaskKernel,i++,sizeof(cl_mem),(void *)&imageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskKernel,i++,sizeof(cl_mem),(void *)&filteredImageBuffer);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskKernel,i++,sizeof(cl_mem),(void *)&imageKernelBuffer);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskKernel,i++,sizeof(unsigned int),(void *)&kernelWidth);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskKernel,i++,sizeof(unsigned int),(void *)&imageColumns);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskKernel,i++,sizeof(unsigned int),(void *)&imageRows);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskKernel,i++,sizeof(cl_float4)*(8 * (32 + kernel->width)),(void *) NULL);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskKernel,i++,sizeof(float),(void *)&fGain);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskKernel,i++,sizeof(float),(void *)&fThreshold);
      clStatus|=clEnv->library->clSetKernelArg(unsharpMaskKernel,i++,sizeof(cl_uint),(void *)&justBlur);
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clSetKernelArg failed.", "'%s'", ".");
        goto cleanup;
      }
    }

    /* launch the kernel */
    {
      size_t gsize[2];
      size_t wsize[2];

      gsize[0] = ((image->columns + 7) / 8) * 8;
      gsize[1] = ((image->rows + 31) / 32) * 32;
      wsize[0] = 8;
      wsize[1] = 32;

	  clStatus = clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, unsharpMaskKernel, 2, NULL, gsize, wsize, 0, NULL, &event);
      if (clStatus != CL_SUCCESS)
      {
        (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
        goto cleanup;
      }
      clEnv->library->clFlush(queue);
      RecordProfileData(clEnv,UnsharpMaskKernel,event);
      clEnv->library->clReleaseEvent(event);
    }
  }

  /* get result */
  if (ALIGNED(filteredPixels,CLPixelPacket)) 
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ|CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else 
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void) OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady=SyncCacheViewAuthenticPixels(filteredImage_view,exception);

cleanup:
  OpenCLLogException(__FUNCTION__,__LINE__,exception);

  image_view=DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view=DestroyCacheView(filteredImage_view);

  if (kernel != NULL)			      kernel=DestroyKernelInfo(kernel);
  if (imageBuffer!=NULL)		      clEnv->library->clReleaseMemObject(imageBuffer);
  if (filteredImageBuffer!=NULL)              clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (imageKernelBuffer!=NULL)                clEnv->library->clReleaseMemObject(imageKernelBuffer);
  if (unsharpMaskKernel!=NULL)                RelinquishOpenCLKernel(clEnv, unsharpMaskKernel);
  if (queue != NULL)                          RelinquishOpenCLCommandQueue(clEnv, queue);
  if (outputReady == MagickFalse)
  {
    if (filteredImage != NULL)
    {
      DestroyImage(filteredImage);
      filteredImage = NULL;
    }
  }
  return(filteredImage);
}

MagickExport Image *AccelerateUnsharpMaskImage(const Image *image,
  const double radius,const double sigma,const double gain,
  const double threshold,ExceptionInfo *exception)
{
  Image
    *filteredImage;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *) NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return NULL;

  if (radius < 12.1)
    filteredImage=ComputeUnsharpMaskImageSingle(image,radius,sigma,gain,
      threshold,0,exception);
  else if (splitImage(image) && (image->rows / 2 > radius))
    filteredImage=ComputeUnsharpMaskImageSection(image,radius,sigma,gain,
      threshold,exception);
  else
    filteredImage=ComputeUnsharpMaskImage(image,radius,sigma,gain,threshold,
      exception);
  return(filteredImage);
}

static Image *ComputeWaveletDenoiseImage(const Image *image,
  const double threshold,ExceptionInfo *exception)
{
  CacheView
    *filteredImage_view,
    *image_view;

  cl_command_queue
    queue;

  cl_context
  context;

  cl_int
    clStatus;

  cl_kernel
    denoiseKernel;

  cl_event
    event;

  cl_mem
    filteredImageBuffer,
    imageBuffer;

  cl_mem_flags
    mem_flags;

  const void
    *inputPixels;

  Image
    *filteredImage;

  MagickBooleanType
    outputReady;

  MagickCLEnv
    clEnv;

  MagickSizeType
    length;

  void
    *filteredPixels,
    *hostPtr;

  unsigned int
    i;

  clEnv = NULL;
  filteredImage = NULL;
  filteredImage_view = NULL;
  context = NULL;
  imageBuffer = NULL;
  filteredImageBuffer = NULL;
  denoiseKernel = NULL;
  queue = NULL;
  outputReady = MagickFalse;

  clEnv = GetDefaultOpenCLEnv();
  context = GetOpenCLContext(clEnv);
  queue = AcquireOpenCLCommandQueue(clEnv);

  /* Create and initialize OpenCL buffers. */
  image_view = AcquireVirtualCacheView(image, exception);
  inputPixels = GetCacheViewVirtualPixels(image_view, 0, 0, image->columns, image->rows, exception);
  if (inputPixels == (const void *)NULL)
  {
    (void)OpenCLThrowMagickException(exception, GetMagickModule(), CacheWarning, "UnableToReadPixelCache.", "`%s'", image->filename);
    goto cleanup;
  }

  /* If the host pointer is aligned to the size of CLPixelPacket,
  then use the host buffer directly from the GPU; otherwise,
  create a buffer on the GPU and copy the data over */
  if (ALIGNED(inputPixels, CLPixelPacket))
  {
    mem_flags = CL_MEM_READ_ONLY | CL_MEM_USE_HOST_PTR;
  }
  else
  {
    mem_flags = CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR;
  }
  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  imageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), (void*)inputPixels, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void)OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.", ".");
    goto cleanup;
  }

  /* create output */
  filteredImage = CloneImage(image, image->columns, image->rows, MagickTrue, exception);
  assert(filteredImage != NULL);
  if (SetImageStorageClass(filteredImage, DirectClass, exception) != MagickTrue)
  {
    (void)OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "CloneImage failed.", "'%s'", ".");
    goto cleanup;
  }
  filteredImage_view = AcquireAuthenticCacheView(filteredImage, exception);
  filteredPixels = GetCacheViewAuthenticPixels(filteredImage_view, 0, 0, filteredImage->columns, filteredImage->rows, exception);
  if (filteredPixels == (void *)NULL)
  {
    (void)OpenCLThrowMagickException(exception, GetMagickModule(), CacheWarning, "UnableToReadPixelCache.", "`%s'", filteredImage->filename);
    goto cleanup;
  }

  if (ALIGNED(filteredPixels, CLPixelPacket))
  {
    mem_flags = CL_MEM_WRITE_ONLY | CL_MEM_USE_HOST_PTR;
    hostPtr = filteredPixels;
  }
  else
  {
    mem_flags = CL_MEM_WRITE_ONLY;
    hostPtr = NULL;
  }

  /* create a CL buffer from image pixel buffer */
  length = image->columns * image->rows;
  filteredImageBuffer = clEnv->library->clCreateBuffer(context, mem_flags, length * sizeof(CLPixelPacket), hostPtr, &clStatus);
  if (clStatus != CL_SUCCESS)
  {
    (void)OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clCreateBuffer failed.", ".");
    goto cleanup;
  }

  /* get the opencl kernel */
  denoiseKernel = AcquireOpenCLKernel(clEnv, MAGICK_OPENCL_ACCELERATE, "WaveletDenoise");
  if (denoiseKernel == NULL)
  {
    (void)OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "AcquireOpenCLKernel failed.", "'%s'", ".");
    goto cleanup;
  };

  // Process image
  {
    const int PASSES = 5;
    cl_int width = (cl_int)image->columns;
    cl_int height = (cl_int)image->rows;
    cl_float thresh = threshold;

    /* set the kernel arguments */
    i = 0;
    clStatus |= clEnv->library->clSetKernelArg(denoiseKernel, i++, sizeof(cl_mem), (void *)&imageBuffer);
    clStatus |= clEnv->library->clSetKernelArg(denoiseKernel, i++, sizeof(cl_mem), (void *)&filteredImageBuffer);
    clStatus |= clEnv->library->clSetKernelArg(denoiseKernel, i++, sizeof(cl_float), (void *)&thresh);
    clStatus |= clEnv->library->clSetKernelArg(denoiseKernel, i++, sizeof(cl_int), (void *)&PASSES);
    clStatus |= clEnv->library->clSetKernelArg(denoiseKernel, i++, sizeof(cl_int), (void *)&width);
    clStatus |= clEnv->library->clSetKernelArg(denoiseKernel, i++, sizeof(cl_int), (void *)&height);

    {
      const int TILESIZE = 64;
      const int PAD = 1 << (PASSES - 1);
      const int SIZE = TILESIZE - 2 * PAD;

      size_t gsize[2];
      size_t wsize[2];

      gsize[0] = ((width + (SIZE - 1)) / SIZE) * TILESIZE;
      gsize[1] = ((height + (SIZE - 1)) / SIZE) * 4;
      wsize[0] = TILESIZE;
      wsize[1] = 4;

      clStatus = clEnv->library->clEnqueueNDRangeKernel(queue, denoiseKernel, 2, NULL, gsize, wsize, 0, NULL, &event);
      if (clStatus != CL_SUCCESS)
      {
        (void)OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "clEnv->library->clEnqueueNDRangeKernel failed.", "'%s'", ".");
        goto cleanup;
      }
    }
    RecordProfileData(clEnv, WaveletDenoiseKernel, event);
    clEnv->library->clReleaseEvent(event);
  }


  /* get result */
  if (ALIGNED(filteredPixels, CLPixelPacket))
  {
    length = image->columns * image->rows;
    clEnv->library->clEnqueueMapBuffer(queue, filteredImageBuffer, CL_TRUE, CL_MAP_READ | CL_MAP_WRITE, 0, length * sizeof(CLPixelPacket), 0, NULL, NULL, &clStatus);
  }
  else
  {
    length = image->columns * image->rows;
    clStatus = clEnv->library->clEnqueueReadBuffer(queue, filteredImageBuffer, CL_TRUE, 0, length * sizeof(CLPixelPacket), filteredPixels, 0, NULL, NULL);
  }
  if (clStatus != CL_SUCCESS)
  {
    (void)OpenCLThrowMagickException(exception, GetMagickModule(), ResourceLimitWarning, "Reading output image from CL buffer failed.", "'%s'", ".");
    goto cleanup;
  }

  outputReady = SyncCacheViewAuthenticPixels(filteredImage_view, exception);

cleanup:
  OpenCLLogException(__FUNCTION__, __LINE__, exception);

  image_view = DestroyCacheView(image_view);
  if (filteredImage_view != NULL)
    filteredImage_view = DestroyCacheView(filteredImage_view);

  if (imageBuffer != NULL)			clEnv->library->clReleaseMemObject(imageBuffer);
  if (filteredImageBuffer != NULL)	clEnv->library->clReleaseMemObject(filteredImageBuffer);
  if (denoiseKernel != NULL)		RelinquishOpenCLKernel(clEnv, denoiseKernel);
  if (queue != NULL)				RelinquishOpenCLCommandQueue(clEnv, queue);
  if (outputReady == MagickFalse)
  {
    if (filteredImage != NULL)
    {
      DestroyImage(filteredImage);
      filteredImage = NULL;
    }
  }
  return(filteredImage);
}

MagickExport Image *AccelerateWaveletDenoiseImage(const Image *image,
  const double threshold,ExceptionInfo *exception)
{
  Image
  *filteredImage;

  assert(image != NULL);
  assert(exception != (ExceptionInfo *)NULL);

  if ((checkAccelerateCondition(image) == MagickFalse) ||
      (checkOpenCLEnvironment(exception) == MagickFalse))
    return (Image *) NULL;

  filteredImage=ComputeWaveletDenoiseImage(image,threshold,exception);

  return(filteredImage);
}

#else  /* MAGICKCORE_OPENCL_SUPPORT  */

MagickExport Image *AccelerateAddNoiseImage(const Image *magick_unused(image),
  const NoiseType magick_unused(noise_type),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(noise_type);
  magick_unreferenced(exception);
  return((Image *) NULL);
}

MagickExport Image *AccelerateBlurImage(const Image *magick_unused(image),
  const double magick_unused(radius),const double magick_unused(sigma),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(radius);
  magick_unreferenced(sigma);
  magick_unreferenced(exception);

  return((Image *) NULL);
}

MagickExport MagickBooleanType AccelerateCompositeImage(
  Image *magick_unused(image),const CompositeOperator magick_unused(compose),
  const Image *magick_unused(composite),
  const float magick_unused(destination_dissolve),
  const float magick_unused(source_dissolve),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(compose);
  magick_unreferenced(composite);
  magick_unreferenced(destination_dissolve);
  magick_unreferenced(source_dissolve);
  magick_unreferenced(exception);

  return(MagickFalse);
}

MagickExport MagickBooleanType AccelerateContrastImage(
  Image* magick_unused(image),const MagickBooleanType magick_unused(sharpen),
  ExceptionInfo* magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(sharpen);
  magick_unreferenced(exception);

  return(MagickFalse);
}

MagickExport MagickBooleanType AccelerateContrastStretchImage(
  Image *magick_unused(image),const double magick_unused(black_point),
  const double magick_unused(white_point),
  ExceptionInfo* magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(black_point);
  magick_unreferenced(white_point);
  magick_unreferenced(exception);

  return(MagickFalse);
}

MagickExport Image *AccelerateConvolveImage(const Image *magick_unused(image),
  const KernelInfo *magick_unused(kernel),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(kernel);
  magick_unreferenced(exception);

  return((Image *) NULL);
}

MagickExport MagickBooleanType AccelerateEqualizeImage(
  Image* magick_unused(image),ExceptionInfo* magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(exception);

  return(MagickFalse);
}

MagickExport Image *AccelerateDespeckleImage(const Image* magick_unused(image),
  ExceptionInfo* magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(exception);

  return((Image *) NULL);
}

MagickExport MagickBooleanType AccelerateFunctionImage(
  Image *magick_unused(image),
  const MagickFunction magick_unused(function),
  const size_t magick_unused(number_parameters),
  const double *magick_unused(parameters),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(function);
  magick_unreferenced(number_parameters);
  magick_unreferenced(parameters);
  magick_unreferenced(exception);

  return(MagickFalse);
}

MagickExport MagickBooleanType AccelerateGrayscaleImage(
  Image *magick_unused(image),const PixelIntensityMethod magick_unused(method),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(method);
  magick_unreferenced(exception);

  return(MagickFalse);
}

MagickExport Image *AccelerateLocalContrastImage(
  const Image *magick_unused(image),const double magick_unused(radius),
  const double magick_unused(strength),ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(radius);
  magick_unreferenced(strength);
  magick_unreferenced(exception);

  return((Image *) NULL);
}

MagickExport MagickBooleanType AccelerateModulateImage(
  Image *magick_unused(image),const double magick_unused(percent_brightness),
  const double magick_unused(percent_hue),
  const double magick_unused(percent_saturation),
  ColorspaceType magick_unused(colorspace),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(percent_brightness);
  magick_unreferenced(percent_hue);
  magick_unreferenced(percent_saturation);
  magick_unreferenced(colorspace);
  magick_unreferenced(exception);

  return(MagickFalse);
}

MagickExport Image *AccelerateMotionBlurImage(
  const Image *magick_unused(image),const double *magick_unused(kernel),
  const size_t magick_unused(width),const OffsetInfo *magick_unused(offset),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(kernel);
  magick_unreferenced(width);
  magick_unreferenced(offset);
  magick_unreferenced(exception);

  return((Image *) NULL);
}

MagickExport MagickBooleanType AccelerateRandomImage(
  Image *magick_unused(image),ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(exception);

  return MagickFalse;
}

MagickExport Image *AccelerateResizeImage(const Image *magick_unused(image),
  const size_t magick_unused(resizedColumns),
  const size_t magick_unused(resizedRows),
  const ResizeFilter *magick_unused(resizeFilter),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(resizedColumns);
  magick_unreferenced(resizedRows);
  magick_unreferenced(resizeFilter);
  magick_unreferenced(exception);

  return((Image *) NULL);
}

MagickExport Image *AccelerateRotationalBlurImage(
  const Image *magick_unused(image),const double magick_unused(angle),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(angle);
  magick_unreferenced(exception);

  return((Image *) NULL);
}

MagickExport Image *AccelerateUnsharpMaskImage(
  const Image *magick_unused(image),const double magick_unused(radius),
  const double magick_unused(sigma),const double magick_unused(gain),
  const double magick_unused(threshold),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(radius);
  magick_unreferenced(sigma);
  magick_unreferenced(gain);
  magick_unreferenced(threshold);
  magick_unreferenced(exception);

  return((Image *) NULL);
}

MagickExport Image *AccelerateWaveletDenoiseImage(
  const Image *magick_unused(image),const double magick_unused(threshold),
  ExceptionInfo *magick_unused(exception))
{
  magick_unreferenced(image);
  magick_unreferenced(threshold);
  magick_unreferenced(exception);

  return((Image *)NULL);
}
#endif /* MAGICKCORE_OPENCL_SUPPORT */
