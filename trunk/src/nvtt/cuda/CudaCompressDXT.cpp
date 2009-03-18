// Copyright NVIDIA Corporation 2007 -- Ignacio Castano <icastano@nvidia.com>
// 
// Permission is hereby granted, free of charge, to any person
// obtaining a copy of this software and associated documentation
// files (the "Software"), to deal in the Software without
// restriction, including without limitation the rights to use,
// copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following
// conditions:
// 
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
// OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
// HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
// WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.

#include <nvcore/Debug.h>
#include <nvcore/Containers.h>
#include <nvmath/Color.h>
#include <nvimage/Image.h>
#include <nvimage/ColorBlock.h>
#include <nvimage/BlockDXT.h>
#include <nvtt/CompressionOptions.h>
#include <nvtt/OutputOptions.h>
#include <nvtt/QuickCompressDXT.h>
#include <nvtt/OptimalCompressDXT.h>

#include "CudaCompressDXT.h"
#include "CudaUtils.h"


#if defined HAVE_CUDA
#include <cuda_runtime_api.h>
#endif

#include <time.h>
#include <stdio.h>

using namespace nv;
using namespace nvtt;

#if defined HAVE_CUDA

#define MAX_BLOCKS 8192U // 32768, 65535


extern "C" void setupCompressKernel(const float weights[3]);
extern "C" void compressKernelDXT1(uint blockNum, uint * d_data, uint * d_result, uint * d_bitmaps);
extern "C" void compressKernelDXT1_Tex(uint bn, uint blockNum, uint w, cudaArray * d_data, uint * d_result, uint * d_bitmaps);
extern "C" void compressKernelDXT1_Level4(uint blockNum, uint * d_data, uint * d_result, uint * d_bitmaps);
extern "C" void compressWeightedKernelDXT1(uint blockNum, uint * d_data, uint * d_result, uint * d_bitmaps);
extern "C" void compressNormalKernelDXT1(uint blockNum, uint * d_data, uint * d_result, uint * d_bitmaps);
extern "C" void compressKernelCTX1(uint blockNum, uint * d_data, uint * d_result, uint * d_bitmaps);


#include "Bitmaps.h"	// @@ Rename to BitmapTable.h

// Convert linear image to block linear.
static void convertToBlockLinear(const Image * image, uint * blockLinearImage)
{
	const uint w = (image->width() + 3) / 4;
	const uint h = (image->height() + 3) / 4;

	for(uint by = 0; by < h; by++) {
		for(uint bx = 0; bx < w; bx++) {
			const uint bw = min(image->width() - bx * 4, 4U);
			const uint bh = min(image->height() - by * 4, 4U);

			for (uint i = 0; i < 16; i++) {
				const int x = (i % 4) % bw;
				const int y = (i / 4) % bh;
				blockLinearImage[(by * w + bx) * 16 + i] = image->pixel(bx * 4 + x, by * 4 + y).u;
			}
		}
	}
}

#endif


CudaCompressor::CudaCompressor() : m_bitmapTable(NULL), m_bitmapTableCTX(NULL), m_data(NULL), m_result(NULL)
{
#if defined HAVE_CUDA
    // Allocate and upload bitmaps.
    cudaMalloc((void**) &m_bitmapTable, 992 * sizeof(uint));
	if (m_bitmapTable != NULL)
	{
		cudaMemcpy(m_bitmapTable, s_bitmapTable, 992 * sizeof(uint), cudaMemcpyHostToDevice);
	}

    cudaMalloc((void**) &m_bitmapTableCTX, 704 * sizeof(uint));

	if (m_bitmapTableCTX != NULL)
	{
		cudaMemcpy(m_bitmapTableCTX, s_bitmapTableCTX, 704 * sizeof(uint), cudaMemcpyHostToDevice);
	}

	// Allocate scratch buffers.
    cudaMalloc((void**) &m_data, MAX_BLOCKS * 64U);
    cudaMalloc((void**) &m_result, MAX_BLOCKS * 8U);
#endif
}

CudaCompressor::~CudaCompressor()
{
#if defined HAVE_CUDA
	// Free device mem allocations.
	cudaFree(m_data);
	cudaFree(m_result);
	cudaFree(m_bitmapTable);
	cudaFree(m_bitmapTableCTX);
#endif
}

bool CudaCompressor::isValid() const
{
#if defined HAVE_CUDA
	cudaError_t err = cudaGetLastError();
	if (err != cudaSuccess)
   	{
		nvDebug("*** CUDA Error: %s\n", cudaGetErrorString(err));
		return false;
	}
#endif
	return m_data != NULL && m_result != NULL && m_bitmapTable != NULL;
}

