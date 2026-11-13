# LOD、HLOD 与代理几何

## 单模型 LOD

[BakeLodChain()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/LodBake.cpp#L117) 生成模型的细节层级，[SelectLodLevel()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/LodSelect.cpp#L13) 按投影误差选层。[UpdateLodTransition()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/LodSelect.cpp#L54) 记录过渡，绘制可用抖动在新旧层级间交接。[OptimizeVertexCache()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/MeshOptimization.h#L13) 重排索引以改善顶点缓存利用率。

## 分区代理与流送

[BakeHlod()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/HlodBake.cpp#L71) 将实例按空间单元组织为代理数据。[HlodStreamer](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/HlodStreamer.h#L13) 在后台读取场景，呈现线程取得结果后按相机位置更新 [UpdateResidency()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/HlodStream.cpp#L22)。[SelectHlodProxy()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/HlodSelect.cpp#L13) 按投影误差选择代理，[HlodProxyCache](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/HlodProxyCache.h#L9) 约束代理的上传与淘汰。

## Meshlet 与公告板代理

[BakeMeshlets()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/MeshletBake.cpp#L92) 把网格分成顶点和三角形数量受限的小簇，[CullMeshlets()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/MeshletBake.cpp#L233) 对簇做可见性筛选。[BakeImpostorAtlas()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/ImpostorBake.cpp#L40) 烘焙多视角图集，[ImpostorViewPair()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/ImpostorBake.cpp#L156) 按视向选出相邻视图供混合。
