# 环境光照、大气与反射

## 环境贴图进入渲染器

[DecodeRadiance()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/HdrImage.cpp#L19) 把 Radiance HDR 解码为浮点像素。[BakeEnvironment()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/Environment.h#L31) 从全景图生成辐照度立方体、预滤波辐射立方体和 BRDF 查找表，[EnvironmentRenderCache](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/EnvironmentRender.h#L14) 保存当前资源版本并上传。[PrepareEnvironmentBindings()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/EnvironmentRender.cpp#L50) 将结果交给天空和材质绘制。

## 大气与雾的 CPU 模型

[Atmosphere](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Atmosphere.h#L22) 根据大气参数重建透射率查找表，[SkyRadiance()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Atmosphere.cpp#L114) 沿视线积分天空散射。[FogTransmittance()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Atmosphere.cpp#L152) 按相机高度和视距计算雾的透射率。时间变化的数据来源见[昼夜系统](../runtime/daylight.md)。

## 探针与屏幕空间反射的 CPU 参考实现

[BoxProjectReflection()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Reflection.cpp#L66) 将反射方向修正到探针包围盒。[BlendReflectionProbes()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Reflection.cpp#L95) 混合邻近探针的权重，[PrefilterGgx()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Reflection.cpp#L131) 按粗糙度预滤波。[TraceScreenSpaceReflection()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Reflection.cpp#L157) 则沿反射射线查询屏幕深度并报告命中。
