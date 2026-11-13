# D3D12 设备、命令与 GPU 资源寿命

## 记录和提交

[Device](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/Rhi.h#L397) 通过 [CreateD3D12()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/Rhi.h#L403) 创建后端。调用方从 [BeginCommands()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/Rhi.h#L433) 取得命令上下文，声明 Pass、绑定资源并记录绘制，最后通过 [Submit()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/Rhi.h#L436) 获得完成令牌。设备内部保存提交批次借用的资源，Fence 完成后才能回收。

## 分配、屏障与上传

[D3D12MemoryAllocator](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Memory.h#L22) 分配 GPU 内存，[D3D12DescriptorPool](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Descriptors.h#L31) 分配描述符范围，[D3D12StateRegistry](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12StateTracker.h#L43) 追踪资源状态与队列归属。[D3D12UploadArena](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12UploadArena.h#L20) 从映射页分配上传区，[RecordD3D12TextureRows()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12StagedUpload.cpp#L44) 按行记录纹理复制。

## 着色器、PSO 和预热

[LoadD3D12ShaderProgram()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.cpp#L461) 读取着色器程序，[D3D12PipelineCache](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12PipelineCache.h#L23) 复用已准备的管线。[RetainD3D12PipelineLibraryStorage()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12PipelineLibraryStorage.cpp#L26) 将持久管线库依赖的字节保留到相应对象释放。[WarmupPipelines()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/PipelineWarmup.cpp#L6) 在绘制之前准备需要的变体。
