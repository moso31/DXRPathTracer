//=================================================================================================
//
//  MJP's DX12 Sample Framework
//  http://mynameismjp.wordpress.com/
//
//  All code licensed under the MIT license
//
//=================================================================================================

#pragma once

#include "..\\PCH.h"

#include "..\\InterfacePointers.h"
#include "..\\Utility.h"
#include "..\\Containers.h"
#include "DX12.h"
#include "DX12_Upload.h"
#include "DX12_Helpers.h"

namespace SampleFramework12
{

	// 记录分配的描述符（持续内存）
	// 1. CPUHandle
	// 2. 在堆上的对应索引
	struct PersistentDescriptorAlloc
	{
		D3D12_CPU_DESCRIPTOR_HANDLE Handles[DX12::RenderLatency] = { };
		uint32 Index = uint32(-1);
	};

    // 临时内存的职责是，存储当前帧使用的ShaderVisible的Heap的CPUHandle和GPUHandle
	// 记录分配的描述符（临时内存）
	struct TempDescriptorAlloc
	{
		D3D12_CPU_DESCRIPTOR_HANDLE StartCPUHandle = { };
		D3D12_GPU_DESCRIPTOR_HANDLE StartGPUHandle = { };
		uint32 StartIndex = uint32(-1);
	};

	// 此类对DX12描述符堆做了包装
	// 支持持久和临时分配的D3D12描述符堆
	// 分为两个部分：【持续内存区】和【临时内存区】
	struct DescriptorHeap
	{
		ID3D12DescriptorHeap* Heaps[DX12::RenderLatency] = { };
		uint32 NumPersistent = 0;

		// 死亡列表：记录"未使用的索引"
		// 默认foreach DeadList[i] = i
		// 但有释放行为时，会优先在上次分配的索引补回释放的索引。所以不一定总是顺序，可能构成乱序。
		// 例如：
		// DeadList：0 1 2 3 4 5 6 7 8...（初始）
		// DeadList：- - - - - 5 6 7 8...（分配了0-4）
		// DeadList：- - - - 3 5 6 7 8...（释放了3）
		// DeadList：- - - 1 3 5 6 7 8...（释放了1）
		uint32 PersistentAllocated = 0;
		Array<uint32> DeadList;

		uint32 NumTemporary = 0;
		volatile int64 TemporaryAllocated = 0;

		// 帧资源的轮询索引
		uint32 HeapIndex = 0;

		// （帧资源）堆的数量。双缓冲所以NumHeaps = 2.
		// 但是，如果是ShaderVisible的堆，NumHeaps = 1.【？原因？】
		uint32 NumHeaps = 0; 
		uint32 DescriptorSize = 0;
		bool32 ShaderVisible = false;
		D3D12_DESCRIPTOR_HEAP_TYPE HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;

		// 记录描述符堆的起始为止，方便寻址
		D3D12_CPU_DESCRIPTOR_HANDLE CPUStart[DX12::RenderLatency] = { };
		D3D12_GPU_DESCRIPTOR_HANDLE GPUStart[DX12::RenderLatency] = { };
		SRWLOCK Lock = SRWLOCK_INIT;

		~DescriptorHeap();

		void Init(uint32 numPersistent, uint32 numTemporary, D3D12_DESCRIPTOR_HEAP_TYPE heapType, bool shaderVisible);
		void Shutdown();

		// 分配和释放【持续区域的】内存
		PersistentDescriptorAlloc AllocatePersistent();
		void FreePersistent(uint32& idx);
		void FreePersistent(D3D12_CPU_DESCRIPTOR_HANDLE& handle);
		void FreePersistent(D3D12_GPU_DESCRIPTOR_HANDLE& handle);

		// 分配和释放【临时区域的】内存
		TempDescriptorAlloc AllocateTemporary(uint32 count);
		void EndFrame(); // 【临时内存】的部分是用完即弃，每帧结束的时候会将索引自动置零。而置零就是在这个方法做的

