# 开发环境与构建说明

记录日期：2026-10-22

## 1. 当前项目范围

- 开发与本次验证平台：Windows x64。
- 原生验证程序：C++20，使用 CMake、Ninja 和 MSVC 构建。
- 托管验证程序：C#，目标框架为 net10.0，目标平台为 win-x64。
- Visual Studio 2022 用于当前 C++ 开发。
- C# 项目通过独立安装的 .NET SDK 和 dotnet CLI 构建、运行。
- 项目源码、配置、文档和构建输出均位于仓库目录内。

## 2. 已验证的工具版本

| 工具 | 版本 | 用途 |
|---|---|---|
| Visual Studio Community 2022 | 17.14.19 | C++ 编辑与调试 |
| MSVC 编译器 | 19.44.35219 | 编译 C++ 源码 |
| MSVC 工具集目录版本 | 14.44.35207 | 标识已安装的原生工具集 |
| Windows SDK | 10.0.26100.0 | Windows API 的头文件和链接库 |
| CMake | 3.31.6-msvc6 | 配置项目并生成构建规则 |
| Ninja | 1.12.1 | 按依赖关系执行构建 |
| .NET SDK | 10.0.402 | 创建、还原和构建 C# 项目 |
| .NET Runtime | 已安装 10.0.13 | 为当前 C# 程序提供运行环境 |
| Git | 2.55.0.windows.4 | 版本管理 |

以上是当前开发机的已验证版本，不表示其他版本均不可用。

## 3. 版本选择与依赖约定

- global.json 要求准确使用 .NET SDK 10.0.402。
- rollForward 设置为 disable，不自动换用其他 SDK 版本。
- allowPrerelease 设置为 false，不选择预览版 SDK。
- CMakeLists.txt 要求 CMake 至少为 3.25。
- CMakePresets.json 选择 Ninja 和 cl，并定义 Debug、Release 配置。
- 当前配置没有强制锁定具体 MSVC 和 Windows SDK 版本。
- 升级工具后，需要重新验证构建与运行，并更新本文件。
- 当前没有引入第三方源码库或额外的 NuGet 包引用。
- bin、obj、out 等生成目录不提交到 Git。

## 4. 执行前准备

以下命令均在仓库根目录执行，即包含 CMakeLists.txt 和 global.json 的目录。
每条命令成功后，再继续下一条；遇到错误应先停止检查。

### 4.1 初始化 C++ 开发环境

在 PowerShell 中定位 Visual Studio 2022，并加载 x64 工具链：

```powershell
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsPath = & $vswhere -version '[17.0,18.0)' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw '未找到安装了 C++ 工具链的 Visual Studio 2022。' }
& (Join-Path $vsPath 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
```

该操作只配置当前终端会话。新开终端后，需要重新初始化。

### 4.2 检查 .NET SDK

C# 构建可以使用普通 PowerShell，无需初始化 MSVC。

```powershell
dotnet --version
```

在仓库根目录执行时，预期输出 10.0.402，与 global.json 一致。

## 5. C++ 构建与运行

### 5.1 Debug

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
.\out\build\windows-x64-debug\EngineSandbox.exe
$LASTEXITCODE
```

### 5.2 Release

```powershell
cmake --preset windows-x64-release
cmake --build --preset windows-x64-release
.\out\build\windows-x64-release\EngineSandbox.exe
$LASTEXITCODE
```

两种配置均应输出“自研引擎第一次构建成功”，退出码为 0。

## 6. C# 构建与运行

运行当前构建出的程序，需要电脑上安装兼容的 .NET 10 x64 Runtime。

### 6.1 还原依赖

```powershell
dotnet restore .\Source\ManagedSandbox\ManagedSandbox.csproj
```

### 6.2 Debug

```powershell
dotnet build .\Source\ManagedSandbox\ManagedSandbox.csproj --configuration Debug --no-restore
.\Source\ManagedSandbox\bin\Debug\net10.0\win-x64\ManagedSandbox.exe
$LASTEXITCODE
```

### 6.3 Release

```powershell
dotnet build .\Source\ManagedSandbox\ManagedSandbox.csproj --configuration Release --no-restore
.\Source\ManagedSandbox\bin\Release\net10.0\win-x64\ManagedSandbox.exe
$LASTEXITCODE
```

当前项目两种配置使用相同依赖，可以复用上述还原结果。

两种配置均应输出：

```text
自研引擎 C# 验证程序构建成功
当前是否为 64 位进程:True
```

退出码应为 0。

## 7. 全新构建目录验证

以下示例要求验证目录最初不存在。
如果目录已经存在，应选择新目录名，并在同一组命令中一致替换。
无需删除之前成功构建的目录。

### 7.1 C++

先检查目录，预期返回 False：

```powershell
Test-Path .\out\build\repro-debug
```

确认目录不存在后，依次配置、构建和运行：

```powershell
cmake --preset windows-x64-debug -B out/build/repro-debug
cmake --build out/build/repro-debug
.\out\build\repro-debug\EngineSandbox.exe
$LASTEXITCODE
```

-B 只为本次配置覆盖构建目录，不修改预设文件。
因此后续构建直接指定这个目录。

### 7.2 C#

先检查目录，预期返回 False：

```powershell
Test-Path .\out\repro-managed
```

确认目录不存在后，依次还原、构建和运行：

```powershell
dotnet restore .\Source\ManagedSandbox\ManagedSandbox.csproj --artifacts-path .\out\repro-managed
dotnet build .\Source\ManagedSandbox\ManagedSandbox.csproj --configuration Debug --no-restore --artifacts-path .\out\repro-managed
.\out\repro-managed\bin\ManagedSandbox\debug_win-x64\ManagedSandbox.exe
$LASTEXITCODE
```

--artifacts-path 是命令参数，必须放在完整的 dotnet 命令中，不能单独执行。
还原和构建必须传入相同路径，使依赖信息、中间文件和最终输出保持一致。

## 8. T01 构建验收记录

验证日期：2026-10-22  
验证对应的源码与配置提交：ef0f60a

| 验证项 | 结果 |
|---|---|
| C++ Debug 构建与运行 | 通过，中文正常，退出码 0 |
| C++ Release 构建与运行 | 通过，中文正常，退出码 0 |
| C++ 全新构建目录复现 | 通过，退出码 0 |
| C# Debug 构建与运行 | 通过，中文正常，64 位进程，退出码 0 |
| C# Release 构建与运行 | 通过，中文正常，64 位进程，退出码 0 |
| C# 全新输出与中间目录复现 | 通过，中文正常，64 位进程，退出码 0 |

本记录覆盖当前开发机上的验证。
全新目录验证仍可使用已安装的工具链和 NuGet 全局包缓存，
不代表已经完成另一台电脑或 Windows 10/11 双系统验证。
