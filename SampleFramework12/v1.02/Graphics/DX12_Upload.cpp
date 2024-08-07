//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  https://therealmjp.github.io/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#include "PCH.h"

#include "DX12_Upload.h"
#include "DX12.h"
#include "GraphicsTypes.h"

namespace SampleFramework12
{

	namespace DX12
	{

		// Wraps the copy queue that we will submit to when uploading resource data
		// 对DX12的命令队列进行了包装
		// 每一个队列自己都有一个Fence
		struct UploadQueue
		{
			ID3D12CommandQueue* CmdQueue = nullptr;
			Fence Fence;
			uint64 FenceValue = 0;
			uint64 WaitCount = 0;

			// 每个命令队列都是单线程的，用锁保护起来
			SRWLOCK Lock = SRWLOCK_INIT;

			void Init(const wchar* name)
			{
				// 初始化命令队列...
				D3D12_COMMAND_QUEUE_DESC queueDesc = { };
				queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
				queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
				DXCall(Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&CmdQueue)));
				CmdQueue->SetName(name);

				// ...和围栏
				Fence.Init(0);
			}

			void Shutdown()
			{
				// 不在需要队列的时候，应该将围栏和资源都释放掉
				Fence.Shutdown();
				Release(CmdQueue);
			}

			void SyncDependentQueue(ID3D12CommandQueue* otherQueue)
			{
				AcquireSRWLockExclusive(&Lock);

				if (WaitCount > 0)
				{
					// 告知一个指定的命令队列，你先挂起，等我（当前队列）执行完再继续后续内容

					// 注：为什么这段代码能表示"当前队列执行完"：
					// 按官方文档对Wait()的解释（https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait），
					// otherQueue 会立即进入等待，并且不执行任何工作。
					// 当 Fence 在 GPU中的实际 GetCompletedValue() >= X 时，otherQueue 才会继续执行。
					// 那什么时候才会有 GetCompletedValue() >= X 呢？就是当前队列，this，在GPU上响应上一帧的Fence.Signal()的时候。
					otherQueue->Wait(Fence.D3DFence, FenceValue);

					WaitCount = 0;
				}

				ReleaseSRWLockExclusive(&Lock);
			}

			uint64 SubmitCmdList(ID3D12GraphicsCommandList* cmdList, bool syncOnDependentQueue)
			{
				// 提交命令队列，并且围栏标记+1
				AcquireSRWLockExclusive(&Lock);

				ID3D12CommandList* cmdLists[1] = { cmdList };
				CmdQueue->ExecuteCommandLists(1, cmdLists);

				const uint64 newFenceValue = ++FenceValue;
				Fence.Signal(CmdQueue, newFenceValue);

				// WaitCount的意思是做等待标记
				// 这样一旦调用SyncDependentQueue()，其他队列就会立即挂起，等待当前队列执行完
				if (syncOnDependentQueue)
					WaitCount += 1;

				ReleaseSRWLockExclusive(&Lock);

				return newFenceValue;
			}

