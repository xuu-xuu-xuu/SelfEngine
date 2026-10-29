# SelfEngine

面向 Windows x64 的实验性游戏引擎项目，使用 C++20 开发核心，计划接入 C# 行为脚本。

## 当前进度

当前仓库包含两个验证程序：

- `EngineSandbox`：使用 CMake、Ninja 和 MSVC 构建的 C++ 验证程序，保留 T02 练习，并已加入 T03 的 Win32 空窗口、部分键盘、文本与鼠标消息处理。
- `ManagedSandbox`：面向 .NET 10 和 Windows x64 的最小 C# 程序。

T01 基线中的两个程序均已在当前开发机上通过 Debug、Release 构建与运行检查，
并通过全新输出与中间目录的重新构建验证。

截至 2026-10-26，T02 的 RAII、移动与借用、日志等级、文件读取结果枚举、
断言、HRESULT 诊断、COM 引用与异常边界练习已通过运行和理解检查，T02 验收通过。
`RunComPtrExample()` 已从主流程提取；同进程 C 接口练习在内部捕获 C++ 异常并返回错误码。
T02 收尾版本的正常路径已通过 Debug、Release 和全新 Debug 构建目录验证，退出码均为 0；
Debug 下的两类异常转换路径均能清理资源，由调用者以退出码 1 结束程序。
验证范围与结果见 [T02 验收记录](Docs/Environment.md#10-t02-验收记录)。

T03 已于 2026-10-29 在单窗口、基础输入与计时范围内验收通过；最终确认 ClearEvents 只清事件而保留按住状态。下一步进入 T04 的 D3D12 调试层与适配器枚举。以下保留 T03 各小步实施过程，最终证据见 [T03 验收记录](Docs/T03Regression.md)。截至 2026-10-28，用户已在 Debug 下验证窗口显示、
移动、调整尺寸、最小化与恢复、焦点切换和正常关闭；键盘状态表可过滤重复状态日志并在失焦时整表清除，
文字消息可区分小写 w 与大写 W，相关理解检查通过。
已验证鼠标左键在客户区内按下、松开时分别记录当时的坐标，并通过相关理解检查。
当前仍是单窗口、普通与系统键盘消息、Esc 取消、两个英文字符及左键消息的练习。窗口生命周期位于 `Source/Platform/Windows/Window.h/.cpp`，输入消息处理位于 `WindowsInput.h/.cpp`，输入数据位于 `Source/Platform/InputState.h`；应用循环已提取到 `Source/Sandbox/SandboxApp.cpp`，main 只负责启动、调用与返回退出码。
加入捕获前已观察到窗口外松开缺少对应消息；现在已加入按下时捕获、松开时释放，
并验证能收到客户区右侧之外的松开消息，相关理解检查通过。
已观察主动释放时的 `WM_CAPTURECHANGED` 通知，并通过日志顺序与捕获含义的理解检查。
已保存左键按下状态，并在失去捕获时清除；窗口内点击和窗口外松开的正常路径已验证状态从 1 回到 0。
已验证左键仍按住时按 Esc 主动释放捕获，由捕获变化分支清零状态；后续普通点击仍正常。
已验证仅在保存的左键状态为 true 时记录移动坐标，Esc 取消后停止记录，松开并重新按下后恢复。
已将 W 键变量扩展为键盘状态表，验证 W/A 独立记录、Esc 取消，以及按住 A 失焦后清表、切回重新按下正常；理解检查通过。
已将系统按键消息接入同一状态表，验证普通 W、Alt+W 的按下与松开，以及 Alt+F4 正常关闭；相关理解检查通过。
已改为 `PeekMessageW` 两层循环，修正循环末尾位置后验证 Alt+F4 正常退出；相关理解检查通过。当前已接入基础计时，尚无实际游戏更新。
T02 练习已提取为 `RunT02Examples()`，由 `main()` 检查返回值后继续启动窗口；正常与模拟失败路径及理解检查通过。
该函数及其辅助代码已迁入 `Source/Sandbox/T02Examples.h/.cpp`，迁移后正常运行及声明、实现、链接和工作目录的理解检查通过。
2026-10-29 已讲明窗口模块职责与数据归属，并通过句柄复制、游戏更新位置与状态寿命的理解检查。
已在 `main.cpp` 定义 `InputState`，此前由窗口过程中的静态对象集中保存键盘与左键状态；该阶段日志确认 Esc 取消后左键清零及正常退出，用户确认 W/A 与失焦操作正常，类型、实例、生命周期和作用域的理解检查通过。
已定义包含 `HWND` 与 `InputState` 的 `WindowState`，由 `main()` 持有并用其句柄创建、显示窗口；调整结构体定义顺序后正常启动和关闭，退出码为 0。补讲成员包含、局部对象寿命与两份输入状态的区别后，本小步理解检查通过。
窗口创建参数的概念检查通过：已区分对象地址与 HWND，理解通过地址访问原对象及创建期间即可回调。已传入 `&window` 并在 `WM_NCCREATE` 中取回，用户日志确认同次运行的两个地址相同、发生在消息循环开始前，正常关闭退出码为 0；补讲成员中保存的地址与创建信息自身地址的区别后，地址取回小步的理解检查通过。
窗口状态关联与输入迁移现已实现：创建时将对象地址保存到 `GWLP_USERDATA`，后续消息取回地址并引用同一份 `window.input`，旧静态副本已移除；销毁时解除关联并清空句柄与输入。
主循环只在 W 状态变化时记录读取结果。关联与输入迁移版本的 Debug 增量编译和链接通过；用户日志已验证 W 按下、松开及失焦清零能被主循环读到，Esc 取消后左键清零，Alt+F4 正常退出且退出码为 0。地址持久关联与引用/副本的理解检查通过；该轮未单独提供窗口外松开的回归证据。
2026-10-29 已拆出窗口模块：头文件提供状态类型和创建、销毁函数，WindowProc 原样迁入实现文件的匿名命名空间；main 继续持有状态与运行主循环。Debug 构建通过；用户日志验证缩放、最小化/恢复、焦点及 Alt+F4 退出码 0，并文字确认地址、W/失焦、窗口外松开和 Esc 取消两组测试正常；补讲后确认函数返回由 main 决定是否退出，核心理解检查通过。
新增 `Source/Platform/Windows/FrameTimer.h/.cpp`：用性能计数器测量相邻更新的秒数、累计时间和更新帧数，主循环约每秒输出一次计时日志。Debug 构建通过；用户日志验证时间和帧数持续递增、W 失焦清零及退出码 0。输入拆分后又观察到尺寸变化后的 1113.1797 ms 长 dt，随后恢复约 15–16 ms；相关答案已应用户要求集中讲解，用户表示明白。
按用户要求进一步拆出输入职责：WindowsInput 只借用 HWND 与 InputState，不依赖 WindowState；以 Handled/UseDefault 交回消息处理结果，保留键盘默认行为。输入分支仅转换 6 处返回值，其他正文比对一致。拆分版 Debug 构建通过，用户日志覆盖缩放、最小化/恢复、普通左键和 Alt+F4 退出码 0；拆分后的 W/失焦、窗口外松开与 Esc 未另提供明确反馈。
计时上限小步在 main 中保留实际 dt，并将供后续游戏更新使用的 dt 限制到最多 0.1 秒；日志同时显示两者，实际累计时间不截断。Debug 构建通过；用户日志验证实际 dt=4830.7974 ms 时更新 dt=100 ms，恢复后两者重新一致，Alt+F4 退出码 0。用户正确区分角色更新使用 updateDeltaSeconds、实际累计时间仍增加约 4.83 秒，本步运行和理解检查通过。当前没有实际游戏更新、固定步长或补跑逻辑。
详细范围见 [T03 阶段进度](Docs/Environment.md#t03-progress)。

已整理入口并增加 KeyEvent（按下/松开/重置）和 TextInputEvent 两类事件，消息处理时追加，应用每轮读取后清空，保留持续按键状态。文本仍只观察 w/W。用户日志已覆盖 Esc 按下/松开事件、捕获取消清零、持续计时和 Alt+F4 退出码 0，其余输入回归尚缺反馈。
按用户要求进一步分离职责：Windows 窗口模块提供消息泵，SandboxDiagnostics 只读观察输入/计时，InputState::ClearEvents 独立清理事件，SandboxApp 只协调启动、更新策略和退出。该拆分时 main 为 19 行、SandboxApp.cpp 从 133 行缩为 55 行（后续语言表接入另有增加）。Debug 构建和不创建窗口的模块边界检查通过；用户日志确认地址一致、窗口外松开、Esc 取消、持续计时和正常退出，并文字确认 W/Shift+W 与失焦回归正常。职责边界最终理解检查已通过，见 [T03 回归清单](Docs/T03Regression.md)。

渲染和 C++/C# 互调仍处于规划阶段，尚未实现。

2026-10-29 新增独立文本表组件：代码以 ASCII 文本键查询 UTF-8 语言资源，启动参数 `--language zh-CN` / `--language en-US` 选择语言（默认中文）。首批仅迁移应用循环开始、结束及两条计时失败日志。Debug 构建与无窗口检查通过，用户英文日志已验证启动与结束文案，文本键基础理解检查通过；中文交互反馈和该次退出码待补充，详见 [语言表说明](Docs/Localization.md)。

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

C++ 程序输出练习日志后会显示空窗口，点击该窗口的关闭按钮后应以退出码 `0` 结束。
C# 程序应显示当前进程为 64 位，并以退出码 `0` 结束。

Release 构建和全新目录验证步骤见[开发环境与构建说明](Docs/Environment.md)。

## 目录与文件

- `Source/Sandbox/`：C++ 验证程序及当前 Win32 窗口练习。
- `Source/Core/`：最小日志设施。
- `Source/Platform/Windows/`：Windows HRESULT 检查与日志诊断。
- `Source/ManagedSandbox/`：C# 控制台验证程序。
- `Docs/`：开发文档。
- `CMakeLists.txt`：原生程序的构建目标和编译要求。
- `CMakePresets.json`：项目共享的原生构建配置。
- `global.json`：.NET SDK 版本选择。
