# 开发环境与构建说明

2026-10-29 语言表补充：已接入 `Localization`、程序目录定位和资源复制目标。Debug 构建与无窗口检查通过；用户英文运行日志已确认循环开始、结束文案切换正确，并确认文本键不随语言改变。中文交互反馈及该次进程退出码尚未提供。范围、格式、回退和命令见 [语言表说明](Localization.md)。T03 本身已完成运行与理解检查并验收，语言表补充项的待反馈内容单独记录。

初始记录日期：2026-10-22；T02 验收日期：2026-10-26；T03 阶段进度更新日期：2026-10-29。

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

T01 基线中的两种配置均输出“自研引擎第一次构建成功”，退出码为 0。
T02 收尾版本的正常路径会输出所有权、COM 与文件读取练习日志，最后输出
“[Info] 离开作用域之后”，退出码为 0；必须从仓库根目录运行以读取 README。
T02 收尾版本的正常路径已通过 Debug、Release 和全新 Debug 构建目录验证，详细范围见第 10 节。
当前 T03 工作区代码会在这些练习日志之后创建并显示空窗口，进入消息循环。
点击窗口关闭按钮后输出“[Info] 消息循环已结束”，正常退出码为 0。
T03 当前验证来自 Debug；本轮未重新验证 Release 或全新构建目录，范围见第 11 节。

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

## 9. Visual Studio 小三角启动与工作目录

配置与验证日期：2026-10-25。

在 Visual Studio 中打开仓库根目录，选择 `windows-x64-debug` 配置，
启动项选择 `EngineSandbox - 项目根目录`。保存源码后点击小三角或按 F5，
由 Visual Studio 按 CMake 目标检查构建并启动调试。

本机 `.vs/launch.vs.json` 使用以下配置，文件保存为 UTF-8：

```json
{
  "version": "0.2.1",
  "defaults": {},
  "configurations": [
    {
      "type": "default",
      "project": "CMakeLists.txt",
      "projectTarget": "EngineSandbox.exe",
      "name": "EngineSandbox - 项目根目录",
      "currentDir": "${workspaceRoot}"
    }
  ]
}
```

- `project` 与 `projectTarget` 关联实际 CMake 构建目标，避免只启动已有的 exe 而遗漏构建。
- `currentDir` 指定工作区根目录；相对路径 `README.md` 因此指向仓库中的文件。
- `.vs/` 已由现有规则忽略，不提交本机启动配置；此处保留配置以便恢复。
- Build Insights 用于收集构建性能数据，不是普通构建或运行所必需的步骤。
  本次没有修改它的权限或系统跟踪设置，也没有启用管理员模式。

验证结果：备份并移除当前 Debug 的一个生成目标文件后，点击小三角，
Visual Studio 自动重新生成该目标文件并启动程序，调试输出确认退出码为 0。
另从仓库根目录运行生成的 exe，确认读取 README 成功，编号 1、2、3 的对象各析构一次，
执行到“离开作用域之后”，退出码为 0。当时的模拟失败开关为 false。
本节保留 2026-10-25 的 Debug 启动配置验证记录；后续 T02 完整验收见第 10 节。

## 10. T02 验收记录

验收日期：2026-10-26。证据来自用户逐步执行后提供的构建与运行输出，并结合当前源码核对。
验收对象是在阶段提交 `d800f98` 基础上增加异常边界练习的代码，收尾版本已保存为提交 `c95e2d7`。

| 验证项 | 结果 |
|---|---|
| Debug 正常路径 | 通过，接口返回码 0，程序退出码 0 |
| Debug 标准异常路径（参数 1） | 捕获 `std::runtime_error`，接口返回 1；资源清理后由 main 返回 1 |
| Debug 其他 C++ 异常路径（参数 2） | `catch (...)` 捕获整数异常，接口返回 2；资源清理后由 main 返回 1 |
| Release 配置、构建与正常运行 | 通过，接口返回码 0，程序退出码 0；提供的构建输出未出现警告 |
| 全新 Debug 构建目录 | `out/build/t02-repro-debug` 初始不存在，配置、构建与正常运行通过，退出码 0 |
| 综合理解检查 | 通过；已纠正悬空指针、HRESULT 布尔判断和异常清理时机的误解 |

正常路径会读取 `README.md`，查询到 COM 缓冲区大小为 16 字节，编号 #1–#4 的对象各析构一次，
最后输出“[Info] 离开作用域之后”。异常边界的两条失败路径中，#4 在接口返回错误码前析构，
外层 #1–#3 在 main 提前返回时析构；不以 vector 元素的具体析构顺序作为验收条件。
当前源码保留三个实验分支，默认调用 `Engine_RunBoundaryExample(0)`。

