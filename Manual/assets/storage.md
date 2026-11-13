# 文件解码、引用与派生缓存

## 导入器如何保留层级

[ImportFbx()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/ModelImporter.cpp#L301) 用 ufbx 解码 FBX，[ImportGltf()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/ModelImporter.cpp#L147) 用 cgltf 读取 glTF 或 GLB。两条路径都生成节点、网格与材质的统一表示。[ResolveReference()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/AssetIO.cpp#L136) 处理资产引用，文件落盘使用 [WriteAtomic()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/AssetIO.cpp#L106)。外部导入的复制和登记事务见[FBX 导入](import.md)。

## 解码工作与资源文件

[DecodeImage()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/AssetStore.h#L36) 产生 CPU 像素，[PrepareModelImages()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/AssetTasks.cpp#L7) 准备模型材质所需图像。[CompressBc1MipChain()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/TextureCompression.cpp#L153) 等压缩函数处理 mip 链；环境资产由 [SaveEnvironment()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/Environment.h#L33) 写出，并由 [DecodeEnvironment()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Assets/Environment.h#L34) 校验读回。派生数据的作用与显存预算分别见[模型与纹理](models.md)和[异步加载](loading.md)。

## 属性值如何持久化

[WritePropertyJson()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/PropertyJson.h#L10) 和 [ReadPropertyJson()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/PropertyJson.h#L20) 在有类型的属性值与 JSON 之间转换。场景实例化后，[World::CommitScene()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/WorldSceneCommit.cpp#L7) 将已验证的候选交给活动世界。