// @@ This code is very repetitive and needs to be cleaned up.

#if 0

struct CudaCompressionKernel
{
	virtual void setup(const CompressionOptions::Private & compressionOptions)
	{
		setupCompressKernel(compressionOptions.colorWeight.ptr());
	}
	
	virtual void setBitmapTable();
	
	virtual void runDeviceCode(int count);
	
	virtual void runHostCode(int count);
	
};

void CudaCompressor::compressKernel(CudaCompressionKernel * kernel)
{
	nvDebugCheck(cuda::isHardwarePresent());
#if defined HAVE_CUDA

	// Image size in blocks.
	const uint w = (image->width() + 3) / 4;
	const uint h = (image->height() + 3) / 4;

	uint imageSize = w * h * 16 * sizeof(Color32);
    uint * blockLinearImage = (uint *) malloc(imageSize);
	convertToBlockLinear(image, blockLinearImage);	// @@ Do this in parallel with the GPU, or in the GPU!

	const uint blockNum = w * h;
	const uint compressedSize = blockNum * 8;

	clock_t start = clock();

	kernel->setup(compressionOptions);
	kernel->setBitmapTable(m_bitmapTable);
	
	// TODO: Add support for multiple GPUs.
	uint bn = 0;
	while(bn != blockNum)
	{
		uint count = min(blockNum - bn, MAX_BLOCKS);

	    cudaMemcpy(m_data, blockLinearImage + bn * 16, count * 64, cudaMemcpyHostToDevice);

		kernel->runDeviceCode(count, m_data, m_result);

		kernel->runHostCode(count);
		
		// Check for errors.
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess)
		{
			nvDebug("CUDA Error: %s\n", cudaGetErrorString(err));

			if (outputOptions.errorHandler != NULL)
			{
				outputOptions.errorHandler->error(Error_CudaError);
			}
		}

		// Copy result to host, overwrite swizzled image.
		cudaMemcpy(blockLinearImage, m_result, count * 8, cudaMemcpyDeviceToHost);

		// Output result.
		kernel->outputResult(outputOptions.outputHandler);
		
		if (outputOptions.outputHandler != NULL)
		{
			outputOptions.outputHandler->writeData(blockLinearImage, count * 8);
		}

		bn += count;
	}

	clock_t end = clock();
	//printf("\rCUDA time taken: %.3f seconds\n", float(end-start) / CLOCKS_PER_SEC);

	free(blockLinearImage);

#else
	if (outputOptions.errorHandler != NULL)
	{
		outputOptions.errorHandler->error(Error_CudaError);
	}
#endif
}

#endif // 0


void CudaCompressor::setImage(const Image * image, nvtt::AlphaMode alphaMode)
{
	m_image = image;
	m_alphaMode = alphaMode;
}



/// Compress image using CUDA.
void CudaCompressor::compressDXT1(const CompressionOptions::Private & compressionOptions, const OutputOptions::Private & outputOptions)
{
	nvDebugCheck(cuda::isHardwarePresent());
#if defined HAVE_CUDA

	// Image size in blocks.
	const uint w = (m_image->width() + 3) / 4;
	const uint h = (m_image->height() + 3) / 4;

	uint imageSize = w * h * 16 * sizeof(Color32);
    uint * blockLinearImage = (uint *) malloc(imageSize);
	convertToBlockLinear(m_image, blockLinearImage);	// @@ Do this in parallel with the GPU, or in the GPU!

	const uint blockNum = w * h;
	const uint compressedSize = blockNum * 8;

	clock_t start = clock();

	setupCompressKernel(compressionOptions.colorWeight.ptr());
	
	// TODO: Add support for multiple GPUs.
	uint bn = 0;
	while(bn != blockNum)
	{
		uint count = min(blockNum - bn, MAX_BLOCKS);

	    cudaMemcpy(m_data, blockLinearImage + bn * 16, count * 64, cudaMemcpyHostToDevice);

		// Launch kernel.
		compressKernelDXT1(count, m_data, m_result, m_bitmapTable);

		// Check for errors.
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess)
		{
			nvDebug("CUDA Error: %s\n", cudaGetErrorString(err));

			if (outputOptions.errorHandler != NULL)
			{
				outputOptions.errorHandler->error(Error_CudaError);
			}
		}

		// Copy result to host, overwrite swizzled image.
		cudaMemcpy(blockLinearImage, m_result, count * 8, cudaMemcpyDeviceToHost);

		// Output result.
		if (outputOptions.outputHandler != NULL)
		{
			outputOptions.outputHandler->writeData(blockLinearImage, count * 8);
		}

		bn += count;
	}

	clock_t end = clock();
	//printf("\rCUDA time taken: %.3f seconds\n", float(end-start) / CLOCKS_PER_SEC);

	free(blockLinearImage);

