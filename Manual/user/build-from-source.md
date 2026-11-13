# 从源码启动编辑器

在 Windows x64 上安装 Visual Studio 2022 的「使用 C++ 的桌面开发」工作负载，包含 Windows SDK、CMake（3.25 或更高）与 Ninja。脚本项目使用仓库根目录 `global.json` 指定的 .NET SDK 10.0.402。

在仓库根目录打开 PowerShell，依次执行下面的命令。前两行把 MSVC x64 工具加载到当前终端。

```powershell
. .\Scripts\Import-MsvcEnvironment.ps1
Import-EngineMsvcEnvironment
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
dotnet build .\Source\FunctionLayer\Scripting\Bootstrap\Bootstrap.csproj -c Release
.\Scripts\Start-Editor.ps1
```

看到 **Flare 引擎** 启动器后，点击「新建项目」选择一个工作目录，或点击「打开项目」选择已有项目的 `project.json`。构建出的编辑器在 `out/build/windows-x64-release/FlareEditor.exe`，下次也可以直接双击启动。
