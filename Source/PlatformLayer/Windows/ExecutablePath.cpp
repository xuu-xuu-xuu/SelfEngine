#include "PlatformLayer/Windows/ExecutablePath.h"

#include <Windows.h>
#include <string>

std::filesystem::path GetExecutableDirectory()
{
    std::wstring buffer(256, L'\0');
    while (buffer.size() <= 32768)
    {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size())
        {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
    return {};
}
