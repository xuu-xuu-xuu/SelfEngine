# 交换链、绘制与读回

## 每个窗口的呈现资源

[InitializeD3D12Context()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Context.cpp#L212) 创建图形上下文，[InitializeD3D12Presentation()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Presentation.cpp#L190) 为窗口创建交换链、深度缓冲、帧上下文和 Fence。[ResizeD3D12Presentation()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Presentation.cpp#L874) 等待相关 GPU 工作完成后重建尺寸相关资源。浮动视口也有独立呈现目标，使用同一设备的直接队列。

## 一次绘制在后端做什么

[RecordD3D12Meshes()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Mesh.cpp#L156) 绑定网格、材质与管线并发出绘制。深度预通道由 [D3D12DepthPrepass](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12DepthPrepass.h#L8) 记录，前向结果由 [RecordD3D12ForwardOutput()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Forward.cpp#L80) 输出，后处理由 [RecordD3D12PostProcess()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12PostProcess.cpp#L115) 记录。CPU 常量布局与 HLSL 之间通过[大小、偏移与布局版本](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/MaterialLayout.h)校验。

## 截帧与性能输出

[SetCaptureOutput()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/SandboxRenderer.h#L41) 选择捕获帧，提交接口支持读回颜色、HDR 或阴影深度。[WritePpmImage()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/ImageWriter.cpp#L5) 写出像素结果。CPU 采样可用 [WriteProfileCsv()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Profiler.cpp#L35) 导出，帧图还可用 [WriteDot()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/FrameGraph.cpp#L800) 和 [WriteJson()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/FrameGraph.cpp#L856) 检查 Pass 依赖和资源寿命。