#else
	if (outputOptions.errorHandler != NULL)
	{
		outputOptions.errorHandler->error(Error_CudaError);
	}
#endif
}

void CudaCompressor::compressDXT1_Tex(const CompressionOptions::Private & compressionOptions, const OutputOptions::Private & outputOptions)
{
	nvDebugCheck(cuda::isHardwarePresent());
#if defined HAVE_CUDA

    // Allocate image as a cuda array.
    cudaChannelFormatDesc channelDesc = cudaCreateChannelDesc(8, 8, 8, 8, cudaChannelFormatKindUnsigned);

    cudaArray * d_image;
	const int imageSize = m_image->width() * m_image->height() * sizeof(uint);
    cudaMallocArray(&d_image, &channelDesc, m_image->width(), m_image->height()); 
    cudaMemcpyToArray(d_image, 0, 0, m_image->pixels(), imageSize, cudaMemcpyHostToDevice);

	// Image size in blocks.
	const uint w = (m_image->width() + 3) / 4;
	const uint h = (m_image->height() + 3) / 4;
	const uint blockNum = w * h;
	const uint compressedSize = blockNum * 8;

	void * h_result = malloc(MAX_BLOCKS * 8);

	clock_t start = clock();

	setupCompressKernel(compressionOptions.colorWeight.ptr());
	
	uint bn = 0;
	while(bn != blockNum)
	{
		uint count = min(blockNum - bn, MAX_BLOCKS);

		// Launch kernel.
		compressKernelDXT1_Tex(bn, count, w, d_image, m_result, m_bitmapTable);

		// Check for errors.
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess)
		{
			nvDebug("CUDA Error: %s\n", cudaGetErrorString(err));

			if (outputOptions.errorHandler != NULL)
			{
				outputOptions.errorHandler->error(Error_CudaError);
			}
		}

		// Copy result to host, overwrite swizzled image.
		cudaMemcpy(h_result, m_result, count * 8, cudaMemcpyDeviceToHost);

		// Output result.
		if (outputOptions.outputHandler != NULL)
		{
			outputOptions.outputHandler->writeData(h_result, count * 8);
		}

		bn += count;
	}

	clock_t end = clock();
	//printf("\rCUDA time taken: %.3f seconds\n", float(end-start) / CLOCKS_PER_SEC);

	free(h_result);

#else
	if (outputOptions.errorHandler != NULL)
	{
		outputOptions.errorHandler->error(Error_CudaError);
	}
#endif
}



/// Compress image using CUDA.
void CudaCompressor::compressDXT3(const CompressionOptions::Private & compressionOptions, const OutputOptions::Private & outputOptions)
{
	nvDebugCheck(cuda::isHardwarePresent());
#if defined HAVE_CUDA

	// Image size in blocks.
	const uint w = (m_image->width() + 3) / 4;
	const uint h = (m_image->height() + 3) / 4;

	uint imageSize = w * h * 16 * sizeof(Color32);
    uint * blockLinearImage = (uint *) malloc(imageSize);
	convertToBlockLinear(m_image, blockLinearImage);

	const uint blockNum = w * h;
	const uint compressedSize = blockNum * 8;

	AlphaBlockDXT3 * alphaBlocks = NULL;
	alphaBlocks = (AlphaBlockDXT3 *)malloc(min(compressedSize, MAX_BLOCKS * 8U));

	setupCompressKernel(compressionOptions.colorWeight.ptr());
	
	clock_t start = clock();

	uint bn = 0;
	while(bn != blockNum)
	{
		uint count = min(blockNum - bn, MAX_BLOCKS);

	    cudaMemcpy(m_data, blockLinearImage + bn * 16, count * 64, cudaMemcpyHostToDevice);

		// Launch kernel.
		if (m_alphaMode == AlphaMode_Transparency)
		{
			compressWeightedKernelDXT1(count, m_data, m_result, m_bitmapTable);
		}
		else
		{
			compressKernelDXT1_Level4(count, m_data, m_result, m_bitmapTable);
		}

		// Compress alpha in parallel with the GPU.
		for (uint i = 0; i < count; i++)
		{
			ColorBlock rgba(blockLinearImage + (bn + i) * 16);
			OptimalCompress::compressDXT3A(rgba, alphaBlocks + i);
		}

		// Check for errors.
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess)
		{
			nvDebug("CUDA Error: %s\n", cudaGetErrorString(err));

			if (outputOptions.errorHandler != NULL)
			{
				outputOptions.errorHandler->error(Error_CudaError);
			}
		}

		// Copy result to host, overwrite swizzled image.
		cudaMemcpy(blockLinearImage, m_result, count * 8, cudaMemcpyDeviceToHost);

		// Output result.
		if (outputOptions.outputHandler != NULL)
		{
			for (uint i = 0; i < count; i++)
			{
				outputOptions.outputHandler->writeData(alphaBlocks + i, 8);
				outputOptions.outputHandler->writeData(blockLinearImage + i * 2, 8);
			}
		}

		bn += count;
	}

	clock_t end = clock();
	//printf("\rCUDA time taken: %.3f seconds\n", float(end-start) / CLOCKS_PER_SEC);

	free(alphaBlocks);
	free(blockLinearImage);

