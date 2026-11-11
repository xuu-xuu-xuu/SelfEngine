#include "PlatformLayer/Graphics/D3D12/D3D12Result.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include <sstream>

bool CheckGraphicsResult(HRESULT result, const Localization& localization,
    std::string_view errorKey, LogLevel level)
{
    if (SUCCEEDED(result)) return true;

    std::ostringstream message;
    message << localization.GetText(errorKey) << " (HRESULT=0x"
        << std::hex << static_cast<unsigned long>(result) << ')';
    Log(level, message.str());
    return false;
}