			void Flush()
			{
				// 自等待，在当前队列执行完之前，自己挂起。
				AcquireSRWLockExclusive(&Lock);

				// Wait for all pending submissions on the queue to finish
				Fence.Wait(FenceValue);

				ReleaseSRWLockExclusive(&Lock);
			}
		};

		// A single submission that goes through the upload ring buffer
		struct UploadSubmission
		{
			ID3D12CommandAllocator* CmdAllocator = nullptr;
			ID3D12GraphicsCommandList5* CmdList = nullptr;
			uint64 Offset = 0;
			uint64 Size = 0;
			uint64 FenceValue = 0;
			uint64 Padding = 0;

			void Reset()
			{
				Offset = 0;
				Size = 0;
				FenceValue = 0;
				Padding = 0;
			}
		};

		struct UploadRingBuffer
		{
			// A ring buffer of pending submissions
			static const uint64 MaxSubmissions = 16;
			UploadSubmission Submissions[MaxSubmissions];
			uint64 SubmissionStart = 0;
			uint64 SubmissionUsed = 0; // 已经使用的submission数量

			// CPU-writable UPLOAD buffer
			uint64 BufferSize = 64 * 1024 * 1024;	// buffer的总大小
			ID3D12Resource* Buffer = nullptr;	// buffer主体
			uint8* BufferCPUAddr = nullptr;	// buffer的CPU地址

			// For using the UPLOAD buffer as a ring buffer
			uint64 BufferStart = 0;
			uint64 BufferUsed = 0;

			// Thread safety
			SRWLOCK Lock = SRWLOCK_INIT;

			// The queue for submitting on
			UploadQueue* submitQueue = nullptr;

			void Init(UploadQueue* queue)
			{
				// 一个UploadRingBuffer（上传环形缓冲）包含了若干个UploadSubmission（上传子任务）
				// 每个UploadSubmission包含了一个CommandAllocator和一个CommandList
				Assert_(queue != nullptr);
				submitQueue = queue;

				for (uint64 i = 0; i < MaxSubmissions; ++i) {
					UploadSubmission& submission = Submissions[i];
					DXCall(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&submission.CmdAllocator)));
					DXCall(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, submission.CmdAllocator, nullptr, IID_PPV_ARGS(&submission.CmdList)));
					DXCall(submission.CmdList->Close());

					submission.CmdList->SetName(L"Upload Command List");
				}

				Resize(BufferSize);
			}

			void Shutdown()
			{
				Release(Buffer);
				for (uint64 i = 0; i < MaxSubmissions; ++i) {
					Release(Submissions[i].CmdAllocator);
					Release(Submissions[i].CmdList);
				}
			}

			void Resize(uint64 newBufferSize)
			{
				// 释放掉原来的并分配一个新的
				Release(Buffer);

				BufferSize = newBufferSize;

				D3D12_RESOURCE_DESC resourceDesc = { };
				resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				resourceDesc.Width = BufferSize;
				resourceDesc.Height = 1;
				resourceDesc.DepthOrArraySize = 1;
				resourceDesc.MipLevels = 1;
				resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
				resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
				resourceDesc.SampleDesc.Count = 1;
				resourceDesc.SampleDesc.Quality = 0;
				resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				resourceDesc.Alignment = 0;

				// 创建一个上传堆Buffer
				DXCall(Device->CreateCommittedResource(DX12::GetUploadHeapProps(), D3D12_HEAP_FLAG_NONE, &resourceDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Buffer)));

				// 将上传堆Buffer的GPU地址映射到CPU地址
				// 其他代码可以通过各种内存操作，直接从cpu端修改Buffer的内容
				D3D12_RANGE readRange = { };
				DXCall(Buffer->Map(0, &readRange, reinterpret_cast<void**>(&BufferCPUAddr)));
			}

			void ClearPendingUploads(uint64 waitCount)
			{
				// 清理掉所有等待的submission
				const uint64 start = SubmissionStart;
				const uint64 used = SubmissionUsed;
				for (uint64 i = 0; i < used; ++i)
				{
					const uint64 idx = (start + i) % MaxSubmissions;
					UploadSubmission& submission = Submissions[idx];
					Assert_(submission.Size > 0);
					Assert_(BufferUsed >= submission.Size);

					// 如果一个submission之前GPU用都没用过，不用等待
					if (submission.FenceValue == uint64(-1))
						break; // 【猜测submission是按照顺序分配的？所以只要碰到一个没用过的，后面的也不用等了？】

					// 从架构上看，submitFence对应了UploadRingBuffer的内置队列的内置Fence
					// 可以理解成是 UploadRingBuffer 本身的Fence。
					ID3D12Fence* submitFence = submitQueue->Fence.D3DFence;

					// 这段代码实际不做任何事情(hEvent == NULL) 不知道作者想的啥
					if (i < waitCount) 
						submitFence->SetEventOnCompletion(submission.FenceValue, NULL);

					// 检测UploadRingBuffer的Fence的GPU实际值。
					// 如果该值大于等于submission的FenceValue，说明某个submission已经完成了。
					if (submitFence->GetCompletedValue() >= submission.FenceValue)
					{
						// start+1，used-1
						SubmissionStart = (SubmissionStart + 1) % MaxSubmissions;
						SubmissionUsed -= 1;

						BufferStart = (BufferStart + submission.Padding) % BufferSize;
						Assert_(submission.Offset == BufferStart);
						Assert_(BufferStart + submission.Size <= BufferSize);
						BufferStart = (BufferStart + submission.Size) % BufferSize;
						BufferUsed -= (submission.Size + submission.Padding);
						submission.Reset();

						if (BufferUsed == 0)
							BufferStart = 0;
					}
					else
					{
						// We don't want to retire our submissions out of allocation order, because
						// the ring buffer logic above will move the tail position forward (we don't
						// allow holes in the ring buffer). Submitting out-of-order should still be
						// ok though as long as we retire in-order.
						// 翻译成中文：
						// 我们不希望提交的submission的释放顺序与分配顺序不一致，因为ring buffer的逻辑会将尾部位置向前移动（我们不允许ring buffer中有空洞）。
						// 但是，只要我们按顺序释放，提交的顺序不一致应该还是可以的。
						break;
					}
				}
			}

			void Flush()
			{
				AcquireSRWLockExclusive(&Lock);

				while (SubmissionUsed > 0)
					ClearPendingUploads(uint64(-1));

				ReleaseSRWLockExclusive(&Lock);
			}

			void TryClearPending()
			{
				// See if we can clear out any pending submissions, but only if we can grab the lock
				if (TryAcquireSRWLockExclusive(&Lock))
				{
					ClearPendingUploads(0);

					ReleaseSRWLockExclusive(&Lock);
				}
			}

			UploadSubmission* AllocSubmission(uint64 size)
			{
				// 分配一个submission
				Assert_(SubmissionUsed <= MaxSubmissions);
				if (SubmissionUsed == MaxSubmissions)
					return nullptr;

				// 要分配的索引编号为 start + used
				// 【目前还不太确定，推测：在每次gpu队列完成时，start+1，used-1；在每次分配时，used+1】
				const uint64 submissionIdx = (SubmissionStart + SubmissionUsed) % MaxSubmissions;
				Assert_(Submissions[submissionIdx].Size == 0);

				// size = 分配的大小
				Assert_(BufferUsed <= BufferSize);
				if (size > (BufferSize - BufferUsed))
					return nullptr;

				// 本次在buffer上分配的内存段，为start~end
				const uint64 start = BufferStart;
				const uint64 end = BufferStart + BufferUsed;
				uint64 allocOffset = uint64(-1);
				uint64 padding = 0; // 在本次分配末端空间不足时，使用padding记录末端剩余的空间大小。

				if (end < BufferSize) // 如果end还在buffer范围内
				{
					const uint64 endAmt = BufferSize - end; // end到buffer末尾的距离
					if (endAmt >= size)
					{
						// case 1：如果end到buffer末尾的距离足够大，就直接分配在end处
						// allocOffset 代表本次分配的起始位置
						allocOffset = end;
					}
					else if (start >= size)
					{
						// case 2：如果end到buffer末尾的距离不够，但是start到end的距离足够大，就分配在start处
						// allocOffset 代表本次分配的起始位置
						allocOffset = 0;
						BufferUsed += endAmt;
						padding = endAmt;
					}
				}
				else
				{
					// 如果end已经超出buffer范围，则需要在start处留出超出的空间【估计上一次分配在了这些地方，要不然叫ringbuffer呢】
					// allocOffset 代表本次分配的起始位置
					const uint64 wrappedEnd = end % BufferSize;
					if ((start - wrappedEnd) >= size)
						allocOffset = wrappedEnd;
				}

				if (allocOffset == uint64(-1))
					return nullptr;

				// 结论：分配1个submission，使用ringbuffer中长度为size的内存
				SubmissionUsed += 1;
				BufferUsed += size;

				// 记录本次分配的submission信息
				UploadSubmission* submission = &Submissions[submissionIdx];
				submission->Offset = allocOffset; // 在ringbuffer中的起始位置
				submission->Size = size; // ringbuffer中分配的大小
				submission->FenceValue = uint64(-1); 
				submission->Padding = padding; // 记录在ringbuffer中分配的大小不足时，剩余的空间大小

				return submission;
			}

			UploadContext Begin(uint64 size)
			{
				Assert_(Device != nullptr);

				Assert_(size > 0);
				size = AlignTo(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT); // 512字节对齐

				// 如果要分配的内存大于buffer的总大小，就将buffer中待执行的任务全部做完，然后将buffer扩大
				if (size > BufferSize) 
				{
					// Resize the ring buffer so that it's big enough
					AcquireSRWLockExclusive(&Lock);

					while (SubmissionUsed > 0)
						ClearPendingUploads(uint64(-1));

					Resize(size);

					ReleaseSRWLockExclusive(&Lock);
				}

				UploadSubmission* submission = nullptr;

				{
					AcquireSRWLockExclusive(&Lock);

					// 【清空？】
					ClearPendingUploads(0);

					// 在buffer上分配一个占size大小内存的submission
					submission = AllocSubmission(size);
					while (submission == nullptr)
					{
						// 如果分配不成功，【清空？】，然后持续尝试分配，直到成功为止
						ClearPendingUploads(1);
						submission = AllocSubmission(size);
					}

					ReleaseSRWLockExclusive(&Lock);
				}

				// 启动submission的命令列表、命令分配器
				DXCall(submission->CmdAllocator->Reset());
				DXCall(submission->CmdList->Reset(submission->CmdAllocator, nullptr));

				// 创建一个Upload上下文
				// 记录submission的状态：使用了哪个CmdList、在UploadRingBuffer中的起始位置和指针、Buffer本体和自己的指针
				UploadContext context;
				context.CmdList = submission->CmdList;
				context.Resource = Buffer;
				context.CPUAddress = BufferCPUAddr + submission->Offset;
				context.ResourceOffset = submission->Offset;
				context.Submission = submission;

				return context;
			}

			void End(UploadContext& context, bool syncOnDependentQueue)
			{
				Assert_(context.CmdList != nullptr);
				Assert_(context.Submission != nullptr);
				UploadSubmission* submission = reinterpret_cast<UploadSubmission*>(context.Submission);

				// 当submission【完成？】时，关闭并提交命令列表
				// Kick off the copy command
				DXCall(submission->CmdList->Close());
				submission->FenceValue = submitQueue->SubmitCmdList(submission->CmdList, syncOnDependentQueue);

				// 还原状态（不确定是否必要）
				context = UploadContext();
			}
		};

		static UploadQueue uploadQueue;
		static UploadRingBuffer uploadRingBuffer;

		// Per-frame temporary upload buffer resources
		static const uint64 TempBufferSize = 2 * 1024 * 1024;
		static ID3D12Resource* TempFrameBuffers[RenderLatency] = { };
		static uint8* TempFrameCPUMem[RenderLatency] = { };
		static uint64 TempFrameGPUMem[RenderLatency] = { };
		static int64 TempFrameUsed = 0;

		// Resources for doing fast uploads while generating render commands
		struct FastUpload
		{
			ID3D12Resource* SrcBuffer = nullptr;
			uint64 SrcOffset = 0;
			ID3D12Resource* DstBuffer = nullptr;
			uint64 DstOffset = 0;
			uint64 CopySize = 0;
		};

		struct FastUploader
		{
			ID3D12GraphicsCommandList5* CmdList = nullptr;
			ID3D12CommandAllocator* CmdAllocators[RenderLatency] = { };
			uint64 CmdAllocatorIdx = 0;

			static const uint64 MaxFastUploads = 256;
			FastUpload Uploads[MaxFastUploads];
			int64 NumUploads = 0;

			void Init()
			{
				CmdAllocatorIdx = 0;

				for (uint32 i = 0; i < RenderLatency; ++i)
				{
					DXCall(Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&CmdAllocators[i])));
					CmdAllocators[i]->SetName(L"Fast Uploader Command Allocator");
				}

				DXCall(Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, CmdAllocators[0], nullptr, IID_PPV_ARGS(&CmdList)));
				DXCall(CmdList->Close());
				CmdList->SetName(L"Fast Upload Command List");
			}

			void Shutdown()
			{
				Assert_(NumUploads == 0);
				for (uint32 i = 0; i < RenderLatency; ++i)
					Release(CmdAllocators[i]);
				Release(CmdList);
			}

			void QueueUpload(FastUpload upload)
			{
				const int64 idx = InterlockedIncrement64(&NumUploads) - 1;
				Assert_(idx < MaxFastUploads);
				Uploads[idx] = upload;
			}

			void SubmitPending(UploadQueue& queue)
			{
				if (NumUploads == 0)
					return;

				CmdAllocators[CmdAllocatorIdx]->Reset();
				CmdList->Reset(CmdAllocators[CmdAllocatorIdx], nullptr);

				for (int64 uploadIdx = 0; uploadIdx < NumUploads; ++uploadIdx)
				{
					const FastUpload& upload = Uploads[uploadIdx];
					CmdList->CopyBufferRegion(upload.DstBuffer, upload.DstOffset, upload.SrcBuffer, upload.SrcOffset, upload.CopySize);
				}

				CmdList->Close();

				queue.SubmitCmdList(CmdList, true);

				NumUploads = 0;
				CmdAllocatorIdx = (CmdAllocatorIdx + 1) % RenderLatency;
			}
		};

		static UploadQueue fastUploadQueue;
		static FastUploader fastUploader;

		void Initialize_Upload()
		{
			uploadQueue.Init(L"Upload Queue");
			uploadRingBuffer.Init(&uploadQueue);

			fastUploadQueue.Init(L"Fast Upload Queue");
			fastUploader.Init();

			// Temporary buffer memory that swaps every frame
			D3D12_RESOURCE_DESC resourceDesc = { };
			resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			resourceDesc.Width = uint32(TempBufferSize);
			resourceDesc.Height = 1;
			resourceDesc.DepthOrArraySize = 1;
			resourceDesc.MipLevels = 1;
			resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
			resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
			resourceDesc.SampleDesc.Count = 1;
			resourceDesc.SampleDesc.Quality = 0;
			resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			resourceDesc.Alignment = 0;

			for (uint64 i = 0; i < RenderLatency; ++i)
			{
				DXCall(Device->CreateCommittedResource(DX12::GetUploadHeapProps(), D3D12_HEAP_FLAG_NONE, &resourceDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&TempFrameBuffers[i])));

				D3D12_RANGE readRange = { };
				DXCall(TempFrameBuffers[i]->Map(0, &readRange, reinterpret_cast<void**>(&TempFrameCPUMem[i])));
				TempFrameGPUMem[i] = TempFrameBuffers[i]->GetGPUVirtualAddress();
			}
		}

		void Shutdown_Upload()
		{
			uploadQueue.Shutdown();
			uploadRingBuffer.Shutdown();
			fastUploader.Shutdown();
			fastUploadQueue.Shutdown();

			for (uint64 i = 0; i < ArraySize_(TempFrameBuffers); ++i)
				Release(TempFrameBuffers[i]);
		}

		void EndFrame_Upload()
		{
			// Kick off any queued "fast" uploads
			fastUploader.SubmitPending(fastUploadQueue);

			uploadRingBuffer.TryClearPending();

			// Make sure that the graphics queue waits for any pending uploads that have been submitted.
			uploadQueue.SyncDependentQueue(GfxQueue);
			fastUploadQueue.SyncDependentQueue(GfxQueue);

			TempFrameUsed = 0;
		}

		void Flush_Upload()
		{
			uploadQueue.Flush();
			uploadRingBuffer.Flush();
			fastUploadQueue.Flush();
		}

		UploadContext ResourceUploadBegin(uint64 size)
		{
			// 目前有两个地方调用：1. Buffer初始化的时候；2. 纹理加载和上传的时候
			// 开始时调用
			return uploadRingBuffer.Begin(size);
		}

		void ResourceUploadEnd(UploadContext& context, bool syncOnGraphicsQueue)
		{
			// 目前有两个地方调用：1. Buffer初始化的时候；2. 纹理加载和上传的时候
			// 结束时调用
			uploadRingBuffer.End(context, syncOnGraphicsQueue);
		}

		MapResult AcquireTempBufferMem(uint64 size, uint64 alignment)
		{
			uint64 allocSize = size + alignment;
			uint64 offset = InterlockedAdd64(&TempFrameUsed, allocSize) - allocSize;
			if (alignment > 0)
				offset = AlignTo(offset, alignment);
			Assert_(offset + size <= TempBufferSize);

			MapResult result;
			result.CPUAddress = TempFrameCPUMem[CurrFrameIdx] + offset;
			result.GPUAddress = TempFrameGPUMem[CurrFrameIdx] + offset;
			result.ResourceOffset = offset;
			result.Resource = TempFrameBuffers[CurrFrameIdx];

			return result;
		}

		void QueueFastUpload(ID3D12Resource* srcBuffer, uint64 srcOffset, ID3D12Resource* dstBuffer, uint64 dstOffset, uint64 copySize)
		{
			FastUpload upload = { .SrcBuffer = srcBuffer, .SrcOffset = srcOffset, .DstBuffer = dstBuffer, .DstOffset = dstOffset, .CopySize = copySize };
			fastUploader.QueueUpload(upload);
		}

	} // namespace DX12

} // namespace SampleFramework12