		// 各种寻址方法
		D3D12_CPU_DESCRIPTOR_HANDLE CPUHandleFromIndex(uint32 descriptorIdx) const;
		D3D12_GPU_DESCRIPTOR_HANDLE GPUHandleFromIndex(uint32 descriptorIdx) const;
		D3D12_CPU_DESCRIPTOR_HANDLE CPUHandleFromIndex(uint32 descriptorIdx, uint64 heapIdx) const;
		D3D12_GPU_DESCRIPTOR_HANDLE GPUHandleFromIndex(uint32 descriptorIdx, uint64 heapIdx) const;
		uint32 IndexFromHandle(D3D12_CPU_DESCRIPTOR_HANDLE handle);
		uint32 IndexFromHandle(D3D12_GPU_DESCRIPTOR_HANDLE handle);

		// 获取当前帧的描述符堆（每帧交替切换）
		ID3D12DescriptorHeap* CurrentHeap() const;
		uint32 TotalNumDescriptors() const { return NumPersistent + NumTemporary; }
	};

	// 单个Buffer资源
	struct Buffer
	{
		ID3D12Resource* Resource = nullptr; // Buffer资源本体
		uint64 CurrBuffer = 0;

		// CPU/GPU地址
		uint8* CPUAddress = 0;
		uint64 GPUAddress = 0;

		uint64 Alignment = 0;
		uint64 Size = 0;

		// 类似DX11：是否动态资源，是否CPU可访问
		bool32 Dynamic = false; 
		bool32 CPUAccessible = false;

		ID3D12Heap* Heap = nullptr;
		uint64 HeapOffset = 0;
		uint64 UploadFrame = uint64(-1);
		uint64 CreateFrame = uint64(-1);

		Buffer();
		~Buffer();

		void Initialize(uint64 size, uint64 alignment, bool32 dynamic, bool32 cpuAccessible,
			bool32 allowUAV, const void* initData, D3D12_RESOURCE_STATES initialState,
			ID3D12Heap* heap, uint64 heapOffset, const wchar* name);
		void Shutdown();

		MapResult Map();
		MapResult MapAndSetData(const void* data, uint64 dataSize);
		template<typename T> MapResult MapAndSetData(const T& data) { return MapAndSetData(&data, sizeof(T)); }
		uint64 QueueUpload(ID3D12Resource* srcResource, uint64 srcOffset, uint64 srcSize, uint64 dstOffset);

		uint64 CycleBuffer();

		void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const;
		void MakeReadable(ID3D12GraphicsCommandList* cmdList) const;
		void MakeWritable(ID3D12GraphicsCommandList* cmdList) const;
		void UAVBarrier(ID3D12GraphicsCommandList* cmdList) const;

		bool Initialized() const { return Size > 0; }

#if UseAsserts_
		bool ReadyForBinding() const;
#endif

	private:

		Buffer(const Buffer& other) { }
	};

	// For aligning to float4 boundaries
#define Float4Align __declspec(align(16))
#define Float4Align_ __declspec(align(16))

	struct ConstantBufferInit
	{
		uint64 Size = 0;
		bool32 Dynamic = true;
		bool32 CPUAccessible = true;
		const void* InitData = nullptr;
		D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
		ID3D12Heap* Heap = nullptr;
		uint64 HeapOffset = 0;
		const wchar* Name = nullptr;
	};

	struct ConstantBuffer
	{
		Buffer InternalBuffer;
		uint64 CurrentGPUAddress = 0;

		ConstantBuffer();
		~ConstantBuffer();

		void Initialize(const ConstantBufferInit& init);
		void Shutdown();

		void SetAsGfxRootParameter(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter) const;
		void SetAsComputeRootParameter(ID3D12GraphicsCommandList* cmdList, uint32 rootParameter) const;

