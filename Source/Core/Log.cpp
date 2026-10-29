#include "Core/Log.h"
#include<iostream>

// 普通信息写入 cout，警告和错误写入 cerr。
void Log(LogLevel level,const std::string &message)
{
    switch (level)
    {
      case LogLevel::Info:
          std::cout << "[Info] " << message << '\n';
          break;
      case LogLevel::Warning:
          std::cerr << "[Warning] " << message << '\n';
          break;
      case LogLevel::Error:
          std::cerr << "[Error] " << message << '\n';
          break;
    }
}