# FBX 导入

从项目外选择 FBX 时，编辑器先列出模型及其依赖的目标位置，再复制文件并登记资产。要查看“文件已选中，为什么还没出现在项目里”，先检查导入计划的诊断，再检查登记结果。[编辑器调用入口](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorApplication.cpp#L612)

## AssetImportService

[`se::AssetImportService`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/AssetImportService.h#L35) 提供静态导入接口。[Plan()](#plan) 生成计划，[ExecuteRegistered()](#executeregistered) 接收该计划和项目的 [AssetStore](models.md#assetstore)。[查看类定义](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/AssetImportService.h#L35)

### Plan

输入 [AssetImportRequest](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/AssetImportService.h#L10)，指定源文件、项目内容根目录和相对目标目录。返回的 [AssetImportPlan](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/AssetImportService.h#L25) 包含可执行的文件列表、冲突和诊断。批量选择时应同时查看 `valid` 和 `diagnostics`，部分输入组可能被拒绝。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/AssetImportService.cpp#L294)

### ExecuteRegistered

按计划复制所需文件，将可登记项加入资产库，通过 `imported` 返回资产 ID。项目内重导入可复用原文件。登记过程中出错时，事务会尝试回滚本次改动，调用方通过 `errorKey` 和 `detail` 获取原因。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/AssetImportService.cpp#L372)
