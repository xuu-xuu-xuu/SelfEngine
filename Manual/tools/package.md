# 构建环境、资源复制与包内启动

## 编译环境和运行时目录

[Import-MsvcEnvironment.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Import-MsvcEnvironment.ps1) 将 x64 MSVC 工具环境导入当前构建进程。[Copy-RendererResources.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Copy-RendererResources.ps1) 复制渲染器所需的着色器与纹理；[Generate-FlareBranding.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Generate-FlareBranding.ps1) 从品牌图生成 Windows 图标资源。[Start-Editor.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Start-Editor.ps1) 按配置选择编辑器，并转交项目、语言和安全模式参数。

## 源码工作区与已分发的包

[Export-Package.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Export-Package.ps1) 是导出入口之一，[EditorPackageRuntime.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/EditorPackageRuntime.ps1) 提供编辑器包清单读取、预编译脚本匹配等操作。[Start-ExportedGame.ps1](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Scripts/Start-ExportedGame.ps1) 从包目录解析运行文件和项目路径，转交运行模式、语言和截帧参数。验证项目描述与导出依赖的公共入口见[构建与导出](delivery.md)。