全新目录实际使用的命令如下，在已初始化工具链的仓库根目录逐条执行：

```powershell
Test-Path .\out\build\t02-repro-debug
cmake --preset windows-x64-debug -B out/build/t02-repro-debug
cmake --build out/build/t02-repro-debug
.\out\build\t02-repro-debug\EngineSandbox.exe
$LASTEXITCODE
```

首次验证时第一条输出为 False；以后复验应使用另一个尚不存在的目录，并一致替换命令中的路径。
Release 本次验证正常路径，失败路径的运行证据来自 Debug。
全新目录验证使用当前电脑已安装的工具链，不表示另一台电脑或部署包验证。
此轮未改动或重新验收 C# 项目；同进程异常边界练习不代表已实现 DLL 导出或 C#/C++ 互调。

<a id="t03-progress"></a>

## 11. T03 阶段进度（未完成）

记录日期：2026-10-26；最新更新于 2026-10-29。早期运行依据为用户通过 Visual Studio 的 Debug 配置提供的截图、
日志、退出码和操作反馈。最新一轮由助手成组实现窗口关联与输入迁移并完成 Debug 增量构建；用户随后提供了新版本 W 状态读取、失焦清零、Esc 取消和 Alt+F4 退出的日志。迁移前的运行结果不等同于新版本已验证。
T03 改动仍在工作区，尚未提交；本记录不表示 T03 整体验收通过。

