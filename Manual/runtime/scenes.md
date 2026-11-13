# 场景加载、相机与空间查询

## 场景替换和叠加

[ParseSceneDocument()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Scenes/SceneSerializer.cpp#L121) 将 JSON 解析为场景文档，[InstantiateScene()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Scenes/SceneSerializer.cpp#L144) 将文档实例化到世界中。[SceneManager](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/SceneManager.h#L31) 通过 [RequestFile()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/SceneManager.h#L47) 发起读取，在帧边界处理结果。替换与叠加模式共享暂存和提交过程，取消或过期的请求不能覆盖新的场景。

## SceneSpatialIndex

[SceneSpatialIndex](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/SceneSpatialIndex.h#L16) 将局部包围盒随实体变换更新为世界包围盒，静态和动态对象分别组织在 [BoundsIndex](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/CoreLayer/Spatial/BoundsIndex.h#L20) 中。[QueryAabb()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/SceneSpatialIndex.h#L28) 返回区域候选，[RaycastCandidates()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/SceneSpatialIndex.h#L29) 返回射线候选。需要精确命中时，调用方继续检查几何体。

## 相机与拾取射线

[SelectCamera()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/Camera.cpp#L68) 确定场景相机，[BuildCameraSnapshot()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/Camera.cpp#L38) 从变换和视口生成视图、投影矩阵。[ViewportPointToRay()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Runtime/Camera.cpp#L123) 将视口坐标变成世界空间射线；编辑器用自己的相机快照完成选择和导航。
