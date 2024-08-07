//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  https://therealmjp.github.io/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#pragma once

#include "..\\PCH.h"

namespace SampleFramework12
{

	struct MapResult
	{
		void* CPUAddress = nullptr;
		uint64 GPUAddress = 0;
		uint64 ResourceOffset = 0;
		ID3D12Resource* Resource = nullptr;
	};

	// 记录Upload的上下文
	struct UploadContext
	{
		ID3D12GraphicsCommandList* CmdList; // 使用哪个submission中的列表
		void* CPUAddress = nullptr; // submission在UploadRingBuffer中的指针
		uint64 ResourceOffset = 0; // submission在UploadRingBuffer的偏移量
		ID3D12Resource* Resource = nullptr; // 依赖的UploadRingBuffer
		void* Submission = nullptr; // submission本体的指针
	};

	namespace DX12
	{

		void Initialize_Upload();
		void Shutdown_Upload();

		void EndFrame_Upload();

		void Flush_Upload();

		// Resource upload/init
		UploadContext ResourceUploadBegin(uint64 size);
		void ResourceUploadEnd(UploadContext& context, bool syncOnGraphicsQueue = true);

		// Temporary CPU-writable buffer memory
		MapResult AcquireTempBufferMem(uint64 size, uint64 alignment);

		// Fast in-frame upload path through the copy queue
		void QueueFastUpload(ID3D12Resource* srcBuffer, uint64 srcOffset, ID3D12Resource* dstBuffer, uint64 dstOffset, uint64 copySize);

	}

}