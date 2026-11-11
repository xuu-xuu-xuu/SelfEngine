#include "ImGuiEditor.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <set>
#include <cstdio>
#include <fstream>
#include <regex>
#include <Windows.h>
#include <cmath>
#include <system_error>
#include <vector>

namespace se
{
namespace
{
using Json=nlohmann::json;
std::filesystem::path EnvironmentPath(const wchar_t* name)
{
    const auto length=GetEnvironmentVariableW(name,nullptr,0);if(!length)return {};
    std::wstring value(length,L'\0');const auto copied=GetEnvironmentVariableW(name,value.data(),length);
    if(!copied||copied>=length)return {};value.resize(copied);return std::filesystem::path(value);
}
std::string Lower(std::string value)
{std::transform(value.begin(),value.end(),value.begin(),[](unsigned char ch){return static_cast<char>(std::tolower(ch));});return value;}
bool SourceFile(const std::filesystem::path& path,const Json& status)
{
    const auto name=Lower(asset_io::Utf8(path.filename()));
    for(const auto& extension:status.value("sourceExtensions",std::vector<std::string>{}))if(name.ends_with(Lower(extension)))return true;
    for(const auto& filename:status.value("sourceFileNames",std::vector<std::string>{}))if(name==Lower(filename))return true;
    return false;
}
}

void ImGuiEditor::ConfigureBuildControl()
{
    restartRequestPath_=EnvironmentPath(L"SELFENGINE_EDITOR_RESTART_REQUEST");
    const auto statusPath=EnvironmentPath(L"SELFENGINE_EDITOR_BUILD_STATUS");
    buildDiagnosticsPath_=EnvironmentPath(L"SELFENGINE_EDITOR_BUILD_DIAGNOSTICS");
    if(restartRequestPath_.empty())return;
    if(!asset_io::Within(projectRoot_/"out"/"Editor",restartRequestPath_)||restartRequestPath_.filename()!=L"restart.json"
        ||statusPath.parent_path()!=restartRequestPath_.parent_path())
    {restartRequestPath_.clear();buildStale_=true;LogConsole(2,"Editor.BuildControlInvalid");return;}
    std::vector<std::uint8_t> bytes;
    if(!asset_io::Read(statusPath,bytes)){buildStale_=true;LogConsole(2,"Editor.BuildControlInvalid");return;}
    try
    {
        buildStatusJson_.assign(bytes.begin(),bytes.end());const auto status=Json::parse(buildStatusJson_);
        if(status.value("schemaVersion",0)!=1)throw std::runtime_error("schema");
        buildHasCandidate_=status.value("hasCandidate",false);buildStale_=status.value("state",std::string{})!="ready";
        if(buildStale_)LogConsole(2,status.value("diagnosticKey",std::string("Editor.BuildRequired")),asset_io::Utf8(buildDiagnosticsPath_));
        if(buildStale_&&!buildDiagnosticsPath_.empty())
        {
            std::ifstream log(buildDiagnosticsPath_,std::ios::binary);std::string line;
            const std::regex diagnostic(R"(^\s*(.+\.(?:cs|csproj))\((\d+),(\d+)\):\s*(.*))",std::regex::icase);
            while(std::getline(log,line)&&buildDiagnostics_.size()<200)
            {
                std::smatch match;
                if(std::regex_search(line,match,diagnostic))
                {
                    const auto file=asset_io::FromUtf8(match[1].str());
                    if(asset_io::Within(projectRoot_,file))buildDiagnostics_.push_back({file,static_cast<std::uint32_t>(std::stoul(match[2].str())),static_cast<std::uint32_t>(std::stoul(match[3].str())),match[4].str()});
                }
            }
            showBuildDiagnostics_=!buildDiagnostics_.empty();
        }
    }
    catch(...){buildStale_=true;buildStatusJson_.clear();LogConsole(2,"Editor.BuildControlInvalid");}
}

bool ImGuiEditor::ValidateBuildSources()
{
    if(allowStaleBuild_)return true;
    if(buildStale_)return false;
    if(buildStatusJson_.empty())return true;
    try
    {
        const auto status=Json::parse(buildStatusJson_);
        BuildSettings currentBuild;std::string buildError;
        if(!buildHasCandidate_&&std::filesystem::exists(projectRoot_/"build.json"))
        {
            if(!ProjectSettingsService::ReadBuild(projectRoot_/"build.json",currentBuild,buildError)||!currentBuild.scriptsProject.empty())
            {buildStale_=true;return false;}
        }
        std::set<std::string> expected,observed;
        for(const auto& source:status.value("sourceFiles",Json::array()))
        {
            const auto relative=source.at("path").get<std::string>();const auto path=projectRoot_/asset_io::FromUtf8(relative);
            std::vector<std::uint8_t> bytes;
            if(!asset_io::Within(projectRoot_,path)||!asset_io::Read(path,bytes)||Lower(asset_io::Hash(bytes))!=Lower(source.at("sha256").get<std::string>()))
            {buildStale_=true;return false;}
            expected.insert(Lower(relative));
        }
        for(const auto& directory:status.value("sourceDirectories",std::vector<std::string>{}))
        {
            const auto path=(projectRoot_/asset_io::FromUtf8(directory)).lexically_normal();
            if(!asset_io::Within(projectRoot_,path)){buildStale_=true;return false;}
            std::error_code error;
            for(std::filesystem::recursive_directory_iterator it(path,error),end;!error&&it!=end;it.increment(error))
            {
                const auto name=Lower(asset_io::Utf8(it->path().filename()));
                if(it->is_directory(error)&&(name=="out"||name=="bin"||name=="obj"||name==".git"||name==".vs"||name==".cache")){it.disable_recursion_pending();continue;}
                if(it->is_symlink(error)){it.disable_recursion_pending();buildStale_=true;return false;}
                if(it->is_regular_file(error)&&SourceFile(it->path(),status))
                    observed.insert(Lower(asset_io::Utf8(it->path().lexically_normal().lexically_relative(projectRoot_.lexically_normal()).generic_wstring())));
            }
            if(error){buildStale_=true;return false;}
        }
        if(expected!=observed){buildStale_=true;return false;}
        return true;
    }
    catch(...){buildStale_=true;return false;}
}

void ImGuiEditor::BuildControlToolbar(EditorSession& editor,AssetStore&,UiTree*,const Localization& text)
{
    if(restartRequestPath_.empty())return;
    ImGui::SameLine();ImGui::BeginDisabled(editor.IsPlaying()||FileDialogActive()||ExportBusy()||gizmo_.Active()||inspectorEditing_||material_.Editing());
    if(ImGui::Button(Label(text,"Editor.CompileScripts").c_str()))
    {
        Queue([this]{const auto result=ExecuteCommand("editor.compile");if(result.state!=EditorCommandResult::Succeeded)LogConsole(2,result.diagnosticKey,result.detail);});
    }
    ImGui::EndDisabled();
    if(buildStale_)
    {
        ImGui::SameLine();ImGui::TextUnformatted(text.GetText(allowStaleBuild_?"Editor.RunningLastBuild":"Editor.BuildRequired").c_str());
        if(buildHasCandidate_&&!allowStaleBuild_)
        {ImGui::SameLine();if(ImGui::Button(Label(text,"Editor.UseLastBuild").c_str()))Queue([this]{ExecuteCommand("editor.use-last-build");});}
        if(!buildDiagnosticsPath_.empty())
        {ImGui::SameLine();if(ImGui::Button(Label(text,"Editor.BuildDiagnostics").c_str()))showBuildDiagnostics_=true;}
    }
}

bool ImGuiEditor::PublishRestartRequest()
{
    if(!recompileRequested_)return true;
    if(!CloseApproved()||restartRequestPath_.empty())return false;
    const std::string request="{\"schemaVersion\":1,\"action\":\"compile\"}\n";
    if(!asset_io::WriteAtomic(restartRequestPath_,{reinterpret_cast<const std::uint8_t*>(request.data()),request.size()},false))
    {LogConsole(2,"Editor.BuildControlInvalid");return false;}
    return true;
}

void ImGuiEditor::ConfigurePreferences(const EditorPreferences& preferences,std::filesystem::path directory,std::function<bool(const std::string&)> language)
{
    preferences_=preferences;preferencesDirectory_=std::move(directory);changeLanguage_=std::move(language);
    std::snprintf(externalEditorField_.data(),externalEditorField_.size(),"%s",preferences.externalEditor.c_str());
    std::snprintf(colorLutField_.data(),colorLutField_.size(),"%s",preferences.colorLut.c_str());
    colorLutScanned_=false;
    colorLutAssets_.clear();
    colorLutPreview_.Close();
    colorLutStatus_.clear();
}
std::filesystem::path ImGuiEditor::ColorLutPath() const
{
    if(preferences_.colorLut.empty())return {};
    auto path=asset_io::FromUtf8(preferences_.colorLut);
    if(path.is_relative()&&!projectRoot_.empty())path=projectRoot_/path;
    return path;
}
void ImGuiEditor::PreferencesPanel(const Localization& text)
{
    ImGui::SetNextWindowSize({620,520},ImGuiCond_FirstUseEver);
    if(ImGui::Begin(Label(text,"Editor.Preferences").c_str(),&showPreferences_))
    {
        int language=preferences_.language=="en-US"?1:0;
        const auto chinese=text.GetText("Editor.PreferencesChinese"),english=text.GetText("Editor.PreferencesEnglish");
        const char* languages[]{chinese.c_str(),english.c_str()};
        if(ImGui::Combo(Label(text,"Editor.PreferencesLanguage").c_str(),&language,languages,2))preferences_.language=language?"en-US":"zh-CN";
        ImGui::SliderFloat(Label(text,"Editor.PreferencesScale").c_str(),&preferences_.uiScale,0.75f,2.0f,"%.2f");
        ImGui::Checkbox(Label(text,"Editor.PreferencesRestore").c_str(),&preferences_.restoreLastProject);
        ImGui::InputText(Label(text,"Editor.PreferencesCodeEditor").c_str(),externalEditorField_.data(),externalEditorField_.size());
        ImGui::TextWrapped("%s",text.GetText("Editor.PreferencesCodeEditorHint").c_str());
        ColorLutSection(text,projectRoot_);
        if(ImGui::Button(Label(text,"EditorUi.Save").c_str()))
        {
            preferences_.externalEditor=externalEditorField_.data();
            preferences_.colorLut=colorLutField_.data();
            std::string error;
            if(EditorSessionStore::SavePreferences(preferencesDirectory_,preferences_,error))
            {
                SetUiScale(preferences_.uiScale,dpiScale_);
                if(changeLanguage_&&!changeLanguage_(preferences_.language))errorKey_="Editor.PreferencesLanguageFailed";
                else{errorKey_.clear();LogConsole(0,"Editor.PreferencesSaved");}
            }
            else{errorKey_=error;LogConsole(1,error);}
        }
    }
    ImGui::End();
}
void ImGuiEditor::ColorLutSection(const Localization& text,const std::filesystem::path& root)
{
    ImGui::SeparatorText(Label(text,"EditorUi.ColorLut").c_str());
    const auto scan=[&]
    {
        colorLutAssets_.clear();
        std::error_code error;
        if(root.empty()||!std::filesystem::is_directory(root,error))return;
        std::size_t visited=0;
        std::filesystem::recursive_directory_iterator iterator(root,
            std::filesystem::directory_options::skip_permission_denied,error),end;
        while(!error&&iterator!=end&&visited++<20000)
        {
            const auto& entry=*iterator;
            std::error_code entryError;
            if(entry.is_directory(entryError))
            {
                const auto name=entry.path().filename().string();
                if(name==".git"||name==".vs"||name=="out"||name=="build"||name=="build-g"||name=="Cache"||name=="ThirdParty")
                    iterator.disable_recursion_pending();
            }
            else if(entry.is_regular_file(entryError)&&entry.path().extension()==".selut")
            {
                const auto relative=std::filesystem::relative(entry.path(),root,entryError);
                if(!entryError)colorLutAssets_.push_back(relative);
                if(colorLutAssets_.size()>=256)break;
            }
            iterator.increment(error);
        }
        std::sort(colorLutAssets_.begin(),colorLutAssets_.end());
        colorLutScanned_=true;
    };
    if(!colorLutScanned_)scan();
    const auto open=[&](const std::filesystem::path& relativeOrAbsolute)
    {
        auto resolved=relativeOrAbsolute;
        std::error_code ignored;
        if(!resolved.empty()&&resolved.is_relative()&&!root.empty())resolved=root/resolved;
        std::string error;
        if(resolved.empty()){colorLutPreview_.Close();colorLutStatus_.clear();return;}
        colorLutPreview_.Open(resolved,error);
        colorLutSeenFailure_=colorLutPreview_.Status().failures;
        colorLutStatus_=colorLutPreview_.Ready()?text.GetText("EditorUi.ColorLutReady")
            :text.GetText(error.empty()?"Rendering.LutAssetInvalid":error);
    };
    // 选择列表只显示项目内的 `.selut`；路径输入仍允许项目外/尚未存在的文件。
    const auto current=std::string(colorLutField_.data());
    const auto comboPreview=current.empty()?text.GetText("EditorUi.ColorLutEmpty"):current;
    if(ImGui::BeginCombo(Label(text,"EditorUi.ColorLutBrowse").c_str(),comboPreview.c_str()))
    {
        if(colorLutAssets_.empty())ImGui::TextUnformatted(text.GetText("EditorUi.ColorLutScanEmpty").c_str());
        for(const auto& asset:colorLutAssets_)
        {
            const auto label=asset_io::Utf8(asset);
            if(ImGui::Selectable(label.c_str(),current==label))
            {
                std::snprintf(colorLutField_.data(),colorLutField_.size(),"%s",label.c_str());
                open(asset);
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if(ImGui::Button(Label(text,"EditorUi.ColorLutRefresh").c_str()))scan();
    ImGui::InputText(Label(text,"EditorUi.ColorLutPath").c_str(),colorLutField_.data(),colorLutField_.size());
    if(ImGui::Button(Label(text,"EditorUi.ColorLutReload").c_str()))open(asset_io::FromUtf8(colorLutField_.data()));
    ImGui::TextWrapped("%s",text.GetText("EditorUi.ColorLutHint").c_str());
    // 面板可见时按文件时间戳热重载；预览与状态使用同一份 CPU 晶格。
    if(!colorLutPreview_.Path().empty())
    {
        const auto previousRevision=colorLutPreview_.Status().revision;
        colorLutPreview_.Poll();
        const auto& status=colorLutPreview_.Status();
        if(status.revision!=previousRevision)
            colorLutStatus_=text.GetText("EditorUi.ColorLutReloaded");
        else if(status.failures!=colorLutSeenFailure_)
        {
            colorLutSeenFailure_=status.failures;
            colorLutStatus_=text.GetText("Rendering.LutReloadFailed");
        }
    }
    if(colorLutPreview_.Ready())
    {
        const auto& status=colorLutPreview_.Status();
        ImGui::Text("%s: %u^3, %s, revision %llu, failures %llu",colorLutStatus_.empty()?"-":colorLutStatus_.c_str(),
            status.size,status.colorSpace==se::ColorLutColorSpace::Srgb?"sRGB":"Linear",
            static_cast<unsigned long long>(status.revision),static_cast<unsigned long long>(status.failures));
        ImGui::TextUnformatted(text.GetText("EditorUi.ColorLutPreview").c_str());
        const auto origin=ImGui::GetCursorScreenPos();
        const auto available=(std::max)(ImGui::GetContentRegionAvail().x,64.0f);
        const float height=40;
        auto* draw=ImGui::GetWindowDrawList();
        // 灰阶：显示线性 0..1 经过 LUT 后的 sRGB 结果。
        for(int column=0;column<256;++column)
        {
            const auto t=static_cast<float>(column)/255.0f;
            const auto linear=colorLutPreview_.Lut().Sample({t,t,t});
            const auto srgb=se::LinearToSrgb(linear);
            const ImU32 color=IM_COL32(static_cast<int>(std::clamp(srgb.x,0.0f,1.0f)*255),
                static_cast<int>(std::clamp(srgb.y,0.0f,1.0f)*255),static_cast<int>(std::clamp(srgb.z,0.0f,1.0f)*255),255);
            const auto left=origin.x+available*static_cast<float>(column)/256.0f;
            const auto right=origin.x+available*static_cast<float>(column+1)/256.0f;
            draw->AddRectFilled({left,origin.y},{right,origin.y+height},color);
        }
        ImGui::Dummy({available,height});
        ImGui::Dummy({0,4});
        // 彩色网格：8 个色相 × 3 个亮度，直观看到 LUT 对通道的交叉影响。
        const auto swatchOrigin=ImGui::GetCursorScreenPos();
        const float swatchWidth=available/8.0f,swatchHeight=20;
        for(int row=0;row<3;++row)for(int column=0;column<8;++column)
        {
            const float hue=static_cast<float>(column)/8.0f;
            const float value=0.25f+0.3f*static_cast<float>(row);
            const auto input=se::Vec3{
                value*(1.0f-std::clamp(std::abs(hue*6.0f-3.0f)-1.0f,0.0f,1.0f)),
                value*(1.0f-std::clamp(std::abs(hue*6.0f-2.0f)-1.0f,0.0f,1.0f)),
                value*(1.0f-std::clamp(std::abs(hue*6.0f-4.0f)-1.0f,0.0f,1.0f))};
            const auto srgb=se::LinearToSrgb(colorLutPreview_.Lut().Sample(input));
            const ImU32 color=IM_COL32(static_cast<int>(std::clamp(srgb.x,0.0f,1.0f)*255),
                static_cast<int>(std::clamp(srgb.y,0.0f,1.0f)*255),static_cast<int>(std::clamp(srgb.z,0.0f,1.0f)*255),255);
            draw->AddRectFilled({swatchOrigin.x+swatchWidth*column,swatchOrigin.y+swatchHeight*row},
                {swatchOrigin.x+swatchWidth*(column+1),swatchOrigin.y+swatchHeight*(row+1)},color);
        }
        ImGui::Dummy({available,swatchHeight*3});
    }
    else if(!colorLutStatus_.empty())ImGui::TextColored({0.95f,0.45f,0.4f,1}, "%s",colorLutStatus_.c_str());
    else ImGui::TextUnformatted(text.GetText("EditorUi.ColorLutEmpty").c_str());
}
void ImGuiEditor::BuildDiagnosticsPanel(const Localization& text)
{
    ImGui::SetNextWindowSize({700,360},ImGuiCond_FirstUseEver);
    if(ImGui::Begin(Label(text,"Editor.BuildDiagnostics").c_str(),&showBuildDiagnostics_))
    {
        if(ImGui::Button(Label(text,"Editor.OpenBuildLog").c_str()))OpenInExplorer(buildDiagnosticsPath_,true);
        for(std::size_t i=0;i<buildDiagnostics_.size();++i)
        {
            const auto& diagnostic=buildDiagnostics_[i];ImGui::PushID(static_cast<int>(i));
            ImGui::TextWrapped("%s (%u,%u): %s",asset_io::Utf8(diagnostic.file.lexically_relative(projectRoot_)).c_str(),diagnostic.line,diagnostic.column,diagnostic.message.c_str());
            if(ImGui::SmallButton(Label(text,"Editor.OpenCodeLocation").c_str()))OpenCodeSource(diagnostic.file,diagnostic.line,diagnostic.column);
            ImGui::PopID();
        }
    }
    ImGui::End();
}
}
