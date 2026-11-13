# 场景视口、变换手柄与选中描边

## 从鼠标坐标到变换

[EditorViewportController](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorViewportController.h#L12) 协调相机导航、选取、聚焦和拖动。[EditorViewportCamera](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorViewportCamera.h#L8) 提供编辑相机快照，[PickEditorBounds()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorViewportCamera.cpp#L26) 从射线和包围盒产生选择。[EditorGizmo](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorGizmo.h#L14) 根据操作轴、坐标空间与投影计算移动、旋转或缩放，并通过[编辑事务](session.md)提交。

## 固定世界网格

[PSGrid()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Resources/Shaders/SceneOverlay.hlsl#L41) 用相机射线与地面求交，按世界 XZ 坐标绘出 1 米细格与 10 米主格。导数控制抗锯齿宽度，交点深度参与遮挡，摄像机无需携带一张有限网格。

## 模型轮廓

[D3D12SceneOverlay](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12SceneOverlay.h#L12) 记录选中物体掩码和叠加绘制。[PSOutline()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Resources/Shaders/SceneOverlay.hlsl#L64) 比较相邻像素的轮廓覆盖和可见性，将线条限制在可见轮廓边缘。
