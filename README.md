# 代码梳理

![NJNW9NT37J)9SUDQHQTPG5F](https://github.com/user-attachments/assets/faa9aac3-de80-439c-b226-fa640a3d1cdb)

## 上传系统

上传系统分成两个部分：常规上传系统 和 快速上传系统

### 常规上传系统

常规上传系统分为以下几个模块：

| 模块名称                | 描述                                           |
| ----------------------- | ---------------------------------------------- |
| **UploadQueue**         | 上传队列（命令队列）                           |
| **UploadSubmission(x16)** | 单个【子任务块】（命令列表、命令分配器）         |
| **UploadRingBuffer**    | 环形缓冲区，是最根本的内存结构。整个上传流程就是不断往Buffer中读写数据。每次读写数据都使用一个【子任务块】存储本次将会读写Buffer的哪段。 |
| **UploadContext**       | 上传上下文，负责直接对接底层DXAPI。记录依赖哪个子任务（命令分配器、命令列表），以及在Buffer中的偏移量。 |


#### 执行流程

- 在最上层，每次调用任务的时候，会调用Begin()/End()。（比如，我想传一个纹理到GPU上）
- 每次上传资源时，会调用Begin()，先把手头所有等待的子任务处理完，然后再开始新的任务。
    - 具体来说，会判断UploadQueue GPU上的Fence是否 大于 子任务的FenceValue
    - 如果大于，说明子任务已经完成，可以在Buffer上释放对应的内存区域了
- 然后启动子任务的【命令列表、命令分配器】，并创建一个上下文
- 通过上下文调用DXAPI，将initdata拷贝到上下文的Buffer上，然后又从Buffer拷贝到texture上。
    - Buffer相当于一个中介，拿了数据又传出去。
- 传输完成时，调用End()，进行实际的命令队列提交。
    - UploadQueue提交刚才的【命令列表】，并且记录命令队列的FenceValue到子任务上。

总体大致是这样的一个流程：

![GWE{GPYN1P5 SNHW0Q JQ{Y](https://github.com/user-attachments/assets/252dbaac-c480-49ac-92c2-1e3047c88c27)

## 描述符堆

分为了两个部分：持续内存和临时内存。

不同的堆size是不一样的：
- SRV堆的大小就给了4096持续+4096临时，并且是ShaderVisibleHeap。
- 而其他类型的堆（RTV/DSV/UAV）只给了256持续，没有临时，也不是ShaderVisibleHeap

如果是ShaderVisible（SRV），则分配两个堆，依赖帧缓冲交替使用；否则（RTV DSV UAV）只需分配一个堆。

在内存分布上，无论两个堆还是一个堆的情况，统一先按照持续内存的大小，布局持续内存；然后紧接着的临时内存大小的区段表示临时内存。

好的，我们来完善这个内存分配与释放的过程。

### 持续内存

持续内存使用一个DeadList记录目前仍然可用的索引。

#### 持续内存-DeadList

默认初始化如下：

```C++
    DeadList.Init(numPersistent);
    for(uint32 i = 0; i < numPersistent; ++i)
        DeadList[i] = uint32(i);
```

说白了就是初始化成下面这样一个表
|DeadList索引|0|1|2|3|4|5|6|7|8|...|
|-|-|-|-|-|-|-|-|-|-|-|
|实际值|0|1|2|3|4|5|6|7|8|...|

使用一个默认=0的常量记录DeadList的最新可用索引。每次分配内存的时候，将DeadList上对应的内存划掉。

每次释放指定索引的时候，将索引还给DeadList，并放在DeadList的最前面。

#### 持续内存-分配

每次分配内存的时候 使用PersistentDescriptorAlloc记录

- 本次分配的实际地址

注意，如果是SRV堆，由于是ShaderVisibleHeap，会给两个堆都分配内存。

### 临时内存

临时分配内存和持续分配内存有些不太一样的点。包括：

- 分配的时候，会分配到持续内存的最大值（SRV=4096, RTV/UAV/DSV=256）后面。
- 即使是SRV ShaderVisibleHeap，每次也**只会给当前帧**的堆分配内存
- 每帧结束时，临时分配的内存会**全部清零**。