| 已验证的小步 | 结果与范围 |
|---|---|
| 窗口与消息循环 | 空窗口可显示、拖动；关闭后消息循环结束，退出码 0 |
| 尺寸与最小化 | 拖动边框可记录客户区宽高；最小化有日志，恢复后重新记录尺寸 |
| 键盘焦点 | 切换应用时记录失焦、获焦；正常关闭仍返回 0 |
| W 键按下与松开 | 可记录两类消息；长按的重复消息由状态比较过滤，不重复记录按下状态 |
| W 键失焦清理 | 按住 W 切出，在外部松开后切回，重新轻按可收到完整的按下、松开日志 |
| w/W 文本对照 | 英文输入且关闭 Caps Lock 时，普通 W 输出文本 w，Shift+W 输出文本 W；两者按键码相同 |
| 左键按下与客户区坐标（2026-10-27） | 客户区为 1264 × 681，两次点击分别记录 (13, 9)、(1252, 669)；正常关闭后退出码 0 |
| 鼠标理解检查（2026-10-27） | 已理解消息类型、lParam 坐标及带符号提取；补讲后确认客户区坐标与屏幕坐标的区别，正确计算 (520, 340) 相对客户区原点 (500, 300) 为 (20, 40) |
| 左键松开（2026-10-27） | 第一对按下、松开均为 (177, 199)；第二对按下为 (194, 184)、松开为 (769, 314)；正常关闭后退出码 0 |
| 左键松开理解检查（2026-10-27） | 已确认两个 case 共用代码时由 if/else 选择一个分支，松开消息携带松开时坐标，return 0 仅结束本次 WindowProc 调用 |
| 窗口外松开观察，加入捕获前（2026-10-27） | 第一轮 (216, 189) 按下、松开完整；第二轮 (26, 23) 按下后在窗口外松开，无对应松开日志；第三轮 (94, 39) 按下、松开完整；退出码 0 |
| 窗口外松开理解检查（2026-10-27） | 用户确认缺少松开日志不能证明物理按键仍按住，假设仅靠按下、松开消息维护状态，变量可能错误保留 true；已纠正“移出屏幕”的表述，本轮实际是移出窗口到桌面 |
| 捕获与释放正常路径（2026-10-27） | 修正捕获条件中误写的 WM_MBUTTONDOWN 后，按下为 (1164, 257)，松开为 (1603, 239)，超过客户区宽度 1264 仍收到松开消息；正常关闭退出码 0 |
| 捕获理解检查（2026-10-27） | 用户理解捕获改变鼠标消息接收者而不限制指针位置、松开后应释放捕获恢复正常分发，以及 GetCapture() == hwnd 检查捕获是否属于当前窗口 |
| 主动释放的捕获变化通知（2026-10-28） | 普通点击依次输出按下 (940, 317)、失去鼠标捕获、松开 (940, 317)，退出码 0；关闭前另有一条捕获变化通知，具体触发来源尚未确定 |
| 捕获变化理解检查（2026-10-28） | 补讲后确认单纯把指针移出窗口不会自动解除捕获，本次通知由 ReleaseCapture() 引发；松开消息的处理先释放捕获，再打印松开日志 |
| 左键状态正常路径（2026-10-28） | 窗口内点击 (1078, 248) 和移到 (1549, 247) 松开均输出按下状态 1、捕获变化与松开后状态 0；客户区宽 1264，退出码 0 |
| 左键状态理解检查（2026-10-28） | 已确认 static 保留跨调用状态、清零不证明物理按键已松开；补讲后确认若先记录日志而抛出异常，后续清理会被跳过，状态可能错误保留 true |
| Esc 主动取消捕获（2026-10-28） | 按下 (913, 249) 后状态 1，随后捕获变化和 Esc 日志均为 0；用户明确确认此时物理左键仍按住。之后在 (914, 247) 松开仍为 0，再次普通点击可从 1 回到 0，退出码 0 |
| Esc 取消理解检查（2026-10-28） | 已确认清零由 WM_CAPTURECHANGED 执行；Esc 判断须位于 W 键过滤之前，否则会被 break 跳过 |
| 鼠标移动与取消（2026-10-28） | 日志显示拖动坐标从 (735, 379) 变化至 (771, 378)，松开后状态 0，退出码 0；用户明确确认不按左键时无拖动日志，保持左键按住在客户区内按 Esc 后继续移动不再新增，松开重按后恢复 |
| 鼠标移动理解检查（2026-10-28） | 已确认停止日志由 if (!isLeftDown) 提前返回控制，释放捕获后客户区内仍能收到移动消息；新 WM_LBUTTONDOWN 恢复状态，漏清零则会继续记录 |
| 普通键盘状态表（2026-10-28） | W（87）与 A（65）可独立记录按下、松开；长按 W 不重复记录按下状态，文本 w 仍可重复。Esc（27）记录按下、松开并保留取消捕获功能，取消后左键状态 0；退出码 0 |
| 键盘状态表失焦清理（2026-10-28） | 补测按住 A 后失焦，日志显示键盘状态已清除；在外部松开并切回，再按 A 可记录状态 1、0，正常关闭退出码 0 |
| 键盘状态表理解检查（2026-10-28） | 已确认不同按键占独立槽位，256 个元素的有效索引为 0–255；补讲后确认失焦可能漏收多个松开消息，只清 A 会让 W 错误保留 true，理解检查通过 |
| 系统按键状态（2026-10-28） | 用户补测普通 W：87 状态 1、文本 w、87 状态 0；随后按住左 Alt 操作 W：18 状态 1、87 状态 1→0、18 状态 0；正常关闭退出码 0 |
| 系统按键默认处理（2026-10-28） | 另一轮日志显示 Alt（18）和 F4（115）按下后失焦清表、消息循环结束，退出码 0，Alt+F4 正常关闭；不要求窗口关闭后补齐松开日志 |
| 系统按键理解检查（2026-10-28） | 已确认 message 区分消息类别、wParam 指定本次变化的按键；两种 DOWN 都应得到 true。补讲后确认 break 跳出 switch 后继续默认处理，return 会结束本次窗口过程；失焦仍可能漏收松开消息，需要保留清表，理解检查通过 |
| PeekMessage 循环（2026-10-28） | 初次替换后日志显示尺寸变化、W 的 87 状态 1→0；发现更新位置与 Sleep 位于外层循环外，且遗漏结束日志与返回语句。修正后再次验证 Alt+F4，出现“消息循环已结束”，退出码 0 |
| 消息循环理解检查（2026-10-28） | 已理解 GetMessage 等待消息、消息数量不等于帧数、PeekMessage 无消息返回 0 和 PM_REMOVE 的用途；已确认无消息只退出内层，WM_QUIT 通过 running 控制两层退出，循环外的更新只在退出后执行 |
| T02 练习函数提取（2026-10-28） | 用户将练习整体提取为 int RunT02Examples()；正常路径日志执行一遍，对象析构后输出“离开作用域之后”，窗口显示并经 Alt+F4 关闭，退出码 0 |
| 提取后的失败传播（2026-10-28） | 临时调用 Engine_RunBoundaryExample(1)：对象 #4 析构、接口返回码 1、失败日志、对象 #1–#3 各析构一次；main 在创建窗口前返回 1。源码参数随后恢复为 0，文件迁移后的正常运行见后续记录；迁移后未重测模拟失败路径 |
| 函数职责理解检查（2026-10-28） | 已确认 return 1 只结束练习函数，main 若忽略返回值仍会继续创建窗口；练习函数不负责创建窗口，理解检查通过。已纠正将函数称为“测试类”的术语 |
| T02 练习文件迁移（2026-10-28） | 练习及专用辅助代码已迁入 Source/Sandbox/T02Examples.cpp，头文件只声明入口；main 包含头文件并保留返回值检查。CMake 已列入新源文件；迁移后正常参数 0 的练习、资源析构、窗口显示与关闭均有日志，退出码 0 |
| 编码与读取排查（2026-10-28） | 新文件最初为代码页 936，cpp 的 UTF-8 解码失败位置 0x15f 与 C4828 一致；现两文件均通过严格 UTF-8 校验，CMake 重复条目已删除。初次 README 打开失败时未提供工作目录；重新运行日志确认目录为仓库根目录并成功读到 # SelfEngine。未另行取得完整的零警告构建报告 |
| 文件拆分理解检查（2026-10-28） | 已区分头文件声明和 cpp 函数实现；补讲后确认漏编译实现会在链接时找不到函数，已理解相对文件路径依据运行时工作目录，理解检查通过 |
| 窗口职责与数据归属理解检查（2026-10-29） | 已确认复制 HWND 不会创建新窗口、角色移动放在每帧游戏更新、状态对象销毁后回调不能继续安全访问。用户将每帧更新与操作流畅度联系起来，助手补充消息频率与更新频率不同，以及窗口记录输入、游戏决定行为的职责划分。本步为概念检查，无新增源码或运行验证 |
| InputState 数据归组（2026-10-29） | 用户在 main.cpp 定义 InputState，WindowProc 使用 static InputState input{}；源码中 5 处键盘和 6 处左键使用均改为成员访问，保留失焦清表、捕获变化清零与默认处理。用户日志包含拖动、Esc 取消后左键为 0、随后松开仍为 0、失焦与正常退出码 0；所给片段在 Esc 后无新增拖动日志。用户另以文字确认 W/A 按下、松开与按住 A 失焦后切回测试正常，未另贴对应日志 |
| InputState 理解检查（2026-10-29） | 已区分 InputState 类型与 input 实例，理解去掉 static 会在每次回调中重新初始化。针对 main 无法直接访问 input 的原因补讲生命周期与作用域后，用户确认 main 另建同名对象会产生独立状态，修改其中一份不会改变另一份；理解检查通过 |
| WindowState 数据组合（2026-10-29） | 用户定义包含 HWND 与 InputState 的 WindowState，main 在创建窗口前建立局部对象 window，创建结果、失败判断和显示调用使用 window.hwnd。初次生成失败时源码将 WindowState 放在 InputState 前；调整定义顺序后，日志显示窗口启动、消息循环结束，退出码 0。仅保存数据，未实现自动销毁或回调关联；窗口过程仍使用独立的静态输入对象 |
| WindowState 理解检查（2026-10-29） | 将“调用 InputState”修正为“包含 InputState 成员”，补讲按值成员需要完整类型定义。针对对象所在位置与状态同步的误解补讲后，用户确认 WindowProc 返回时 main 中的 window 仍存在，修改 window.input 不会自动修改回调内的静态 input；理解检查通过 |
| 窗口创建参数理解检查（2026-10-29） | 已讲明 lpParam 携带对象地址、WM_NCCREATE 的 lParam 指向创建信息且 lpCreateParams 保存传入地址。用户确认通过地址修改原对象、创建回调无需等到主消息循环；补讲后正确区分 &window（C++ 状态对象地址）与 window.hwnd（系统窗口句柄），理解检查通过。源码最后一个参数已改为 &window，但尚无 WM_NCCREATE 接收分支，也未提供这次参数修改后的运行结果 |
| WM_NCCREATE 地址取回（2026-10-29） | 用户新增创建消息分支，从 lParam 取得 CREATESTRUCTW 指针，再从 lpCreateParams 还原 WindowState 指针；空状态地址返回 FALSE，正常路径在异常边界内打印地址后返回 TRUE。用户同次运行中 main 的 &window 与回调取回的 state 地址相同，均在“窗口已显示，开始消息循环”前输出；关闭后循环结束，退出码 0。仅验证本次创建消息的地址传递，没有保存供后续消息使用的关联；空地址拒绝创建路径未主动验证 |
| 地址取回理解检查（2026-10-29） | 用户正确判断 WM_NCCREATE 返回 0 会中止创建，下一条消息不能直接使用本 case 的局部 state，但 main 的 window 仍存在。针对 createInfo 与 lpCreateParams 的误解，补讲 Windows 将 &window 填入创建信息成员，随后通过成员读取取回地址；用户在简化例子中正确判断位于 8000 的对象，其成员保存 1000 时读取结果为 1000，理解检查通过 |
| 窗口状态关联与输入迁移（2026-10-29，实现与构建完成） | 用户选择助手直接修改、用户运行验证的协作模式。WM_NCCREATE 检查并保存地址到 GWLP_USERDATA，后续消息读取地址并通过 InputState& 引用原对象；旧静态输入副本已移除。保留普通/系统键盘默认处理、失焦清表与捕获变化清零。WM_NCDESTROY 解除关联、清空句柄和输入；main 退出循环时销毁仍存在的窗口。主循环仅在 W 状态变化时记录读取结果。代码复核未发现实质问题，Debug 增量编译与链接退出码 0；用户随后验证主循环 W 状态读取、失焦清零、Esc 取消与 Alt+F4 退出（退出码 0），持久关联与引用的核心理解检查通过；本轮窗口外松开和异常路径未单独验证 |
| 本轮构建环境排查（2026-10-29） | 助手命令行初次构建遇到 MSVC D8050，复核正常本机执行仍失败；仅将本次进程的 TEMP/TMP 指向仓库内 out/validation/window-state/tmp 后，使用相同 Debug 预设编译 main.cpp 并链接成功，输出无警告。未改变用户全局环境或项目编译参数，未确定原临时目录问题的具体根因；未启动 EngineSandbox，也未重测 Release/C# |
| 理解检查 | 已确认窗口类与窗口实例、回调与消息循环、消息参数含义、异常边界、状态保留与失焦清理，以及按键状态与文本输入的区别 |

