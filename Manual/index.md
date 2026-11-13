# Flare 开发者文档

一帧画面从[窗口与主循环](platform/frame.md)开始。运行时提取场景数据，渲染器编排[前向 Pass 与帧图](render/pipeline.md)，最后由[D3D12 命令提交](render/device.md)送到 GPU。编辑器和 C# 宿主都复用这条链上的引擎接口。

**渲染与光照**

[灯光组件](runtime/light.md) · [前向管线与帧图](render/pipeline.md) · [光照着色、材质接口与 Shader 包](render/materials.md) · [阴影、分簇灯光与环境遮蔽](render/shadows.md) · [环境光照、大气与反射](render/environment.md) · [后处理、抗锯齿与调色](render/post.md) · [可见性、Hi-Z 与间接绘制](render/visibility.md) · [LOD、HLOD 与代理几何](render/detail.md) · [D3D12 设备、命令与 GPU 资源寿命](render/device.md) · [交换链、绘制与读回](render/presentation.md)

**世界与运行时**

[实体、层级与组件存储](runtime/entities.md) · [组件基类与生命周期](runtime/component-base.md) · [World 的灯光接口](runtime/world.md) · [原生组件反射与属性序列化](runtime/metadata.md) · [场景加载、相机与空间查询](runtime/scenes.md) · [主循环、任务顺序与事件](runtime/scheduling.md) · [昼夜、天空与场景环境](runtime/daylight.md) · [刚体、碰撞与角色控制](runtime/physics.md)

**资产与资源**

[FBX 导入](assets/import.md) · [模型、纹理与材质资产](assets/models.md) · [文件解码、引用与派生缓存](assets/storage.md) · [异步加载、上传与纹理驻留](assets/loading.md)

**C# 脚本与宿主**

[C# 行为、绑定与事件](scripts/behaviours.md) · [脚本反射、程序集卸载与 GC](scripts/lifetime.md) · [托管宿主与原生 ABI](scripts/bridge.md)

**UI 系统**

[自研 UI 文档、样式与布局](ui/toolkit.md) · [字体图集、文本编辑与 DPI](ui/text.md) · [控件状态、列表与叠层](ui/widgets.md) · [游戏 UI、绑定与合成](ui/game.md)

**编辑器**

[项目入口、启动进度与编辑器外壳](editor/workspace.md) · [面板模型、默认场景与界面合成](editor/panels.md) · [面板拖出与吸附](editor/docking.md) · [场景视口、变换手柄与选中描边](editor/viewport.md) · [编辑会话、撤销与播放](editor/session.md) · [资产操作、属性面板与项目文档](editor/documents.md)

**平台与基础服务**

[从窗口消息到一帧画面](platform/frame.md) · [输入、文字事件与窗口适配](platform/input.md) · [基础服务、任务与诊断](platform/services.md)

**构建与分发**

[构建工具、预览与导出](tools/delivery.md) · [构建环境、资源复制与包内启动](tools/package.md)

源码基线为 [Flare e7ca801](https://github.com/xuu-xuu-xuu/FlareEngine/tree/e7ca801292037b33172d094cc4a434682ac83c9c)。
