# 异步加载、上传与纹理驻留

## AsyncAssetLoader

[AsyncAssetLoader](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/AsyncAssetLoader.h#L101) 按资产 ID、种类和目标接收请求并返回 [AssetTicket](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/AsyncAssetLoader.h#L62)。工作线程在独立资产视图中解码模型及关联图像，所有者线程通过 [Pump()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/AsyncAssetLoader.cpp#L294) 合并结果。只请求 CPU 数据时，到此即可读取。

## 从 CPU 交给 GPU

[AssetUploadScheduler](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/AssetUploadScheduler.h#L25) 按每帧预算消费等待上传的条目。[CompleteUpload()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/AsyncAssetLoader.cpp#L344) 交回上传回执，下一次泵送复核请求仍然有效后才发布就绪状态。渲染资源通过 [RenderAssetLease](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/AsyncAssetLoader.h#L38) 保活，GPU 提交的寿命见[设备与 Fence](../render/device.md)。

## 纹理预算

[TextureResidency](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/TextureResidency.h#L20) 追踪纹理最近使用帧、目标 mip 和预计字节数。[TrimToBudget()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/TextureResidency.cpp#L36) 在预算压力下调整驻留选择，真正的子资源传输经 [UploadTextureSubresourcesRange()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/Rhi.h#L415) 记录。
