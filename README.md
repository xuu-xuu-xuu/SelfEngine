# SelfEngine

面向 Windows x64 的实验性游戏引擎项目，使用 C++20 开发核心，计划接入 C# 行为脚本。

## 当前进度

当前仓库包含两个控制台验证程序：

- `EngineSandbox`：使用 CMake、Ninja 和 MSVC 构建的 C++ 验证程序，当前用于 T02 的所有权、日志、错误处理与 COM 练习。
- `ManagedSandbox`：面向 .NET 10 和 Windows x64 的最小 C# 程序。

T01 基线中的两个程序均已在当前开发机上通过 Debug、Release 构建与运行检查，
并通过全新输出与中间目录的重新构建验证。

截至 2026-10-26，T02 的 RAII、移动与借用、日志等级、文件读取结果枚举、
断言、HRESULT 诊断、COM 引用与异常边界练习已通过运行和理解检查，T02 验收通过。
`RunComPtrExample()` 已从主流程提取；同进程 C 接口练习在内部捕获 C++ 异常并返回错误码。
当前正常路径已通过 Debug、Release 和全新 Debug 构建目录验证，退出码均为 0；
Debug 下的两类异常转换路径均能清理资源，由调用者以退出码 1 结束程序。
验证范围与结果见 [T02 验收记录](Docs/Environment.md#10-t02-验收记录)。

渲染和 C++/C# 互调仍处于规划阶段，尚未实现。

## 环境要求

- Visual Studio 2022，安装 C++ 构建工具和 Windows SDK。
- CMake 3.25 或更新版本，以及 Ninja。
- .NET SDK 10.0.402，与 `global.json` 中的版本约定一致。

当前使用 Visual Studio 2022 进行 C++ 开发，
使用独立安装的 .NET SDK 和 dotnet CLI 构建、运行 C# 程序。

已验证的工具版本和详细操作见[开发环境与构建说明](Docs/Environment.md)。

## 快速开始

所有命令均在仓库根目录执行。每条命令成功后，再继续下一条。

### C++

先按照环境说明初始化 x64 MSVC 开发环境，再执行：

```powershell
cmake --preset windows-x64-debug
cmake --build --preset windows-x64-debug
.\out\build\windows-x64-debug\EngineSandbox.exe
```

### C#

```powershell
dotnet restore .\Source\ManagedSandbox\ManagedSandbox.csproj
dotnet build .\Source\ManagedSandbox\ManagedSandbox.csproj --configuration Debug --no-restore
.\Source\ManagedSandbox\bin\Debug\net10.0\win-x64\ManagedSandbox.exe
```

两个程序均应以退出码 `0` 正常结束，C# 程序还应显示当前进程为 64 位。

Release 构建和全新目录验证步骤见[开发环境与构建说明](Docs/Environment.md)。

## 目录与文件

- `Source/Sandbox/`：C++ 控制台验证程序。
- `Source/Core/`：最小日志设施。
- `Source/Platform/Windows/`：Windows HRESULT 检查与日志诊断。
- `Source/ManagedSandbox/`：C# 控制台验证程序。
- `Docs/`：开发文档。
- `CMakeLists.txt`：原生程序的构建目标和编译要求。
- `CMakePresets.json`：项目共享的原生构建配置。
- `global.json`：.NET SDK 版本选择。
