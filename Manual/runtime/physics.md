# 刚体、碰撞与角色控制

## PhysicsWorld

[PhysicsWorld](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Physics/PhysicsWorld.h#L55) 管理盒形刚体的位置、速度、质量和睡眠状态。[Step()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Physics/PhysicsWorld.cpp#L239) 先用轴对齐包围盒筛出候选，再通过 [CollideBoxes()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Physics/PhysicsWorld.cpp#L91) 求接触点与法线。求解器复用接触缓存并迭代施加冲量，接触开始和结束可从 [Events()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Physics/PhysicsWorld.h#L89) 读取。

## 查询和调试绘制

[Raycast()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Physics/PhysicsWorld.cpp#L371) 接收起点、方向、最大距离和掩码，命中时返回刚体、世界坐标与法线。[AppendPhysicsDebugDraws()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/PhysicsDebugDraw.cpp#L18) 将物理调试线转换为渲染数据；物理系统无需持有 GPU 对象。

## 胶囊角色

[CharacterController](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Physics/PhysicsWorld.h#L116) 保存直立胶囊、垂直速度和接地状态。[CharacterController::Move()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Physics/CharacterController.cpp#L69) 将移动拆成受限步长，处理胶囊与盒体接触，并限制推开动态刚体的冲量。C# 通过托管 [CharacterController](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/CharacterController.cs#L16) 转交移动和传送请求。
