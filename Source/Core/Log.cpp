#include "Core/Log.h"
#include<iostream>

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