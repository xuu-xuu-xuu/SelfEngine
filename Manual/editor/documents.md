# 资产操作、属性面板与项目文档

## 重命名、移动和依赖

[导入服务](../assets/import.md#assetimportservice) 把外部文件带入项目。[EditorAssetActions](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorAssetActions.h#L54) 在移动或删除前生成影响计划，检查场景、材质、UI 和绑定引用；[Execute()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorAssetActions.cpp#L295) 按计划修改文件与项目登记。[ValidatePlayAssets()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorAssetActions.cpp#L438) 在播放前检查所依赖的资产。

## 专用编辑器

[DrawPropertyWidget()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/PropertyWidgets.cpp#L9) 依据属性元数据选择编辑控件。[MaterialEditSession](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/MaterialEditSession.h#L8) 保存材质编辑事务和撤销历史，[RenderMaterialThumbnail()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/MaterialThumbnail.cpp#L16) 产生预览。[UICanvasEditor](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/UICanvasEditor.h#L10) 在预览与屏幕坐标间换算并命中节点，以支持画布上的移动、缩放和锚点编辑。

## 项目、脚本与设置

[ProjectSettingsService](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/ProjectSettingsService.h#L24) 读取和合并项目、构建设置；[EditorSettingsDocument](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSettingsDocuments.h#L10) 跟踪草稿、源摘要与脏状态。[CreateBehaviour()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorScriptTools.cpp#L120) 生成行为脚本，[PrepareOpenSource()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorScriptTools.cpp#L222) 组装外部编辑器命令。脚本字段的来源见[反射](../scripts/lifetime.md)。
