#pragma once

#include<Windows.h>
#include<string>

bool CheckHResult(HRESULT hr, const std::string& operation);
