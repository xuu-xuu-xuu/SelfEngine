#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace se
{
struct EditorScriptCreateRequest
{
    std::filesystem::path projectRoot;
    std::string className,nameSpace="Game";
};
struct EditorScriptCreateResult
{
    std::filesystem::path source,project;
    std::string scriptId;
    std::vector<std::filesystem::path> created,updated;
};
struct CodeEditorSettings
{
    std::filesystem::path executable;
    std::vector<std::wstring> arguments; // 每项是一个独立参数；支持 {file}/{line}/{column}。
};
struct EditorCodeOpenCommand
{
    bool useAssociation=false;
    std::filesystem::path executable,file,workingDirectory;
    std::vector<std::wstring> arguments;
};
class EditorScriptTools
{
public:
    static bool IsIdentifier(std::string_view value);
    static bool CreateBehaviour(const EditorScriptCreateRequest& request,EditorScriptCreateResult& result,
        std::string& errorKey,std::string& detail);
    static bool PrepareOpenSource(const std::filesystem::path& projectRoot,const std::filesystem::path& file,
        std::uint32_t line,std::uint32_t column,const CodeEditorSettings& settings,EditorCodeOpenCommand& command,
        std::string& errorKey,std::string& detail);
    static bool OpenSource(const std::filesystem::path& projectRoot,const std::filesystem::path& file,
        std::uint32_t line,std::uint32_t column,const CodeEditorSettings& settings,std::string& errorKey,std::string& detail);
};
}
