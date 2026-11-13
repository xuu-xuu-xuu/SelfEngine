# 可见性、Hi-Z 与间接绘制

## 包围体与 GPU 候选集

[WorldBoundsCache](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Visibility.h#L9) 缓存物体的世界包围盒，[BuildFrustum()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Math/Bounds.h#L33) 从相机矩阵提取视锥平面。CPU 参考 [CullVisibilityReference()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/VisibilityData.cpp#L57) 与 GPU 共用可见性输入布局，便于逐项比较候选集。

## 层次深度

深度预通道先写入近处遮挡物，[D3D12HiZ](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12HiZ.h#L27) 将深度逐级归约。CPU 对照实现 [TestOcclusion()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/HiZ.cpp#L86) 将世界包围盒投影到屏幕矩形，按覆盖范围选 mip，仅在整个包围盒确定被挡住时剔除。[HiZHistory](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/HiZ.h#L76) 管理分辨率或相机突变后的历史有效性。

## 间接提交与统计

[D3D12Visibility](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Visibility.h#L25) 执行 GPU 可见性计算，[D3D12Indirect](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Indirect.h#L34) 准备并记录间接绘制命令。[BuildOcclusionHud()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/OcclusionHud.cpp#L30) 将遮挡统计转成调试显示，[RenderTelemetry](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/RenderTelemetry.h#L84) 保存逐帧的绘制和耗时记录。
