# 面板模型、默认场景与界面合成

## 共享选择状态

[EditorContext](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorContext.h#L52) 将当前项目、文档、选择和资产状态提供给外壳。[EditorHierarchyModel](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorPanels.h#L24) 将节点展开成可见行，[EditorInspectorModel](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorPanels.h#L64) 管理选中对象的属性。实体复制由 [Duplicate()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSession.cpp#L146) 完成，新场景则从 [CreateDefaultScene()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/SceneDefaults.cpp#L21) 建立默认对象。

## 菜单、工具栏与替代绘制路径

[EditorChrome](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorChrome.h#L26) 按命令组织菜单、工具栏与状态文字。ImGui 路径从 [EditorPanels()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorPanelRegistry.cpp#L19) 查询面板描述，[CopyImGuiGeometry()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/ImGuiGeometry.cpp#L6) 复制顶点、索引与裁剪命令，交给 [GuiRenderer](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/GuiRenderer.h#L6) 记录 GPU 绘制。窗口、加载和编辑界面颜色来自[共享色表](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/FlarePalette.h)。

## 未保存的多个文档

[EditorExternalDocuments](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorExternalDocuments.h#L13) 将材质、UI 或脚本绑定文档的保存与恢复操作提供给编辑器。[WriteRecoveryGroup()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSessionStore.cpp#L320) 将会话恢复文件按组写入；构建完成需重启时，[WriteBuildRestartRequest()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/BuildRestartRequest.cpp#L7) 将请求写给启动流程。

资产面板通过 [EditorAssetsModel::SetFilter()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorPanels.cpp#L256) 更新过滤结果，再按滚动位置取可见行。
