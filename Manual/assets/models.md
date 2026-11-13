# 模型、纹理与材质资产

## AssetStore

[AssetStore](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/AssetStore.h#L42) 扫描内容目录与元数据，按资产 ID 查找源文件。[ImportStaticModel()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/StaticModel.h#L87) 读取 FBX 或 glTF/GLB，输出节点层级、网格、材质与纹理引用。[BuildStaticModelLayout()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/StaticModelPreparation.cpp#L18) 准备节点与渲染网格的对应关系。程序内置的立方体、球体分别由 [MakeUnitCube()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/Primitives.cpp#L7) 与 [MakeUnitSphere()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/Primitives.cpp#L27) 生成。

## 材质与着色器变体

[MaterialDocument](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/MaterialDocument.h#L8) 保存可序列化的材质属性和依赖。[WriteMaterialProperty()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/MaterialMetadata.cpp#L38) 通过元数据写入属性，[ResolveMaterialInstance()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/MaterialInstance.h#L31) 将实例参数解析为渲染材质。[VariantMacros()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/MaterialVariants.h#L50) 将特征掩码转换成着色器宏，区分贴图、双面等变体。

## 顶点与纹理压缩

[CompressMeshVertices()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/VertexCompression.cpp#L101) 量化位置并打包法线、UV 等属性。纹理烘焙按用途选择 [BakeBc1Texture()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/TextureBake.cpp#L56)、[BakeBc5Texture()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/TextureBake.cpp#L92) 或 [BakeBc4Texture()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/TextureBake.cpp#L127)，生成带 mip 的块压缩数据，由 [TextureBakeCache](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/TextureBake.h#L41) 缓存。
