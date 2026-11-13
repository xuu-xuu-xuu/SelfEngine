<p align="center">
  <img src="Resources/Branding/FlareMark.png" alt="Flare 引擎图标" width="200">
</p>

<p align="center">使用C++作为底层逻辑语言的自研游戏引擎</p>

## 源码导览

| 目录 | 内容 |
| --- | --- |
| [Source](Source) | C++ 引擎与编辑器源码。渲染器位于 [Rendering](Source/FunctionLayer/Rendering)，D3D12 后端位于 [Graphics](Source/PlatformLayer/Graphics)，C# 脚本宿主位于 [Scripting](Source/FunctionLayer/Scripting)。 |
| [Resources](Resources) | HLSL 着色器、纹理、项目模板、本地化和品牌资源。 |
| [BuildConfig](BuildConfig) | 原生目标和资源构建规则，以及 C# 与 NuGet 配置。 |
| [Scripts](Scripts) | 编辑器启动、脚本预览、打包和游戏导出的 PowerShell 入口。 |
| [ThirdParty](ThirdParty) | 随源码分发的第三方库及其许可证。 |
| [Manual](Manual) | [开发者文档](https://xuu-xuu-xuu.github.io/FlareEngine/) · [使用者文档](Manual/user/index.md) |
