# 文本键与语言表（首批接入）

2026-10-29：借鉴 Unity Localization 的“文本键 + 按语言查表”思路，实现最小 C++ 文本表。不是 Unity 资源格式兼容层。

## 范围和职责

| 文件 / 函数 | 职责 |
|---|---|
| `Source/Core/Localization.h/.cpp`：`Load`、`GetText` | 验证并加载 UTF-8 文本表，按键返回文字；无 Windows 依赖，不输出日志 |
| `Source/Platform/Windows/ExecutablePath.h/.cpp`：`GetExecutableDirectory` | 使用 Windows API 定位当前程序目录，不依赖进程工作目录 |
| `Source/Sandbox/main.cpp`：`main` | 解析 `--language`，默认 `zh-CN`，允许 `en-US`；错误参数退出码为 2 |
| `Source/Sandbox/SandboxApp.h/.cpp`：`RunSandboxApp` | 协调加载所选语言、失败时尝试英文；调用文本查询和现有日志接口 |
| `Resources/Localization/zh-CN.txt`、`en-US.txt` | 同一组键对应不同语言的文案 |
| `CMakeLists.txt`：`CopyLocalizationResources` | 构建时将两份资源复制到程序旁，资源单独修改后也会同步 |

`Log()` 不改动，继续负责输出已经准备好的字符串。不存在全局语言表，也不在消息循环中反复读文件。当前仅启动时选择语言，没有运行中切换语言的 UI。

## 资源格式

使用 UTF-8 的逐行 `键=文字` 格式，暂不引入 JSON 库。例：

```text
Window.MessageLoopStarted=窗口已显示，开始消息循环
Window.MessageLoopEnded=消息循环已结束
Timer.InitializeFailed=初始化帧计时器失败
Timer.UpdateFailed=更新帧计时器失败
```

- 键允许 ASCII 字母、数字、点、下划线和短横线；不可为空或重复。
- 第一处 `=` 分隔键和值；后续 `=` 是文字的一部分。值允许为空，空格原样保留。
- 支持 LF、CRLF、文件开头的 UTF-8 BOM；空行和以 `#` 开头的整行注释忽略。
- 当前不支持多行值、转义语法、占位符替换或复数规则。`\uXXXX` 在这个格式中不会被自动转换。
- 非法 UTF-8、重复键、非法行和空表视为加载失败。读取成功后才替换原表，失败保留原表。

## 关键代码

`Localization::GetText`：返回文本副本，避免换表后出现悬空引用。缺失键返回键名。

```cpp
std::string Localization::GetText(std::string_view key) const
{
    const auto found = texts_.find(std::string(key));
    return found == texts_.end() ? std::string(key) : found->second;
}
```

`Localization::Load`：先读取到 `contents`、验证 UTF-8，再逐行解析到临时映射 `loaded`。所有行验证成功才执行：

```cpp
texts_ = std::move(loaded);
return true;
```

`RunSandboxApp` 在开始消息循环前构造 `Localization`，使用程序目录下的 `Resources/Localization/<语言>.txt`。所选语言整表加载失败时尝试英文；单个键缺失时直接显示键名，不逐键查英文。目录查询失败时也直接显示键名。启动资源诊断使用英文，避免依赖尚未加载的语言表。

原来的中文调用替换为：

```cpp
Log(LogLevel::Info, localization.GetText("Window.MessageLoopStarted"));
Log(LogLevel::Error, localization.GetText("Timer.InitializeFailed"));
Log(LogLevel::Error, localization.GetText("Timer.UpdateFailed"));
Log(LogLevel::Info, localization.GetText("Window.MessageLoopEnded"));
```

这四处位于不同执行分支，不是连续执行的四条命令。

## 运行验证

从仓库根目录分别运行，每次关闭窗口后再运行下一条：

```powershell
.\out\build\windows-x64-debug\EngineSandbox.exe --language zh-CN
```

```powershell
.\out\build\windows-x64-debug\EngineSandbox.exe --language en-US
```

中文应显示“窗口已显示，开始消息循环”“消息循环已结束”；英文对应 `Window shown; starting message loop` 和 `Message loop ended`。其他窗口、输入、计时诊断以及 T02 练习文案尚未迁移，仍显示中文。两次均应正常关闭并退出 0。

修改源资源文字后需要构建以同步到输出目录，随后重启程序。分发时把输出目录内的 `Resources` 与 exe 一起复制。UTF-8 文本资源不等于 ASCII 中文编码；控制台编码与字体仍影响最终显示，Windows 宽字符 API 边界仍需单独转换。

## 验证记录与下一步

- Debug 增量编译、链接通过。
- 不创建窗口的检查通过：中英文查询、缺键回退、文件缺失、加载失败保留旧表、BOM/CRLF、重复键、非法键、空表、合法四字节 UTF-8 与非法 UTF-8、换表移除旧键、改变工作目录后程序目录查询保持一致。
- 用户已提供 `--language en-US` 运行日志，确认循环开始与结束两条文案来自英文表；窗口进入消息循环并输出结束日志。尚未提供该次 `$LASTEXITCODE` 和中文模式运行反馈，不据此推断退出码或其他平台已验证。
- 用户已确认切换语言不需要改变代码中的文本键，文本键与显示文字的基础理解检查通过。
- 后续按模块逐批迁移文案；含数值的文案需要先约定参数格式，本步不扩展格式化系统。
- T03 已于 2026-10-29 收尾：用户确认 `InputState::ClearEvents()` 只清事件、不清持续按住状态，最终理解检查通过。语言表补充项的待反馈范围仍按上文单独记录。
- 后续新增显示文案时，代码使用稳定文本键，中文表与英文表同步增加同键条目；注释、文档不纳入语言表，启动/底层故障诊断保留不依赖语言表的回退。
