#pragma once
#include<string>

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
