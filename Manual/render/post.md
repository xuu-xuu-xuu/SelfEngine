# 后处理、抗锯齿与调色

## HDR 输出

[RecordPostProcessPasses()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/PostProcessRenderer.cpp#L79) 记录高亮提取、横纵模糊、色调映射及可选 FXAA 和叠加层。[BuildExposureHistogram()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/AutoExposure.cpp#L30) 从亮度分布计算直方图，[AutoExposure](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/AutoExposure.h#L34) 随时间逼近目标曝光。GPU 路径从 [D3D12Exposure](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Exposure.h#L43) 记录直方图和适应过程。

## 时间抗锯齿

[HaltonJitter()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/TemporalAA.cpp#L24) 给投影生成子像素抖动。CPU 参考 [ResolveTaa()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/TemporalAA.cpp#L85) 按速度重投影历史颜色，并限制历史值以减少拖影。D3D12 后端用 [RecordVelocity()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Taa.cpp#L226) 写速度，用 [Resolve()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Taa.cpp#L259) 合并当前帧与历史帧。

## 颜色分级

[Tonemap()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/ColorGrading.cpp#L160) 选择 Reinhard 或 ACES 拟合曲线。[ApplyLiftGammaGain()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/ColorGrading.cpp#L172) 调整暗部、中间调和亮部，[ColorLutFile](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/ColorLutFile.h#L25) 管理 LUT 文件的读取与变更检测。
