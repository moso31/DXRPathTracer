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
	/*
		�����ϴ�ϵͳ��Ϊ���漸��ģ��

		UploadQueue��			�ϴ����У�������У�
		UploadSubmission(x16)��	������������顿�������б������������
		UploadRingBuffer��		���λ�����������������ڴ�ṹ�������ϴ�������ʵ���ǲ��ϵ���Buffer�����д���ݡ�
								ÿһ�ζ�д���ݶ�ʹ��һ����������顿�洢���ν����дBuffer���ĶΡ�
		UploadContext��			�ϴ������ģ�����ֱ�ӶԽӵײ�DXAPI����¼�����ĸ�����������������������б�����Buffer�е�ƫ�����Ƕ��١�

		�����ϲ㣬ÿ�ε��������ʱ�򣬻����Begin()/End()�������磬���봫һ������GPU�ϣ�
		ÿ���ϴ���Դʱ�������Begin()���Ȱ���ͷ���еȴ������������꣬Ȼ���ٿ�ʼ�µ�����
			������˵�����ж�UploadQueue GPU�ϵ�Fence�Ƿ� ���� �������FenceValue
			������ڣ�˵���������Ѿ���ɣ�������Buffer���ͷŶ�Ӧ���ڴ�������
		Ȼ������������ġ������б��������������������һ��������
		ͨ�������ĵ���DXAPI����initdata�����������ĵ�Buffer�ϣ�Ȼ���ִ�Buffer������texture�ϡ�
			Buffer�൱��һ���н飬���������ִ���ȥ��
		�������ʱ������End()
			UploadQueue�ύ�ղŵġ������б������Ҽ�¼������е�FenceValue���������ϡ�
	*/

	namespace DX12
	{

		// Wraps the copy queue that we will submit to when uploading resource data
		// ��DX12��������н����˰�װ
		// ÿһ�������Լ�����һ��Fence
		struct UploadQueue
		{
			ID3D12CommandQueue* CmdQueue = nullptr;
			Fence Fence;
			uint64 FenceValue = 0;
			uint64 WaitCount = 0;

			// ÿ��������ж��ǵ��̵߳ģ�������������
			SRWLOCK Lock = SRWLOCK_INIT;

			void Init(const wchar* name)
			{
				// ��ʼ���������...
				D3D12_COMMAND_QUEUE_DESC queueDesc = { };
				queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
				queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
				DXCall(Device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&CmdQueue)));
				CmdQueue->SetName(name);

				// ...��Χ��
				Fence.Init(0);
			}

			void Shutdown()
			{
				// ������Ҫ���е�ʱ��Ӧ�ý�Χ������Դ���ͷŵ�
				Fence.Shutdown();
				Release(CmdQueue);
			}

			void SyncDependentQueue(ID3D12CommandQueue* otherQueue)
			{
				AcquireSRWLockExclusive(&Lock);

				if (WaitCount > 0)
				{
					// ��֪һ��ָ����������У����ȹ��𣬵��ң���ǰ���У�ִ�����ټ�����������

					// ע��Ϊʲô��δ����ܱ�ʾ"��ǰ����ִ����"��
					// ���ٷ��ĵ���Wait()�Ľ��ͣ�https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12commandqueue-wait����
					// otherQueue ����������ȴ������Ҳ�ִ���κι�����
					// �� Fence �� GPU�е�ʵ�� GetCompletedValue() >= X ʱ��otherQueue �Ż����ִ�С�
					// ��ʲôʱ��Ż��� GetCompletedValue() >= X �أ����ǵ�ǰ���У�this����GPU����Ӧ��һ֡��Fence.Signal()��ʱ��
					otherQueue->Wait(Fence.D3DFence, FenceValue);

					WaitCount = 0;
				}

				ReleaseSRWLockExclusive(&Lock);
			}

			uint64 SubmitCmdList(ID3D12GraphicsCommandList* cmdList, bool syncOnDependentQueue)
			{
				// �ύ������У�����Χ�����+1
				AcquireSRWLockExclusive(&Lock);

				ID3D12CommandList* cmdLists[1] = { cmdList };
				CmdQueue->ExecuteCommandLists(1, cmdLists);

				const uint64 newFenceValue = ++FenceValue;
				Fence.Signal(CmdQueue, newFenceValue);

				// WaitCount����˼�����ȴ����
				// ����һ������SyncDependentQueue()���������оͻ��������𣬵ȴ���ǰ����ִ����
				if (syncOnDependentQueue)
					WaitCount += 1;

				ReleaseSRWLockExclusive(&Lock);

				return newFenceValue;
			}

			void Flush()
			{
				// �Եȴ����ڵ�ǰ����ִ����֮ǰ���Լ�����
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
			uint64 SubmissionUsed = 0; // �Ѿ�ʹ�õ�submission����

			// CPU-writable UPLOAD buffer
			uint64 BufferSize = 64 * 1024 * 1024;	// buffer���ܴ�С
			ID3D12Resource* Buffer = nullptr;	// buffer����
			uint8* BufferCPUAddr = nullptr;	// buffer��CPU��ַ

			// For using the UPLOAD buffer as a ring buffer
			uint64 BufferStart = 0;
			uint64 BufferUsed = 0;

			// Thread safety
			SRWLOCK Lock = SRWLOCK_INIT;

			// The queue for submitting on
			UploadQueue* submitQueue = nullptr;

			void Init(UploadQueue* queue)
			{
				// һ��UploadRingBuffer���ϴ����λ��壩���������ɸ�UploadSubmission���ϴ�������
				// ÿ��UploadSubmission������һ��CommandAllocator��һ��CommandList
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
				// �ͷŵ�ԭ���Ĳ�����һ���µ�
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

				// ����һ���ϴ���Buffer
				DXCall(Device->CreateCommittedResource(DX12::GetUploadHeapProps(), D3D12_HEAP_FLAG_NONE, &resourceDesc,
					D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&Buffer)));

				// ���ϴ���Buffer��GPU��ַӳ�䵽CPU��ַ
				// �����������ͨ�������ڴ������ֱ�Ӵ�cpu���޸�Buffer������
				D3D12_RANGE readRange = { };
				DXCall(Buffer->Map(0, &readRange, reinterpret_cast<void**>(&BufferCPUAddr)));
			}

			void ClearPendingUploads(uint64 waitCount)
			{
				// ��������еȴ���submission
				const uint64 start = SubmissionStart;
				const uint64 used = SubmissionUsed;
				for (uint64 i = 0; i < used; ++i)
				{
					const uint64 idx = (start + i) % MaxSubmissions;
					UploadSubmission& submission = Submissions[idx];
					Assert_(submission.Size > 0);
					Assert_(BufferUsed >= submission.Size);

					// ���һ��submission֮ǰGPU�ö�û�ù������õȴ�
					if (submission.FenceValue == uint64(-1))
						break; // ���²�submission�ǰ���˳�����ģ�����ֻҪ����һ��û�ù��ģ������Ҳ���õ��ˣ���

					// �Ӽܹ��Ͽ���submitFence��Ӧ��UploadRingBuffer�����ö��е�����Fence
					// ���������� UploadRingBuffer �����Fence��
					ID3D12Fence* submitFence = submitQueue->Fence.D3DFence;

					// ��δ���ʵ�ʲ����κ�����(hEvent == NULL) ��֪���������ɶ
					if (i < waitCount) 
						submitFence->SetEventOnCompletion(submission.FenceValue, NULL);

					// ���UploadRingBuffer��Fence��GPUʵ��ֵ��
					// �����ֵ���ڵ���submission��FenceValue��˵��ĳ��submission�Ѿ�����ˡ�
					if (submitFence->GetCompletedValue() >= submission.FenceValue)
					{
						// start+1��used-1
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
						// ��������ģ�
						// ���ǲ�ϣ���ύ��submission���ͷ�˳�������˳��һ�£���Ϊring buffer���߼��Ὣβ��λ����ǰ�ƶ������ǲ�����ring buffer���пն�����
						// ���ǣ�ֻҪ���ǰ�˳���ͷţ��ύ��˳��һ��Ӧ�û��ǿ��Եġ�
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
				// ����һ��submission
				Assert_(SubmissionUsed <= MaxSubmissions);
				if (SubmissionUsed == MaxSubmissions)
					return nullptr;

				// Ҫ������������Ϊ start + used
				// ��Ŀǰ����̫ȷ�����Ʋ⣺��ÿ��gpu�������ʱ��start+1��used-1����ÿ�η���ʱ��used+1��
				const uint64 submissionIdx = (SubmissionStart + SubmissionUsed) % MaxSubmissions;
				Assert_(Submissions[submissionIdx].Size == 0);

				// size = ����Ĵ�С
				Assert_(BufferUsed <= BufferSize);
				if (size > (BufferSize - BufferUsed))
					return nullptr;

				// ������buffer�Ϸ�����ڴ�Σ�Ϊstart~end
				const uint64 start = BufferStart;
				const uint64 end = BufferStart + BufferUsed;
				uint64 allocOffset = uint64(-1);
				uint64 padding = 0; // �ڱ��η���ĩ�˿ռ䲻��ʱ��ʹ��padding��¼ĩ��ʣ��Ŀռ��С��

				if (end < BufferSize) // ���end����buffer��Χ��
				{
					const uint64 endAmt = BufferSize - end; // end��bufferĩβ�ľ���
					if (endAmt >= size)
					{
						// case 1�����end��bufferĩβ�ľ����㹻�󣬾�ֱ�ӷ�����end��
						// allocOffset �����η������ʼλ��
						allocOffset = end;
					}
					else if (start >= size)
					{
						// case 2�����end��bufferĩβ�ľ��벻��������start��end�ľ����㹻�󣬾ͷ�����start��
						// allocOffset �����η������ʼλ��
						allocOffset = 0;
						BufferUsed += endAmt;
						padding = endAmt;
					}
				}
				else
				{
					// ���end�Ѿ�����buffer��Χ������Ҫ��start�����������Ŀռ䡾������һ�η���������Щ�ط���Ҫ��Ȼ��ringbuffer�ء�
					// allocOffset �����η������ʼλ��
					const uint64 wrappedEnd = end % BufferSize;
					if ((start - wrappedEnd) >= size)
						allocOffset = wrappedEnd;
				}

				if (allocOffset == uint64(-1))
					return nullptr;

				// ���ۣ�����1��submission��ʹ��ringbuffer�г���Ϊsize���ڴ�
				SubmissionUsed += 1;
				BufferUsed += size;

				// ��¼���η����submission��Ϣ
				UploadSubmission* submission = &Submissions[submissionIdx];
				submission->Offset = allocOffset; // ��ringbuffer�е���ʼλ��
				submission->Size = size; // ringbuffer�з���Ĵ�С
				submission->FenceValue = uint64(-1); 
				submission->Padding = padding; // ��¼��ringbuffer�з���Ĵ�С����ʱ��ʣ��Ŀռ��С

				return submission;
			}

			UploadContext Begin(uint64 size)
			{
				// ע�������������ÿ֡������ÿ�����ϴ���Դ�����ʱ��Żᴥ���������������������ȣ�
				// ÿ�ε�������������������һ��submission

				Assert_(Device != nullptr);

				Assert_(size > 0);
				size = AlignTo(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT); // 512�ֽڶ���

				// ���Ҫ������ڴ����buffer���ܴ�С���ͽ�buffer�д�ִ�е�����ȫ�����꣬Ȼ��buffer����
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

					// ������д�ִ�е�submission
					ClearPendingUploads(0);

					// ��buffer�Ϸ���һ��ռsize��С�ڴ��submission
					submission = AllocSubmission(size);
					while (submission == nullptr)
					{
						// ������䲻�ɹ�����մ�ִ��submission��
						// Ȼ��������Է��䣬ֱ���ɹ�Ϊֹ
						ClearPendingUploads(1);
						submission = AllocSubmission(size);
					}

					ReleaseSRWLockExclusive(&Lock);
				}

				// ����submission�������б����������
				DXCall(submission->CmdAllocator->Reset());
				DXCall(submission->CmdList->Reset(submission->CmdAllocator, nullptr));

				// ����һ��Upload������
				// ��¼submission��״̬��ʹ�����ĸ�CmdList����UploadRingBuffer�е���ʼλ�ú�ָ�롢Buffer������Լ���ָ��
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

				// ��submission���ʱ���رղ��ύ�����б�
				// Kick off the copy command
				DXCall(submission->CmdList->Close());
				submission->FenceValue = submitQueue->SubmitCmdList(submission->CmdList, syncOnDependentQueue);

				// ��ԭ״̬����ȷ���Ƿ��Ҫ��
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
			// ʹ��1�������б��n�������������n=��������������
			// �߼���Լ򵥣�ÿ֡���������ɸ���ҪFastUpload����Ϊ��
			// ��ʱͨ��QueueUpload()����������Щ��Ϊ�洢��Uploads[]�С�
			// ÿ֡����ʱ��ͨ��SubmitPending()��������Uploads[]�е���Ϊ�ύ��GPU�ϣ���ͨ����������ķ������Uploads[]��������ʵ���ڴ��ͷţ�
			ID3D12GraphicsCommandList5* CmdList = nullptr;
			ID3D12CommandAllocator* CmdAllocators[RenderLatency] = { };
			uint64 CmdAllocatorIdx = 0;

			static const uint64 MaxFastUploads = 256;
			FastUpload Uploads[MaxFastUploads];
			int64 NumUploads = 0;

			void Init()
			{
				// ��ʼ����ʱ�򣬳�ʼ��1�������б��n�����������
				// ���������б�󶨵�0�������������
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

			// �������������ʱ�򣬻�洢һ��FastUpload
			void QueueUpload(FastUpload upload)
			{
				const int64 idx = InterlockedIncrement64(&NumUploads) - 1;
				Assert_(idx < MaxFastUploads);
				Uploads[idx] = upload;
			}

			void SubmitPending(UploadQueue& queue)
			{
				// ÿ֡������ʱ�������ְ�����ύ���е�FastUpload
				if (NumUploads == 0)
					return;

				// ��ȡ��ǰ֡������������������������б��������б�󶨵�����ǰ֡���������������
				CmdAllocators[CmdAllocatorIdx]->Reset();
				CmdList->Reset(CmdAllocators[CmdAllocatorIdx], nullptr);

				// ��ȡ����Uploads�����ݣ����������ڴ�ο�����DstBuffer��
				for (int64 uploadIdx = 0; uploadIdx < NumUploads; ++uploadIdx)
				{
					const FastUpload& upload = Uploads[uploadIdx];
					CmdList->CopyBufferRegion(upload.DstBuffer, upload.DstOffset, upload.SrcBuffer, upload.SrcOffset, upload.CopySize);
				}

				CmdList->Close();

				// Ȼ���ύ�����б�
				queue.SubmitCmdList(CmdList, true);

				NumUploads = 0;
				CmdAllocatorIdx = (CmdAllocatorIdx + 1) % RenderLatency;
			}
		};

		static UploadQueue fastUploadQueue;
		static FastUploader fastUploader;

		void Initialize_Upload()
		{
			// ׼���ϴ���ص�һ���߼�
			uploadQueue.Init(L"Upload Queue");
			uploadRingBuffer.Init(&uploadQueue);

			fastUploadQueue.Init(L"Fast Upload Queue");
			fastUploader.Init();

			// ��ʱ�ڴ�
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

			// ��������CommittedResource��ÿ֡����ʹ��
			// ����CPU��GPU��ַ��¼������
			// �����룺������һ����Դ��ʱ�򣬻��Զ���CPU��GPU�������ڴ档��
			// ��ֻҪ������Ĵ���������ʹ�õ����ϴ��ѣ���ô�������ڴ��ַ������ͨ���������ʽ��TempFrameCPUMem��TempFrameGPUMem����ʱ�õ������һ�������CPU���޸ġ�
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
			// ÿ֡����ʱ��
			// ������е�upload������һ�������ġ������ϴ����У�fastUploadQueue�����ύ��Щupload��
			// Kick off any queued "fast" uploads
			fastUploader.SubmitPending(fastUploadQueue);

			// ��ա����λ��塿���еȴ���submission���Ʋ��Ǳ��ղ�����ʵ����ÿ���ϴ��������ʱ ����ͻ�������еȴ���submission��
			uploadRingBuffer.TryClearPending();

			// ������Ⱦ���С����̽���ȴ�״̬�����ڡ������ϴ�������ɡ�֮ǰ�����ȴ���
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
			// Ŀǰ�������ط����ã�1. Buffer��ʼ����ʱ��2. ������غ��ϴ���ʱ��
			// ��ʼʱ����
			return uploadRingBuffer.Begin(size);
		}

		void ResourceUploadEnd(UploadContext& context, bool syncOnGraphicsQueue)
		{
			// Ŀǰ�������ط����ã�1. Buffer��ʼ����ʱ��2. ������غ��ϴ���ʱ��
			// ����ʱ����
			// Begin��End֮�䣬ֻ��һ���£����ϴ���д����
			uploadRingBuffer.End(context, syncOnGraphicsQueue);
		}

		MapResult AcquireTempBufferMem(uint64 size, uint64 alignment)
		{
			// ����һ����ʱBuffer�ڴ�
			// ʹ�á�TempFrameUsed����¼��֡�ڣ����η��䣬���ڴ�ƫ������TempFrameUsed����֡ĩ���㣩
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