# 前向管线与帧图

## 从灯光组件到绘制命令

[灯光组件](../runtime/light.md) 随世界快照提取为灯光实例。[PrepareForwardLighting()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/ForwardRenderer.cpp#L9) 按稳定实体顺序选择方向光和点光源，将颜色乘以强度并限制输入范围。场景路径由 [SceneRenderRequest](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/SceneRenderPath.h#L10) 指定，编辑器视口组合基础光照、阴影与程序化天空。

## FrameGraph

[FrameGraph](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/FrameGraph.h#L85) 用纹理的读写版本描述 Pass 之间的依赖。[Compile()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/FrameGraph.cpp#L466) 解析生产者与消费者、检查环并计算执行次序及临时纹理寿命。[RecordShadowedFrameGraph()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/ForwardRenderer.cpp#L152) 根据编译结果向 [CommandContext](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/Rhi.h#L358) 记录阴影、场景和输出命令。

## 材质与批次

[StaticModelRenderCache](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/StaticModelRender.h#L39) 把模型和材质变成可提交的网格。[SortOpaqueDraws()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/RenderQueue.cpp#L18) 按绘制状态排序，[PbrInstanceBatcher](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/Instancing.h#L14) 将可共享网格与材质的实例合批。 像素阶段的数据与接口见[光照着色与 Shader 包](materials.md)。