		void* Map();
		template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); }
		void MapAndSetData(const void* data, uint64 dataSize);
		template<typename T> void MapAndSetData(const T& data) { MapAndSetData(&data, sizeof(T)); }
		void QueueUpload(ID3D12Resource* srcResource, uint64 srcOffset, uint64 srcSize, uint64 dstOffset);
	};

	struct StructuredBufferInit
	{
		uint64 Stride = 0;
		uint64 NumElements = 0;
		bool32 CreateUAV = false;
		bool32 UseCounter = false;
		bool32 Dynamic = false;
		bool32 CPUAccessible = false;
		const void* InitData = nullptr;
		D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
		bool32 ShaderTable = false;
		ID3D12Heap* Heap = nullptr;
		uint64 HeapOffset = 0;
		const wchar* Name = nullptr;
	};

	struct StructuredBuffer
	{
		Buffer InternalBuffer;
		uint64 Stride = 0;
		uint64 NumElements = 0;
		uint32 SRV = uint32(-1);
		bool32 IsShaderTable = false;
		D3D12_CPU_DESCRIPTOR_HANDLE UAV = { };
		ID3D12Resource* CounterResource = nullptr;
		D3D12_CPU_DESCRIPTOR_HANDLE CounterUAV = { };
		uint64 GPUAddress = 0;

		StructuredBuffer();
		~StructuredBuffer();

		void Initialize(const StructuredBufferInit& init);
		void Shutdown();

		D3D12_VERTEX_BUFFER_VIEW VBView() const;
		ID3D12Resource* Resource() const { return InternalBuffer.Resource; }

		D3D12_GPU_VIRTUAL_ADDRESS_RANGE_AND_STRIDE ShaderTable(uint64 startElement = 0, uint64 numElements = uint64(-1)) const;
		D3D12_GPU_VIRTUAL_ADDRESS_RANGE ShaderRecord(uint64 element) const;

		void* Map();
		template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); }
		void MapAndSetData(const void* data, uint64 numElements);
		void QueueUpload(ID3D12Resource* srcResource, uint64 srcOffset, uint64 srcNumElements, uint64 dstElemOffset);

		void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const;
		void MakeReadable(ID3D12GraphicsCommandList* cmdList) const;
		void MakeWritable(ID3D12GraphicsCommandList* cmdList) const;
		void UAVBarrier(ID3D12GraphicsCommandList* cmdList) const;

	private:

		StructuredBuffer(const StructuredBuffer& other) { }

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc(uint64 bufferIdx) const;
		void UpdateDynamicSRV() const;
	};

	struct FormattedBufferInit
	{
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		uint64 NumElements = 0;
		bool32 CreateUAV = false;
		bool32 Dynamic = false;
		bool32 CPUAccessible = false;
		const void* InitData = nullptr;
		D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
		ID3D12Heap* Heap = nullptr;
		uint64 HeapOffset = 0;
		const wchar* Name = nullptr;
	};

	struct FormattedBuffer
	{
		Buffer InternalBuffer;
		uint64 Stride = 0;
		uint64 NumElements = 0;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		uint32 SRV = uint32(-1);
		D3D12_CPU_DESCRIPTOR_HANDLE UAV = { };
		uint64 GPUAddress = 0;

		FormattedBuffer();
		~FormattedBuffer();

		void Initialize(const FormattedBufferInit& init);
		void Shutdown();

		D3D12_INDEX_BUFFER_VIEW IBView() const;
		ID3D12Resource* Resource() const { return InternalBuffer.Resource; }

		void* Map();
		template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); };
		void MapAndSetData(const void* data, uint64 numElements);
		void QueueUpload(ID3D12Resource* srcResource, uint64 srcOffset, uint64 srcNumElements, uint64 dstElemOffset);

		void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const;
		void MakeReadable(ID3D12GraphicsCommandList* cmdList) const;
		void MakeWritable(ID3D12GraphicsCommandList* cmdList) const;
		void UAVBarrier(ID3D12GraphicsCommandList* cmdList) const;

	private:

		FormattedBuffer(const FormattedBuffer& other) { }

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc(uint64 bufferIdx) const;
		void UpdateDynamicSRV() const;
	};

	struct RawBufferInit
	{
		uint64 NumElements = 0;
		bool32 CreateUAV = false;
		bool32 Dynamic = false;
		bool32 CPUAccessible = false;
		const void* InitData = nullptr;
		D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_GENERIC_READ;
		ID3D12Heap* Heap = nullptr;
		uint64 HeapOffset = 0;
		const wchar* Name = nullptr;
	};

	struct RawBuffer
	{
		Buffer InternalBuffer;
		uint64 NumElements = 0;
		uint32 SRV = uint32(-1);
		D3D12_CPU_DESCRIPTOR_HANDLE UAV = { };
		uint64 GPUAddress = 0;

		static const uint64 Stride = 4;

		RawBuffer();
		~RawBuffer();

		void Initialize(const RawBufferInit& init);
		void Shutdown();

		ID3D12Resource* Resource() const { return InternalBuffer.Resource; }

		void* Map();
		template<typename T> T* Map() { return reinterpret_cast<T*>(Map()); };
		void MapAndSetData(const void* data, uint64 numElements);
		void QueueUpload(ID3D12Resource* srcResource, uint64 srcOffset, uint64 srcNumElements, uint64 dstElemOffset);

		void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const;
		void MakeReadable(ID3D12GraphicsCommandList* cmdList) const;
		void MakeWritable(ID3D12GraphicsCommandList* cmdList) const;
		void UAVBarrier(ID3D12GraphicsCommandList* cmdList) const;

	private:

		RawBuffer(const RawBuffer& other) { }

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc(uint64 bufferIdx) const;
		void UpdateDynamicSRV() const;
	};

	struct ReadbackBuffer
	{
		ID3D12Resource* Resource = nullptr;
		uint64 Size = 0;

		ReadbackBuffer();
		~ReadbackBuffer();

		void Initialize(uint64 size);
		void Shutdown();

		const void* Map() const;
		template<typename T> const T* Map() const { return reinterpret_cast<const T*>(Map()); };
		void Unmap() const;

	private:

		ReadbackBuffer(const ReadbackBuffer& other) { }
	};

	struct Fence
	{
		ID3D12Fence* D3DFence = nullptr;
		HANDLE FenceEvent = INVALID_HANDLE_VALUE;

		~Fence();

		void Init(uint64 initialValue = 0);
		void Shutdown();

		void Signal(ID3D12CommandQueue* queue, uint64 fenceValue);
		void Wait(uint64 fenceValue);
		bool Signaled(uint64 fenceValue);
		void Clear(uint64 fenceValue);
	};

	struct Texture
	{
		uint32 SRV = uint32(-1);
		ID3D12Resource* Resource = nullptr;
		uint32 Width = 0;
		uint32 Height = 0;
		uint32 Depth = 0;
		uint32 NumMips = 0;
		uint32 ArraySize = 0;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		bool32 Cubemap = false;
		uint64 CreateFrame = uint64(-1);

		Texture();
		~Texture();

		bool Valid() const
		{
			return Resource != nullptr;
		}

		void Shutdown();

	private:

		Texture(const Texture& other) { }
	};

	struct RenderTextureInit
	{
		uint64 Width = 0;
		uint64 Height = 0;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		uint64 MSAASamples = 1;
		uint64 ArraySize = 1;
		bool32 CreateUAV = false;
		bool32 CreateRTV = true;
		bool32 NonPSReadable = false;
		bool32 CubeMap = false;
		uint32 NumMips = 1;
		D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATES(-1);
		const wchar* Name = nullptr;
	};

	struct RenderTexture
	{
		Texture Texture;
		D3D12_CPU_DESCRIPTOR_HANDLE RTV = { };
		D3D12_CPU_DESCRIPTOR_HANDLE UAV = { };
		Array<D3D12_CPU_DESCRIPTOR_HANDLE> ArrayRTVs;
		uint32 MSAASamples = 0;
		uint32 MSAAQuality = 0;
		bool32 NonPSReadable = false;

		RenderTexture();
		~RenderTexture();

		void Initialize(const RenderTextureInit& init);
		void Shutdown();

		void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;
		void MakeReadable(ID3D12GraphicsCommandList* cmdList, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;
		void MakeWritable(ID3D12GraphicsCommandList* cmdList, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;
		void MakeReadableUAV(ID3D12GraphicsCommandList* cmdList, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;
		void MakeWritableUAV(ID3D12GraphicsCommandList* cmdList, uint64 mipLevel = uint64(-1), uint64 arraySlice = uint64(-1)) const;
		void UAVBarrier(ID3D12GraphicsCommandList* cmdList) const;

		uint32 SRV() const { return Texture.SRV; }
		uint64 Width() const { return Texture.Width; }
		uint64 Height() const { return Texture.Height; }
		DXGI_FORMAT Format() const { return Texture.Format; }
		ID3D12Resource* Resource() const { return Texture.Resource; }
		uint64 SubResourceIndex(uint64 mipLevel, uint64 arraySlice) const { return arraySlice * Texture.NumMips + mipLevel; }
		D3D12_RESOURCE_STATES ReadState() const;

	private:

		RenderTexture(const RenderTexture& other) { }
	};

	struct VolumeTextureInit
	{
		uint64 Width = 0;
		uint64 Height = 0;
		uint64 Depth = 0;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		const wchar* Name = nullptr;
	};

	struct VolumeTexture
	{
		Texture Texture;
		D3D12_CPU_DESCRIPTOR_HANDLE UAV = { };

		VolumeTexture();
		~VolumeTexture();

		void Initialize(const VolumeTextureInit& init);
		void Shutdown();

		void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) const;
		void MakeReadable(ID3D12GraphicsCommandList* cmdList) const;
		void MakeWritable(ID3D12GraphicsCommandList* cmdList) const;

		uint32 SRV() const { return Texture.SRV; }
		uint64 Width() const { return Texture.Width; }
		uint64 Height() const { return Texture.Height; }
		uint64 Depth() const { return Texture.Depth; }
		DXGI_FORMAT Format() const { return Texture.Format; }
		ID3D12Resource* Resource() const { return Texture.Resource; }

	private:

		VolumeTexture(const RenderTexture& other) { }
	};

	struct DepthBufferInit
	{
		uint64 Width = 0;
		uint64 Height = 0;
		DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
		uint64 MSAASamples = 1;
		uint64 ArraySize = 1;
		D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
		const wchar* Name = nullptr;
	};

	struct DepthBuffer
	{
		Texture Texture;
		D3D12_CPU_DESCRIPTOR_HANDLE DSV = { };
		D3D12_CPU_DESCRIPTOR_HANDLE ReadOnlyDSV = { };
		Array<D3D12_CPU_DESCRIPTOR_HANDLE> ArrayDSVs;
		uint32 MSAASamples = 0;
		uint32 MSAAQuality = 0;
		DXGI_FORMAT DSVFormat = DXGI_FORMAT_UNKNOWN;

		DepthBuffer();
		~DepthBuffer();

		void Initialize(const DepthBufferInit& init);
		void Shutdown();

		void Transition(ID3D12GraphicsCommandList* cmdList, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after, uint64 arraySlice = uint64(-1)) const;
		void MakeReadable(ID3D12GraphicsCommandList* cmdList, uint64 arraySlice = uint64(-1)) const;
		void MakeWritable(ID3D12GraphicsCommandList* cmdList, uint64 arraySlice = uint64(-1)) const;

		uint32 SRV() const { return Texture.SRV; }
		uint64 Width() const { return Texture.Width; }
		uint64 Height() const { return Texture.Height; }
		ID3D12Resource* Resource() const { return Texture.Resource; }

	private:

		DepthBuffer(const DepthBuffer& other) { }
	};

	struct PIXMarker
	{
		ID3D12GraphicsCommandList* CmdList = nullptr;

		PIXMarker(ID3D12GraphicsCommandList* cmdList, const wchar* msg) : CmdList(cmdList)
		{
			PIXBeginEvent(cmdList, 0, msg);
		}

		PIXMarker(ID3D12GraphicsCommandList* cmdList, const char* msg) : CmdList(cmdList)
		{
			PIXBeginEvent(cmdList, 0, msg);
		}

		~PIXMarker()
		{
			PIXEndEvent(CmdList);
		}
	};

}