#else
	if (outputOptions.errorHandler != NULL)
	{
		outputOptions.errorHandler->error(Error_CudaError);
	}
#endif
}


/// Compress image using CUDA.
void CudaCompressor::compressDXT5(const CompressionOptions::Private & compressionOptions, const OutputOptions::Private & outputOptions)
{
	nvDebugCheck(cuda::isHardwarePresent());
#if defined HAVE_CUDA

	// Image size in blocks.
	const uint w = (m_image->width() + 3) / 4;
	const uint h = (m_image->height() + 3) / 4;

	uint imageSize = w * h * 16 * sizeof(Color32);
    uint * blockLinearImage = (uint *) malloc(imageSize);
	convertToBlockLinear(m_image, blockLinearImage);

	const uint blockNum = w * h;
	const uint compressedSize = blockNum * 8;

	AlphaBlockDXT5 * alphaBlocks = NULL;
	alphaBlocks = (AlphaBlockDXT5 *)malloc(min(compressedSize, MAX_BLOCKS * 8U));

	setupCompressKernel(compressionOptions.colorWeight.ptr());
	
	clock_t start = clock();

	uint bn = 0;
	while(bn != blockNum)
	{
		uint count = min(blockNum - bn, MAX_BLOCKS);

	    cudaMemcpy(m_data, blockLinearImage + bn * 16, count * 64, cudaMemcpyHostToDevice);

		// Launch kernel.
		if (m_alphaMode == AlphaMode_Transparency)
		{
			compressWeightedKernelDXT1(count, m_data, m_result, m_bitmapTable);
		}
		else
		{
			compressKernelDXT1_Level4(count, m_data, m_result, m_bitmapTable);
		}

		// Compress alpha in parallel with the GPU.
		for (uint i = 0; i < count; i++)
		{
			ColorBlock rgba(blockLinearImage + (bn + i) * 16);
			QuickCompress::compressDXT5A(rgba, alphaBlocks + i);
		}

		// Check for errors.
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess)
		{
			nvDebug("CUDA Error: %s\n", cudaGetErrorString(err));

			if (outputOptions.errorHandler != NULL)
			{
				outputOptions.errorHandler->error(Error_CudaError);
			}
		}

		// Copy result to host, overwrite swizzled image.
		cudaMemcpy(blockLinearImage, m_result, count * 8, cudaMemcpyDeviceToHost);

		// Output result.
		if (outputOptions.outputHandler != NULL)
		{
			for (uint i = 0; i < count; i++)
			{
				outputOptions.outputHandler->writeData(alphaBlocks + i, 8);
				outputOptions.outputHandler->writeData(blockLinearImage + i * 2, 8);
			}
		}

		bn += count;
	}

	clock_t end = clock();
	//printf("\rCUDA time taken: %.3f seconds\n", float(end-start) / CLOCKS_PER_SEC);

	free(alphaBlocks);
	free(blockLinearImage);

#else
	if (outputOptions.errorHandler != NULL)
	{
		outputOptions.errorHandler->error(Error_CudaError);
	}
#endif
}


