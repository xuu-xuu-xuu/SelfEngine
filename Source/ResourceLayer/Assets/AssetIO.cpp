#include "AssetIO.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Log.h"
#include <algorithm>
#include <fstream>
#include <limits>
#include <Windows.h>
#include <bcrypt.h>

namespace se::asset_io
{
bool Read(const std::filesystem::path& path,std::vector<std::uint8_t>& bytes,std::stop_token stop)
{
    if(stop.stop_requested())return false;
    if(!stop.stop_possible())return Read(path,bytes);
    struct Handle{HANDLE value;~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
    Handle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED|FILE_FLAG_SEQUENTIAL_SCAN,nullptr)};
    if(file.value==INVALID_HANDLE_VALUE||GetFileType(file.value)!=FILE_TYPE_DISK)return false;
    LARGE_INTEGER size{};if(!GetFileSizeEx(file.value,&size)||size.QuadPart<0||size.QuadPart>MaxFileBytes)return false;
    Handle completed{CreateEventW(nullptr,TRUE,FALSE,nullptr)},cancelled{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    if(!completed.value||!cancelled.value)return false;
    std::stop_callback cancellation(stop,[event=cancelled.value]{SetEvent(event);});
    std::vector<std::uint8_t> candidate(static_cast<std::size_t>(size.QuadPart));
    std::size_t offset=0;
    while(offset<candidate.size())
    {
        if(stop.stop_requested())return false;
        const auto count=static_cast<DWORD>(std::min<std::size_t>(1024*1024,candidate.size()-offset));
        OVERLAPPED operation{};operation.Offset=static_cast<DWORD>(offset);operation.OffsetHigh=static_cast<DWORD>(offset>>32);operation.hEvent=completed.value;
        ResetEvent(completed.value);DWORD read=0;
        if(!ReadFile(file.value,candidate.data()+offset,count,nullptr,&operation))
        {
            if(GetLastError()!=ERROR_IO_PENDING)return false;
            HANDLE events[]{completed.value,cancelled.value};const auto wait=WaitForMultipleObjects(2,events,FALSE,10000);
            if(wait!=WAIT_OBJECT_0)
            {
                LogCheckpoint("Assets.Read.Cancel.Begin");CancelIoEx(file.value,&operation);
                // 完成确认前不能释放 OVERLAPPED/目标内存，内核仍可能正在访问它们。
                GetOverlappedResult(file.value,&operation,&read,TRUE);
                LogCheckpoint("Assets.Read.Cancel.End");return false;
            }
        }
        if(!GetOverlappedResult(file.value,&operation,&read,FALSE))return false;
        if(!read)return false;offset+=read;
    }
    if(stop.stop_requested())return false;bytes=std::move(candidate);return true;
}
std::string Utf8(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return { reinterpret_cast<const char*>(text.data()), text.size() };
}

std::filesystem::path FromUtf8(std::string_view text)
{
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

bool Within(const std::filesystem::path& root, const std::filesystem::path& path)
{
    std::error_code error;
    const auto normalizedRoot = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    const auto normalizedPath = std::filesystem::weakly_canonical(path, error);
    if (error) return false;
    auto candidate = normalizedPath.begin();
    for (auto component = normalizedRoot.begin(); component != normalizedRoot.end(); ++component, ++candidate)
    {
        if (candidate == normalizedPath.end()) return false;
        const auto left = component->wstring(), right = candidate->wstring();
        if (CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()),
            right.c_str(), static_cast<int>(right.size()), TRUE) != CSTR_EQUAL) return false;
    }
    return true;
}

bool Read(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return false;
    const auto length = file.tellg();
    if (length < 0 || static_cast<std::uint64_t>(length) > MaxFileBytes) return false;
    std::vector<std::uint8_t> loaded(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!loaded.empty() && !file.read(reinterpret_cast<char*>(loaded.data()), length)) return false;
    bytes = std::move(loaded);
    return true;
}

std::string NewId()
{
    unsigned char bytes[16]{};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(32);
    for (const auto byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    return result;
}

std::string Hash(std::span<const std::uint8_t> bytes)
{
    return se::Sha256(bytes);
}

bool WriteAtomic(const std::filesystem::path& path,std::span<const std::uint8_t> bytes,bool replaceExisting)
{
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const auto id = NewId();
    if (id.empty()) return false;
    auto temporary = path;
    temporary += ".tmp-" + id;
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool succeeded = true;
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        const DWORD count = static_cast<DWORD>((std::min)(bytes.size() - offset, std::size_t{ 1024 * 1024 }));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written != count)
        { succeeded = false; break; }
        offset += written;
    }
    if (succeeded && !FlushFileBuffers(file)) succeeded = false;
    if (!CloseHandle(file)) succeeded = false;
    if (succeeded)
        succeeded = MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH|(replaceExisting?MOVEFILE_REPLACE_EXISTING:0)) != FALSE;
    if (!succeeded) DeleteFileW(temporary.c_str());
    return succeeded;
}

bool ResolveReference(const std::filesystem::path& source, const std::filesystem::path& root,
    std::string uri, std::filesystem::path& absolute, std::string& relative)
{
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        return -1;
    };
    std::string decoded;
    for (std::size_t index = 0; index < uri.size(); ++index)
    {
        char value = uri[index];
        if (value == '%')
        {
            if (index + 2 >= uri.size() || hex(uri[index + 1]) < 0 || hex(uri[index + 2]) < 0) return false;
            value = static_cast<char>(hex(uri[index + 1]) * 16 + hex(uri[index + 2]));
            index += 2;
        }
        if (value == '\0' || value == ':') return false;
        decoded += value == '\\' ? '/' : value;
    }
    return ResolveRelative(source, root, decoded, absolute, relative);
}

bool ResolveRelative(const std::filesystem::path& source, const std::filesystem::path& root,
    const std::string& path, std::filesystem::path& absolute, std::string& relative)
{
    if (path.find('\0') != std::string::npos || path.find(':') != std::string::npos) return false;
    const auto local = FromUtf8(path);
    if (local.empty() || local.is_absolute() || local.has_root_name()) return false;
    absolute = std::filesystem::weakly_canonical(source.parent_path() / local);
    if (!Within(root, absolute)) return false;
    relative = Utf8(absolute.lexically_relative(std::filesystem::weakly_canonical(source.parent_path())));
    return !relative.empty();
}

void Diagnostic(std::vector<AssetDiagnostic>& diagnostics, const char* key, std::string detail, bool warning)
{
    diagnostics.push_back({ key, std::move(detail), warning });
}
}
