#pragma once
#include<string>

enum class LogLevel
{
	Info,
	Warning,
	Error
};

void Log(LogLevel level,const std::string &message);
