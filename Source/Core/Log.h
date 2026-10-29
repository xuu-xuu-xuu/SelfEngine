#pragma once
#include<string>

enum class LogLevel
{
	Info,
	Warning,
	Error
};

// 输出日志；level 是日志级别，message 是消息内容。
void Log(LogLevel level,const std::string &message);