void CudaCompressor::compressDXT1n(const nvtt::CompressionOptions::Private & compressionOptions, const nvtt::OutputOptions::Private & outputOptions)
{
	nvDebugCheck(cuda::isHardwarePresent());
#if defined HAVE_CUDA

	// Image size in blocks.
	const uint w = (m_image->width() + 3) / 4;
	const uint h = (m_image->height() + 3) / 4;

	uint imageSize = w * h * 16 * sizeof(Color32);
    uint * blockLinearImage = (uint *) malloc(imageSize);
	convertToBlockLinear(m_image, blockLinearImage);	// @@ Do this in parallel with the GPU, or in the GPU!

	const uint blockNum = w * h;
	const uint compressedSize = blockNum * 8;

	clock_t start = clock();

	setupCompressKernel(compressionOptions.colorWeight.ptr());
	
	// TODO: Add support for multiple GPUs.
	uint bn = 0;
	while(bn != blockNum)
	{
		uint count = min(blockNum - bn, MAX_BLOCKS);

	    cudaMemcpy(m_data, blockLinearImage + bn * 16, count * 64, cudaMemcpyHostToDevice);

		// Launch kernel.
		compressNormalKernelDXT1(count, m_data, m_result, m_bitmapTable);

		// Check for errors.
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess)
		{
			nvDebug("CUDA Error: %s\n", cudaGetErrorString(err));

			if (outputOptions.errorHandler != NULL)
			{
				outputOptions.errorHandler->error(Error_CudaError);
			}
		}

		// Copy result to host, overwrite swizzled image.
		cudaMemcpy(blockLinearImage, m_result, count * 8, cudaMemcpyDeviceToHost);

		// Output result.
		if (outputOptions.outputHandler != NULL)
		{
			outputOptions.outputHandler->writeData(blockLinearImage, count * 8);
		}

		bn += count;
	}

	clock_t end = clock();
	//printf("\rCUDA time taken: %.3f seconds\n", float(end-start) / CLOCKS_PER_SEC);

	free(blockLinearImage);

#else
	if (outputOptions.errorHandler != NULL)
	{
		outputOptions.errorHandler->error(Error_CudaError);
	}
#endif
}


void CudaCompressor::compressCTX1(const nvtt::CompressionOptions::Private & compressionOptions, const nvtt::OutputOptions::Private & outputOptions)
{
	nvDebugCheck(cuda::isHardwarePresent());
#if defined HAVE_CUDA

	// Image size in blocks.
	const uint w = (m_image->width() + 3) / 4;
	const uint h = (m_image->height() + 3) / 4;

	uint imageSize = w * h * 16 * sizeof(Color32);
    uint * blockLinearImage = (uint *) malloc(imageSize);
	convertToBlockLinear(m_image, blockLinearImage);	// @@ Do this in parallel with the GPU, or in the GPU!

	const uint blockNum = w * h;
	const uint compressedSize = blockNum * 8;

	clock_t start = clock();

	setupCompressKernel(compressionOptions.colorWeight.ptr());
	
	// TODO: Add support for multiple GPUs.
	uint bn = 0;
	while(bn != blockNum)
	{
		uint count = min(blockNum - bn, MAX_BLOCKS);

	    cudaMemcpy(m_data, blockLinearImage + bn * 16, count * 64, cudaMemcpyHostToDevice);

		// Launch kernel.
		compressKernelCTX1(count, m_data, m_result, m_bitmapTableCTX);

		// Check for errors.
		cudaError_t err = cudaGetLastError();
		if (err != cudaSuccess)
		{
			nvDebug("CUDA Error: %s\n", cudaGetErrorString(err));

			if (outputOptions.errorHandler != NULL)
			{
				outputOptions.errorHandler->error(Error_CudaError);
			}
		}

		// Copy result to host, overwrite swizzled image.
		cudaMemcpy(blockLinearImage, m_result, count * 8, cudaMemcpyDeviceToHost);

		// Output result.
		if (outputOptions.outputHandler != NULL)
		{
			outputOptions.outputHandler->writeData(blockLinearImage, count * 8);
		}

		bn += count;
	}

	clock_t end = clock();
	//printf("\rCUDA time taken: %.3f seconds\n", float(end-start) / CLOCKS_PER_SEC);

	free(blockLinearImage);

#else
	if (outputOptions.errorHandler != NULL)
	{
		outputOptions.errorHandler->error(Error_CudaError);
	}
#endif
}


void CudaCompressor::compressDXT5n(const nvtt::CompressionOptions::Private & compressionOptions, const nvtt::OutputOptions::Private & outputOptions)
{
	nvDebugCheck(cuda::isHardwarePresent());
#if defined HAVE_CUDA

	// @@ TODO

#else
	if (outputOptions.errorHandler != NULL)
	{
		outputOptions.errorHandler->error(Error_CudaError);
	}
#endif
}


