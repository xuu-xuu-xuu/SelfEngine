# 构建工具、预览与导出

## 原生工具与资源产物

[生产构建目标](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/CMakeLists.txt) 分别构建引擎库、原生 DLL、编辑器和离线工具。项目工具通过 [ValidateProject()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Tools/Project.cpp#L173) 校验描述，再由 [ExportProjectDependencies()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Tools/Project.cpp#L293) 收集依赖。[ShaderBuilder.cpp](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Tools/ShaderBuilder.cpp) 编译着色器产物，[EnvironmentCooker.cpp](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Tools/EnvironmentCooker.cpp) 烘焙环境资源。

## 行为编译与预览

[Build-EditorScriptCandidate.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Build-EditorScriptCandidate.ps1) 计算脚本输入并构建候选程序集，供宿主加载。[Run-ProjectPreview.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Run-ProjectPreview.ps1) 解析项目和启动参数，调用预览程序并记录结果。后台进程通过 [RunEditorProcess()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorProcessRunner.cpp#L172) 管理；用户打开的外部程序通过 [ExternalProcessBroker](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/ExternalProcessBroker.h#L10) 启动。

## 游戏导出与编辑器包

[ExportJob](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/ExportJob.h#L29) 管理导出的后台工作和取消，[ExportService](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/ExportService.h#L31) 组织参数并读取分阶段报告。[Export-Game.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Export-Game.ps1) 汇集项目、原生运行库和托管宿主，[Publish-Managed.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Publish-Managed.ps1) 负责托管发布。[Build-EditorPackage.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Build-EditorPackage.ps1) 构建可分发的编辑器包，源码身份由 [SourceIdentity.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/SourceIdentity.ps1) 计算，项目构建租约由 [ProjectBuildLease.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/ProjectBuildLease.ps1) 管理。

## 项目回放与发布边界

[ReplayProject()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Tools/Project.cpp#L312) 在固定步注入项目记录的冲量，推进物理世界并计算状态摘要。[ExportOperationController](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/ExportOperation.h#L17) 跟踪导出阶段。游戏包在暂存目录生成文件清单及摘要，成功后才[移动为最终目录](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Export-Game.ps1#L198)。
