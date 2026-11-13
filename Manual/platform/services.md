# 基础服务、任务与诊断

## 后台工作与所有者线程

[TaskQueue](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/TaskQueue.h#L40) 将工作提交给后台线程，[CancellationToken](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/TaskQueue.h#L24) 让解码等长任务能协作退出。[Pump()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/TaskQueue.cpp#L117) 在所有者线程分发已完成的结果，[Shutdown()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/TaskQueue.cpp#L180) 停止接收任务并等待线程退出。

## 数据与文本

几何层以 [TransformBounds()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Math/Bounds.h#L15) 更新包围盒，用 [PerspectiveRH()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Math/Projection.h#L27) 构造右手透视投影。[DecodeUtf8()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Utf.cpp#L6) 和 [EncodeUtf16()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Utf.cpp#L79) 支撑跨平台文本边界，[Localization](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Localization.h#L9) 从语言表按键取文案。资产与构建输入由 [Sha256()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Hash.cpp#L7) 计算摘要。

## 性能、日志和设备故障

[Profiler](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Profiler.h#L27) 记录 CPU 范围耗时，[RenderTelemetry](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/RenderTelemetry.h#L84) 汇总渲染帧统计，[ReadD3D12GpuTimings()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12GpuTimer.cpp#L11) 读取 GPU 时间。[StartFileLog()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Log.cpp#L36) 建立日志，[InitializeProcessDiagnostics()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Windows/ProcessDiagnostics.cpp#L14) 安装进程诊断。设备移除后由 [DumpDred()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Dred.cpp#L39) 提取 GPU 面包屑及故障信息。