当前输入状态类型位于 `Source/Platform/InputState.h`，事件类型位于 InputEvents.h，窗口状态类型位于 `Source/Platform/Windows/Window.h`。窗口创建、关联、尺寸、销毁及消息分发位于 Window.cpp；键盘、鼠标、字符、焦点清理及捕获处理位于 WindowsInput.cpp。`Source/Sandbox/main.cpp` 仅保留启动、T02 调用与 RunSandboxApp 调用；SandboxApp.cpp 组织消息、事件消费和计时循环。
T02 练习已迁入 `Source/Sandbox/T02Examples.h/.cpp`，头文件提供入口声明，实现文件包含练习及专用辅助代码。
收到 `WM_QUIT` 后设置退出标志，跳出内层，再由外层的退出检查跳过更新位置；退出循环后先销毁仍存在的系统窗口，再记录结束日志并返回原消息的 `message.wParam`。
`Sleep(1)` 位于外层循环内，仅用于临时减少空转，不代表精确等待时间或固定帧率。主循环现读取 `window.input.keysDown['W']`，只在状态变化时打印日志；这是每次更新看到的当前状态，同一轮消息中完成的快速按下/松开可能不会出现在这条日志中。当前已加入基础计时和更新帧计数，尚无实际游戏更新。
`Window.h` 包含 InputState.h，定义包含 HWND 和 InputState 成员的 `WindowState`；`RunSandboxApp()` 在创建窗口前建立 `WindowState window{}` 并持有到清理结束。CreateEngineWindow 通过引用访问该对象，取得模块句柄、注册窗口类、创建关联窗口并显示，成功返回 true；DestroyEngineWindow 销毁仍存在的窗口，已经销毁则直接成功。RunSandboxApp 检查结果并返回退出码，再由 main 返回给操作系统。创建接口当前仅用于主线程启动时调用一次，不支持多窗口或反复创建；窗口存活期间状态对象须保持地址稳定。
创建窗口的最后一个参数为 `&window`。WM_NCCREATE 从 CREATESTRUCTW::lpCreateParams 取回对象地址，使用 SetWindowLongPtrW 保存到 GWLP_USERDATA；清除旧错误后，同时检查返回值和 GetLastError，失败返回 FALSE。成功时设置对象的 HWND，保留地址日志并返回 TRUE。
每次 WindowProc 调用先用 GetWindowLongPtrW 取回关联；尚无关联时执行默认处理。输入消息交给 ProcessWindowsInput，其 InputState& 参数引用 state->input，静态副本已移除。WM_NCDESTROY 清除关联、清空 HWND 和输入状态后调用默认处理，main 持有的对象存活到这些回调完成；尚未封装自动析构的窗口类。
状态表共用处理 `WM_KEYDOWN/WM_KEYUP/WM_SYSKEYDOWN/WM_SYSKEYUP`：先检查索引，再过滤重复状态，更新后处理 Esc 取消并记录日志。
`newState` 同时判断普通与系统 DOWN，两个 UP 都得到 false；每条消息只更新 `input.keysDown` 中 `wParam` 指定的槽位。
键盘分支以 `break` 跳出输入模块的 switch，返回 UseDefault，再由窗口过程执行 DefWindowProcW；`WM_KILLFOCUS` 在记录日志前调用 `input.keysDown.fill(false)`。
系统按键在记录状态后仍交由默认处理；迁移前已覆盖 Alt+W 与 Alt+F4，没有验证全部键码、其他系统组合键或 F10 默认行为。新版本交互回归待用户运行。
`WM_CHAR` 只观察 w/W，不代表已实现完整 Unicode 文本或输入法处理。
窗口文件拆分版本已收到用户正常退出码 0 的日志，输入两组回归由用户文字确认正常；最新计时版本仅完成构建与代码复核，尚未运行。创建失败及额外 WM_QUIT 收尾路径未主动验证，也未重新验证 C#。

