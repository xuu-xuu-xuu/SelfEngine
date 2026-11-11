#include "PlatformLayer/Graphics/D3D12/D3D12ShaderProgram.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Utf.h"
#include <iostream>
#include <string_view>

int wmain(int argc,wchar_t** argv)
{
    Localization text;std::string error;
    // 启动期语言表损坏时的独立回退，不依赖尚未可用的键。
    if(!text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error))
    {std::cerr<<"ShaderBuilder could not load localization: "<<error<<'\n';return 1;}
    if(argc<4||(std::wstring_view(argv[1])!=L"mesh"&&std::wstring_view(argv[1])!=L"triangle"&&std::wstring_view(argv[1])!=L"pbr"&&std::wstring_view(argv[1])!=L"forward"&&std::wstring_view(argv[1])!=L"output"&&std::wstring_view(argv[1])!=L"sky"
        &&std::wstring_view(argv[1])!=L"shadowed"&&std::wstring_view(argv[1])!=L"shadowdepth"&&std::wstring_view(argv[1])!=L"shadowoutput"
        &&std::wstring_view(argv[1])!=L"environment"&&std::wstring_view(argv[1])!=L"environmentsky"&&std::wstring_view(argv[1])!=L"pbr-instanced"
        &&std::wstring_view(argv[1])!=L"pbr-forward-instanced"
        &&std::wstring_view(argv[1])!=L"bloomextract"&&std::wstring_view(argv[1])!=L"bloomblur"&&std::wstring_view(argv[1])!=L"tonemap"&&std::wstring_view(argv[1])!=L"fxaa"&&std::wstring_view(argv[1])!=L"overlay"&&std::wstring_view(argv[1])!=L"gui"&&std::wstring_view(argv[1])!=L"user-pbr"&&std::wstring_view(argv[1])!=L"basiclit"))
    {Log(LogLevel::Error,text.GetText("ShaderBuilder.Usage"));return 1;}
    se::rhi::PipelineDescription description;
    const bool userPbr=std::wstring_view(argv[1])==L"user-pbr";
    description.kind=std::wstring_view(argv[1])==L"pbr"?se::rhi::PipelineKind::PbrMesh:std::wstring_view(argv[1])==L"mesh"?se::rhi::PipelineKind::TexturedMesh:se::rhi::PipelineKind::Triangle;
    if(userPbr)description.kind=se::rhi::PipelineKind::PbrMesh;
    if(std::wstring_view(argv[1])==L"forward")description.kind=se::rhi::PipelineKind::ForwardMesh;
    if(std::wstring_view(argv[1])==L"output")description.kind=se::rhi::PipelineKind::FinalOutput;
    if(std::wstring_view(argv[1])==L"sky")description.kind=se::rhi::PipelineKind::Sky;
    if(std::wstring_view(argv[1])==L"basiclit")description.kind=se::rhi::PipelineKind::BasicLitMesh;
    if(std::wstring_view(argv[1])==L"shadowed")description.kind=se::rhi::PipelineKind::ShadowedMesh;
    if(std::wstring_view(argv[1])==L"shadowdepth")description.kind=se::rhi::PipelineKind::ShadowDepth;
    if(std::wstring_view(argv[1])==L"shadowoutput")description.kind=se::rhi::PipelineKind::ShadowOutput;
    if(std::wstring_view(argv[1])==L"environment")description.kind=se::rhi::PipelineKind::EnvironmentMesh;
    if(std::wstring_view(argv[1])==L"environmentsky")description.kind=se::rhi::PipelineKind::EnvironmentSky;
    if(std::wstring_view(argv[1])==L"bloomextract")description.kind=se::rhi::PipelineKind::BloomExtract;
    if(std::wstring_view(argv[1])==L"bloomblur")description.kind=se::rhi::PipelineKind::BloomBlur;
    if(std::wstring_view(argv[1])==L"tonemap")description.kind=se::rhi::PipelineKind::ToneMap;
    if(std::wstring_view(argv[1])==L"fxaa")description.kind=se::rhi::PipelineKind::Fxaa;
    if(std::wstring_view(argv[1])==L"overlay")description.kind=se::rhi::PipelineKind::UiOverlay;
    if(std::wstring_view(argv[1])==L"pbr-instanced")description.kind=se::rhi::PipelineKind::PbrInstancedMesh;
    if(std::wstring_view(argv[1])==L"pbr-forward-instanced")description.kind=se::rhi::PipelineKind::PbrForwardInstancedMesh;
    if(std::wstring_view(argv[1])==L"gui")description.kind=se::rhi::PipelineKind::Gui;
    description.bindingLayoutVersion=se::rhi::BindingLayout(description.kind);
    description.shader=argv[2];
    for(int index=4;index<argc;++index)
    {
        std::u16string units;for(const wchar_t c:std::wstring_view(argv[index]))units+=static_cast<char16_t>(c);
        std::u32string decoded;std::string macro;
        if(!se::DecodeUtf16(units,decoded)||!se::EncodeUtf8(decoded,macro)){Log(LogLevel::Error,text.GetText("Shader.SourceInvalid"));return 1;}
        const auto equals=macro.find('=');description.macros.push_back({macro.substr(0,equals),equals==std::string::npos?"1":macro.substr(equals+1)});
    }
    D3D12ShaderSource source;if(!ReadD3D12ShaderSource(description,source,text))return 1;
    const auto program=CompileD3D12ShaderProgram(std::move(source),text);
    if(!program||!(userPbr?WriteD3D12UserPbrPackage(*program,argv[3],text):WriteD3D12ShaderProgram(*program,argv[3],text)))return 1;
    Log(LogLevel::Info,text.GetText("Shader.BuildComplete")+": "+program->source.key);return 0;
}
