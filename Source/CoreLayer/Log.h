#pragma once
#include<string>
#include<filesystem>
#include<cstdint>

enum class LogLevel
{
	Debug,
	Info,
	Warning,
	Error
};

// 启动时设置；默认关闭详细日志，Info/Warning/Error 不受影响。
void SetDebugLoggingEnabled(bool enabled);
bool IsDebugLoggingEnabled();

// 输出日志；level 是日志级别，message 是消息内容。
void Log(LogLevel level,const std::string &message);

// 每条写入立即刷新用户态缓冲，退出前可读到最后一个检查点。
bool StartFileLog(const std::filesystem::path& path) noexcept;
std::filesystem::path CurrentLogPath();
void LogCheckpoint(const char* stage, const std::string& detail = {}, std::uint64_t frame = 0) noexcept;
bool SampleDiagnosticFrame(std::uint64_t frame) noexcept;