鼠标目前处理 `WM_LBUTTONDOWN`、`WM_LBUTTONUP` 和 `WM_MOUSEMOVE`，使用 `GET_X_LPARAM`、`GET_Y_LPARAM` 将坐标提取为 `int`。
用户日志已确认客户区内按下、松开及正常关闭；加入捕获前，窗口外松开缺少对应消息，
移回窗口未补发松开，随后普通点击仍有完整消息。
当前按下时调用 `SetCapture(hwnd)`，松开时确认 `GetCapture() == hwnd` 后调用 `ReleaseCapture()`，
这些操作位于日志的 try 之前。修正左键消息常量后，已验证右侧窗口外松开仍有日志。
鼠标 DOWN/UP 分支先更新 `input.isLeftDown`，再调用捕获 API；`WM_CAPTURECHANGED` 先清零，再记录状态，
不尝试重新捕获。状态变化均在日志操作之前完成，日志输出读取变量的实际值。
当前 Esc 的按下处理位于键盘状态表更新之后：确认捕获属于当前窗口后调用 `ReleaseCapture()`，
再打印实际状态；Esc 分支没有直接给 `input.isLeftDown` 赋值。
窗口内点击、窗口外松开和物理左键仍按住时的 Esc 主动取消均已验证。
本轮不代表已验证系统或其他窗口抢夺捕获；API 失败路径仍未单独验证。
日志异常导致跳过清理仍是理解检查中的假设场景。
`WM_MOUSEMOVE` 只读取保存的 `input.isLeftDown` 决定是否记录位置，不根据移动消息重新开启左键状态。
移动验证全程位于客户区内；Esc 后即使物理左键仍按住，保存的状态为 false，因此不再记录拖动坐标。
本步仅记录位置，没有实现实际对象拖动或渲染。

