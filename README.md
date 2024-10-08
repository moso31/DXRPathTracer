# 代码梳理
![NJNW9NT37J)9SUDQHQTPG5F](https://github.com/user-attachments/assets/84414f94-dfc0-425c-b465-20ff8695bd9b)


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

在最上层，每次调用任务的时候，会调用Begin()/End()。（比如，我想传一个纹理到GPU上）

每次上传资源时，会调用Begin()，先把手头所有等待的子任务处理完，然后再开始新的任务。具体来说，会判断UploadQueue GPU上的Fence是否 大于 子任务的FenceValue，如果大于，说明子任务已经完成，可以在Buffer上释放对应的内存区域了

然后启动子任务的【命令列表、命令分配器】，并创建一个上下文

通过上下文调用DXAPI，将initdata拷贝到上下文的Buffer上，然后又从Buffer拷贝到texture上。Buffer相当于一个中介，拿了数据又传出去。

传输完成时，调用End()。UploadQueue提交刚才的【命令列表】，并且记录命令队列的FenceValue到子任务上。

![GWE{GPYN1P5 SNHW0Q JQ{Y](https://github.com/user-attachments/assets/76a9e5af-ca5c-4acf-aa6a-0b6cdd993410)
