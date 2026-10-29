#include "Platform/Windows/HResult.h"
#include "Core/Log.h"
#include<sstream>

// 用 FAILED 判断失败，不能把 HRESULT 直接当作 bool。
bool CheckHResult(HRESULT hr, const std::string& operation)
{
	if (FAILED(hr))
	{
		std::ostringstream message;
		message << operation << "失败，HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
		Log(LogLevel::Error, message.str());
		return false;
	}
	return true;
}