**尚未完成：** 只读诊断与事件清理职责边界的理解检查、T03 最终验收核对。更新 dt 上限的运行与理解检查此前已通过；本轮必要输入回归已收到日志或用户文字确认，当前尚无角色移动或渲染逻辑。

**当前位置与下一步：** 用户已明确采用协作加速模式：助手成组修改并检查构建，用户运行交互验证，完成后集中讲解 2–3 个关键概念；在功能完成或暂停时集中更新进度。
窗口关联与输入迁移已实现，Debug 增量编译、链接通过。用户新版本日志已验证：主循环读到 W 的 1、0，失焦清表后主循环读到 0；拖动期间 Esc 释放捕获并将左键状态清零，随后松开仍为 0；Alt+F4 结束消息循环，退出码为 0。本轮未单独提供窗口外松开的回归证据，创建关联失败及额外 WM_QUIT 清理路径也未主动验证。
用户确认去掉引用会产生副本；补讲创建时传递地址与后续持久保存的区别后，正确回答后续消息从 GWLP_USERDATA 取回地址，核心理解检查通过。
2026-10-29 最新一组已将状态类型、窗口过程、创建与销毁逻辑迁入 Window.h/.cpp，并加入 CMake 源文件清单；窗口过程正文与拆分前逐字一致，仅增加匿名命名空间使其成为实现细节。T02 练习现在先于窗口类注册执行。Debug 增量构建编译 Window.cpp、main.cpp 并链接，退出码 0，输出无编译警告；构建进程沿用仓库内临时目录，未改全局环境。未启动程序，未重测 Release 或全新目录。
窗口拆分版本后续验证：用户日志覆盖缩放、最小化/恢复、焦点变化与 Alt+F4 关闭，退出码 0；用户以文字确认前两组（两处地址相同、W/失焦、窗口外松开、Esc 取消）也正常，未另贴完整日志。对象寿命和函数指针回调已讲解，用户补答确认创建函数返回 false 后由 main 的 return 1 结束程序，核心理解检查通过。

2026-10-29 已加入 FrameTimer.h/.cpp，使用 QueryPerformanceFrequency 缓存计数频率、QueryPerformanceCounter 读取时间点；先做整数差再转换为 double 除以频率，得到秒单位 deltaSeconds。main 在窗口显示后初始化计时器，在消息排空且未收到退出请求时每轮更新一次，累计 totalSeconds 和 frameCount，约每秒打印当前 dt（乘 1000 转为毫秒）。当前帧数只是更新次数，不是渲染呈现帧数；dt 是相邻采样间隔，不是一秒平均值或纯 CPU 工作耗时。Sleep、消息处理和日志造成的间隔都会计入后续采样；首帧从初始化起点开始。
初始化或更新失败会记录错误并返回退出码 1；更新失败仍经过统一窗口收尾。正常 WM_QUIT 的退出码保存到 exitCode 后返回。计时器保存未经截断的实际间隔；main 现单独限制供后续游戏更新使用的 dt，尚无固定步长、失焦/最小化暂停策略；拖动窗口或调试停顿仍可能产生较长的实际间隔。
Debug 增量构建编译 FrameTimer.cpp 与 main.cpp 并链接成功，退出码 0，输出无编译警告；git diff --check 通过。构建仅为本次进程指定仓库内 TEMP/TMP，未改全局环境。未启动程序，未重测 Release、全新目录或计时失败路径。
用户随后提供计时版本日志：累计帧数从 1931 增至 2255，时间从 33.815447 s 增至 38.873539 s；采样 dt 多数约 15.6 ms，另有 36.8534 ms，W 按下与失焦清零可被主循环读到，正常结束退出码 0。该片段未提供标题栏拖动数秒的长 dt 证据，客户区左键拖动日志不能替代该验证；计时理解检查尚未进行。API 参考：[QueryPerformanceCounter](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancecounter)、[QueryPerformanceFrequency](https://learn.microsoft.com/en-us/windows/win32/api/profileapi/nf-profileapi-queryperformancefrequency)。

按用户要求继续拆分职责：新增纯数据 InputState.h 及 WindowsInput.h/.cpp。ProcessWindowsInput 仅借用 HWND 和 InputState&，不包含或查询 WindowState，不保存状态地址；返回 Handled 时窗口过程返回 0，返回 UseDefault 时继续默认处理，返回值不表示成功/失败。原输入分支除 6 处 return 0 改为 Handled 外逐字一致；捕获相关同步回调仍更新同一份输入。Window.cpp 保留创建、关联、尺寸、销毁和最终默认处理；main 与计时模块未修改。
输入拆分版本 Debug 编译 WindowsInput.cpp、Window.cpp 和 main.cpp 并链接成功，退出码 0，无编译警告；差异检查通过，助手未启动程序。用户随后提供运行日志：连续尺寸变化后 dt=1113.1797 ms，之后恢复约 15–16 ms；缩放、最小化/恢复、普通左键、Alt+F4 与退出码 0 正常。已观察到长间隔，不将这段尺寸变化日志描述为专门的标题栏测试；拆分后 W/失焦、窗口外松开和 Esc 未另明确确认。用户要求直接提供理解题答案；已结合代码位置讲解 UseDefault、20000/1000000=0.02 秒及更新 previousCounter 的用途，用户表示明白，未记录为独立答题通过。

最新更新 dt 上限实现：只修改 main.cpp 的 main()，设置 maxUpdateDeltaSeconds=0.1，读取 rawDeltaSeconds=timer.deltaSeconds，并以条件表达式取实际间隔与上限中较小者作为 updateDeltaSeconds。实际时间超过上限时打印 Warning；约每秒的日志同时打印实际累计时间、实际 dt 和更新 dt。FrameTimer 与输入模块未修改；计数器起点照常每轮推进，totalSeconds 仍累计真实间隔，超出更新上限的时间不补跑。100 ms 是本步教学策略，不表示固定帧率或完整物理稳定性保障；当前只有计算和日志，尚无实际模拟消费 updateDeltaSeconds。
Debug 增量编译 main.cpp 并链接成功，退出码 0，无编译警告；差异检查通过，助手未运行程序，未重测 Release/全新目录。用户随后提供运行日志：普通间隔两者一致（约 15–16 ms），长帧实际 dt=4830.7974 ms、更新 dt=100 ms，恢复后重新一致；实际累计时间保留停顿，Alt+F4 正常结束，退出码 0。用户正确回答角色移动使用 updateDeltaSeconds、实际累计时间仍增加约 4.83 秒，本步运行与理解检查通过。

首次入口整理：main.cpp 缩为 19 行，运行流程迁入 SandboxApp.h/.cpp。当时 RunSandboxApp 持有窗口和计时器，消息泵、事件读取/清理及计时观察仍集中于该文件；用户随后要求继续分离职责，现行结构见下文。
InputEvents.h 定义 KeyEventType::Pressed/Released/Reset、KeyEvent 与 TextInputEvent；InputState 保存两个 vector。WindowsInput 在状态变化、w/W 字符、失焦时于已有异常边界内追加事件；状态清理仍在日志/分配之前，普通与系统键仍交默认处理。自动重复不重复产生按键变化事件，文字重复保留；Reset 表示逻辑取消，不伪装为物理松开。事件消费后仅清空列表，keysDown 不按帧清空。两个列表各自有序，不提供跨类别统一顺序；只处理 w/W，不代表完整 UTF-16/IME 支持；当前事件消费者只记录日志。
Debug 编译 WindowsInput.cpp、Window.cpp、SandboxApp.cpp、main.cpp 并链接成功，退出码 0，无编译警告。随后编译运行不创建窗口的 InputEventsSmoke 检查，退出码 0：快速按下/松开两事件保留、按键重复过滤、文本重复保留、失焦清表及 Reset、无效键码过滤、系统键 UseDefault 均通过。该检查仅直接调用消息处理函数，不验证真实 Windows 消息投递、捕获、Alt+F4 关闭或完整主循环；检查文件与产物保存在忽略的 out/validation/t03-events。EngineSandbox 未启动，未重测 Release/全新目录。
用户随后提供上一版本运行日志：左键拖动后 Esc 释放捕获、左键状态为 0；帧事件记录 Esc 的按下/松开，计时持续递增；Alt+F4 正常结束且退出码 0。本段没有覆盖 W/失焦、w/W、窗口外松开等全部回归，不据此标记 T03 完成。

最新模块整理：消息泵迁入 Window.cpp 的 PumpWindowMessages，平台层不依赖 Sandbox。新增 SandboxDiagnostics，只通过 const InputState& / const FrameTimer& 观察数据，私有成员保存上次观察到的 W 状态和上次日志时间；不修改输入、计时、不决定 dt 上限、不清理事件。InputState::ClearEvents 只清空两个事件列表，保留键盘与左键状态。RunSandboxApp 保留组件创建、100 ms 应用策略、调用顺序和清理；每轮观察完成后独立调用 ClearEvents，关闭诊断不改变事件生命周期。SandboxApp.cpp 从 133 行缩为 55 行，main 仍为 19 行。底层原有消息/错误日志仍使用 Core/Log，不依赖应用诊断组件；此步不是跨平台或独立动态库实现。
Debug 编译并链接成功，退出码 0，无编译警告；不创建窗口的 SandboxModuleSmoke 检查通过：诊断保留事件/持续状态/实际时间，ClearEvents 保留按住状态，关闭观察器仍能清理事件，清理后不重复输出输入事件，计时日志节流保持，消息泵空队列继续并能取得 WM_QUIT 的退出码 23。检查仅涉及测试进程自己的消息队列，未启动 EngineSandbox。源码检查未发现 Core/Platform 包含 Sandbox 头文件。测试与产物位于忽略的 out/validation/sandbox-modules，未重测 Release/全新目录。
最新版本用户反馈：进程 33740 的日志确认客户区外坐标 (-38, 346) 仍收到左键松开并清零、正常计时与 Alt+F4 退出码 0；该轮 Esc 晚于左键松开，因此未据此判定取消测试通过。进程 29084 的补测确认左键按下状态 1，Esc 释放捕获清零，帧事件按下/松开完整，最终左键松开仍为 0；取消后无新增拖动日志，地址两处相同，正常退出码 0。用户随后以“正常”确认 W/Shift+W 文本大小写与按住 W 切出清零、切回重新按键回归正常，未另贴键盘日志。
本轮必要输入回归已确认，缩放/最小化/恢复和长 dt 上限沿用此前明确证据，不描述为最新版本重新测试。2026-10-29 用户最终正确回答 ClearEvents 只清事件，持续按住 W 的状态仍为 true；结合此前只读诊断与清理独立性的回答，T03 最终理解检查及现有范围验收通过，详见 [T03 回归清单](T03Regression.md)。此次仅更新文档，没有新增构建或运行证据，改动未提交。下一步为 T04 调试层与适配器枚举。
