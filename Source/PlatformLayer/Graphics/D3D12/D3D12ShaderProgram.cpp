#include "D3D12ShaderProgram.h"
#include "PlatformLayer/Graphics/MaterialLayout.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include "CoreLayer/Log.h"
#include "CoreLayer/Utf.h"
#include <d3d12shader.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <unordered_map>

using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace rhi=se::rhi;
namespace
{
using Json=nlohmann::json;
constexpr size_t maxSourceBytes=4*1024*1024,maxProgramBytes=64*1024*1024;
struct ShaderField{const char* name;UINT offset,size;bool matrix;};
constexpr ShaderField pbrFields[]{
    {"clipFromLocal",offsetof(rhi::PbrDrawConstants,clipFromLocal),64,true},
    {"worldFromLocal",offsetof(rhi::PbrDrawConstants,worldFromLocal),64,true},
    {"normalFromLocal",offsetof(rhi::PbrDrawConstants,normalFromLocal),64,true},
    {"baseColor",offsetof(rhi::PbrDrawConstants,baseColor),16,false},
    {"cameraPosition",offsetof(rhi::PbrDrawConstants,cameraPosition),16,false},
    {"directionToLight",offsetof(rhi::PbrDrawConstants,directionToLight),16,false},
    {"radiance",offsetof(rhi::PbrDrawConstants,radiance),16,false},
    {"parameters",offsetof(rhi::PbrDrawConstants,parameters),16,false},
    {"options",offsetof(rhi::PbrDrawConstants,options),16,false}};
Json UserPbrInterface()
{
    Json fields=Json::array();for(const auto& field:pbrFields)fields.push_back({{"name",field.name},{"offset",field.offset},{"size",field.size},
        {"type",field.matrix?"column_major float4x4":"float4"}});
    return {{"type","SelfEngine.UserPbrInterface"},{"version",1},{"backend","D3D12"},{"vertex_profile","vs_5_1"},{"pixel_profile","ps_5_1"},
        {"layout",rhi::PbrMaterialLayoutVersion},{"vertex_stride",sizeof(rhi::Vertex)},
        {"vertex",Json::array({{{"semantic","POSITION0"},{"type","float3"},{"offset",offsetof(rhi::Vertex,position)}},
                              {{"semantic","NORMAL0"},{"type","float3"},{"offset",offsetof(rhi::Vertex,normal)}},
                              {{"semantic","TEXCOORD0"},{"type","float2"},{"offset",offsetof(rhi::Vertex,uv)}},
                              {{"semantic","TANGENT0"},{"type","float4"},{"offset",offsetof(rhi::Vertex,tangent)}}})},
        {"constants",{{"name","PbrConstants"},{"register","b0"},{"space",0},{"stages","VS,PS"},{"bytes",sizeof(rhi::PbrDrawConstants)},
            {"upload_stride",rhi::PbrConstantStride},{"fields",fields}}},
        {"textures",Json::array({"PS:t0:space0:Texture2D<float4>","PS:t1:space0:Texture2D<float4>","PS:t2:space0:Texture2D<float4>"})},
        {"sampler","PS:s0:space0:point-wrap"},{"target","SV_Target0:float4"}};
}
std::string Utf8(const fs::path& path){const auto value=path.generic_u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
bool Read(const fs::path& path,std::vector<std::uint8_t>& output,size_t maximum)
{
    std::ifstream input(path,std::ios::binary|std::ios::ate);if(!input)return false;
    const auto size=input.tellg();if(size<0||static_cast<UINT64>(size)>maximum)return false;
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));input.seekg(0);
    if(!bytes.empty()&&!input.read(reinterpret_cast<char*>(bytes.data()),size))return false;output=std::move(bytes);return true;
}
bool Within(const fs::path& root,const fs::path& path)
{
    auto item=path.begin();for(const auto& component:root)
    {
        if(item==path.end())return false;const auto a=component.wstring(),b=item->wstring();
        if(CompareStringOrdinal(a.c_str(),static_cast<int>(a.size()),b.c_str(),static_cast<int>(b.size()),TRUE)!=CSTR_EQUAL)return false;++item;
    }
    return true;
}
void Diagnostic(const Localization& text,const char* key,const fs::path& path,ID3DBlob* details=nullptr,const std::string& detail={})
{
    Log(LogLevel::Error,text.GetText(key)+": "+Utf8(path)+(detail.empty()?"":" ["+detail+"]"));
    if(details)Log(LogLevel::Error,std::string(static_cast<const char*>(details->GetBufferPointer()),details->GetBufferSize()));
}
bool Identifier(const std::string& value)
{
    const auto letter=[](char c){return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||c=='_';};
    return !value.empty()&&letter(value[0])&&std::all_of(value.begin()+1,value.end(),[&](char c){return letter(c)||(c>='0'&&c<='9');});
}
class Includes final : public ID3DInclude
{
public:
    explicit Includes(fs::path root):root_(std::move(root)){}
    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE type,LPCSTR name,LPCVOID parent,LPCVOID* data,UINT* bytes) override
    {
        try{return OpenFile(type,name,parent,data,bytes);}
        catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
        catch(const std::exception&){return E_FAIL;}
    }
    HRESULT OpenFile(D3D_INCLUDE_TYPE type,LPCSTR name,LPCVOID parent,LPCVOID* data,UINT* bytes)
    {
        if(!name||!data||!bytes||++opens_>256)return E_INVALIDARG;
        const auto relative=fs::path(std::u8string_view(reinterpret_cast<const char8_t*>(name),std::strlen(name)));
        if(relative.is_absolute()||relative.has_root_name())return E_ACCESSDENIED;
        auto directory=root_;const auto found=opened_.find(parent);
        if(type==D3D_INCLUDE_LOCAL&&found!=opened_.end())directory=found->second->path.parent_path();
        std::error_code error;const auto path=fs::weakly_canonical(directory/relative,error);
        if(error||!Within(root_,path))return E_ACCESSDENIED;
        auto opened=std::make_unique<File>();opened->path=path;
        if(!Read(path,opened->bytes,maxSourceBytes)||total_+opened->bytes.size()>maxProgramBytes)return E_FAIL;
        const auto size=opened->bytes.size();total_+=size;const auto hash=se::Sha256(std::span<const std::uint8_t>(opened->bytes));if(hash.empty())return E_FAIL;
        dependencies.insert_or_assign(Utf8(path.lexically_relative(root_)),hash);
        if(opened->bytes.empty())opened->bytes.push_back(0);
        *data=opened->bytes.data();*bytes=static_cast<UINT>(size);opened_.emplace(*data,std::move(opened));return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override{return opened_.erase(data)?S_OK:E_INVALIDARG;}
    std::map<std::string,std::string> dependencies;
private:
    struct File{fs::path path;std::vector<std::uint8_t> bytes;};
    fs::path root_;std::unordered_map<LPCVOID,std::unique_ptr<File>> opened_;size_t total_=0;UINT opens_=0;
};
Json Metadata(const D3D12ShaderSource& source)
{
    Json result{{"schema",1},{"kind",static_cast<int>(source.kind)},{"layout",source.layoutVersion},
        {"compiler","D3DCompiler_47"},{"flags",source.flags},{"vertex_entry","VSMain"},{"vertex_profile","vs_5_1"},
        {"pixel_entry","PSMain"},{"pixel_profile","ps_5_1"},{"macros",Json::array()},{"dependencies",Json::array()}};
    for(const auto& macro:source.macros)result["macros"].push_back({{"name",macro.name},{"value",macro.value}});
    for(const auto& dependency:source.dependencies)result["dependencies"].push_back({{"path",dependency.path},{"sha256",dependency.hash}});
    return result;
}
bool Reflect(ID3DBlob& code,ComPtr<ID3D12ShaderReflection>& output)
{return SUCCEEDED(D3DReflect(code.GetBufferPointer(),code.GetBufferSize(),IID_PPV_ARGS(output.GetAddressOf())));}
bool Fields(ID3D12ShaderReflection& shader,bool mesh,bool pbr,std::string& reason,bool depth=false)
{
    const std::string name=depth?"ShadowConstants":pbr?"PbrConstants":mesh?"DrawConstants":"CameraConstants";
    auto* constants=shader.GetConstantBufferByName(name.c_str());D3D12_SHADER_BUFFER_DESC buffer{};
    if(!constants||FAILED(constants->GetDesc(&buffer))){reason=name+": missing";return false;}
    const auto expectedSize=pbr?sizeof(rhi::PbrDrawConstants):mesh?sizeof(rhi::MeshDrawConstants):sizeof(se::Mat4);
    if(buffer.Size!=expectedSize||buffer.Type!=D3D_CT_CBUFFER)
    {reason=name+".size="+std::to_string(buffer.Size)+", expected="+std::to_string(expectedSize);return false;}
    const ShaderField legacyFields[]{
        {mesh?"clipFromLocal":"clipFromWorld",0,64,true},{"normalFromLocal",64,64,true},{"tint",128,16,false},{"options",144,16,false}};
    const auto fields=pbr?std::span<const ShaderField>(pbrFields):std::span<const ShaderField>(legacyFields,mesh?4:1);
    if(buffer.Variables!=fields.size()){reason=name+".variables="+std::to_string(buffer.Variables);return false;}
    for(const auto& field:fields)
    {
        auto* variable=constants->GetVariableByName(field.name);auto* reflectedType=variable?variable->GetType():nullptr;
        D3D12_SHADER_VARIABLE_DESC value{};D3D12_SHADER_TYPE_DESC type{};
        if(!variable||!reflectedType||FAILED(variable->GetDesc(&value))||FAILED(reflectedType->GetDesc(&type))||value.StartOffset!=field.offset||value.Size!=field.size
            ||type.Type!=D3D_SVT_FLOAT||type.Elements||type.Members||type.Rows!=(field.matrix?4u:1u)||type.Columns!=4
            ||type.Class!=(field.matrix?D3D_SVC_MATRIX_COLUMNS:D3D_SVC_VECTOR))
        {
            reason=name+"."+field.name+": offset="+std::to_string(value.StartOffset)+", size="+std::to_string(value.Size)
                +", class="+std::to_string(type.Class)+", expected="+(field.matrix?"column_major float4x4":"float4")
                +"@"+std::to_string(field.offset)+"/"+std::to_string(field.size);return false;
        }
    }
    return true;
}
bool ExtraFields(ID3D12ShaderReflection& shader,bool output,std::string& reason,bool sky=false,bool shadow=false,bool environment=false)
{
    const char* name=sky?(environment?"EnvironmentSkyConstants":"SkyConstants"):output?"OutputConstants":"ForwardConstants";
    auto* constants=shader.GetConstantBufferByName(name);D3D12_SHADER_BUFFER_DESC buffer{};
    struct Field{const char* name;UINT offset,elements;bool matrix=false;};
    const Field frameFields[]{{"frameDirection",0,0},{"frameRadiance",16,0},{"frameSkyExposure",32,0},{"framePointCount",48,0},{"pointPositionRadius",64,8},{"pointRadiance",192,8},
        {"shadowClipFromWorld",320,0,true},{"shadowParameters",384,0},{"environmentParameters",400,0}};
    const Field outputFields[]{{sky?"skyColor":"exposureAndUnused",0,0}};
    const Field skyFields[]{{"skyFromClip",0,0,true},{"environmentParameters",64,0}};
    const Field plainSkyFields[]{{"skyFromClip",0,0,true},{"zenithColor",64,0},{"horizonColor",80,0},{"groundColor",96,0},
        {"sunDirectionIntensity",112,0},{"sunColor",128,0},{"parameters",144,0}};
    const auto fields=sky&&environment?std::span<const Field>(skyFields):sky?std::span<const Field>(plainSkyFields):output?std::span<const Field>(outputFields):std::span<const Field>(frameFields,environment?9:shadow?8:6);
    const auto size=sky&&environment?sizeof(rhi::EnvironmentSkyConstants):sky?sizeof(rhi::SkyConstants):output?16:environment?sizeof(rhi::EnvironmentFrameConstants):shadow?sizeof(rhi::ShadowFrameConstants):sizeof(rhi::ForwardFrameConstants);
    if(!constants||FAILED(constants->GetDesc(&buffer))||buffer.Size!=size||buffer.Variables!=fields.size()||buffer.Type!=D3D_CT_CBUFFER)
    {reason=std::string(name)+": size="+(constants?std::to_string(buffer.Size):"missing")+", variables="+(constants?std::to_string(buffer.Variables):"missing")
        +", expected="+std::to_string(size)+"/"+std::to_string(fields.size());return false;}
    for(const auto& field:fields)
    {
        auto* variable=constants->GetVariableByName(field.name);D3D12_SHADER_VARIABLE_DESC value{};D3D12_SHADER_TYPE_DESC type{};
        if(!variable||FAILED(variable->GetDesc(&value))||FAILED(variable->GetType()->GetDesc(&type))||value.StartOffset!=field.offset||value.Size!=(field.matrix?64:16*std::max(1u,field.elements))
            ||type.Type!=D3D_SVT_FLOAT||type.Class!=(field.matrix?D3D_SVC_MATRIX_COLUMNS:D3D_SVC_VECTOR)||type.Rows!=(field.matrix?4u:1u)||type.Columns!=4||type.Elements!=field.elements||type.Members)
        {reason=std::string(name)+"."+field.name+": layout";return false;}
    }
    return true;
}
bool PostBindings(ID3D12ShaderReflection& shader,rhi::PipelineKind kind,bool vertex,UINT layoutVersion,std::string& reason)
{
    const bool overlay=kind==rhi::PipelineKind::UiOverlay,sampled=kind==rhi::PipelineKind::ToneMap||kind==rhi::PipelineKind::Fxaa;
    // G15：ToneMap 27 布局在旧 t0/t1 之后追加 1×1 曝光与 33³ LUT；其余后处理技术保持 6 布局。
    const UINT textureCount=overlay?0u:kind==rhi::PipelineKind::ToneMap?(layoutVersion==rhi::ToneMapLayoutVersion?4u:2u):1u;
    D3D12_SHADER_DESC desc{};if(FAILED(shader.GetDesc(&desc)))return false;
    const UINT expected=vertex?(overlay?1u:0u):1+textureCount+(sampled?1u:0u);
    if(desc.BoundResources!=expected){reason="post bindings="+std::to_string(desc.BoundResources)+", expected="+std::to_string(expected);return false;}
    bool constants=false,sampler=false;UINT textures=0;
    for(UINT index=0;index<desc.BoundResources;++index)
    {
        D3D12_SHADER_INPUT_BIND_DESC binding{};if(FAILED(shader.GetResourceBindingDesc(index,&binding))||binding.Space||binding.BindCount!=1)return false;
        if(binding.Type==D3D_SIT_CBUFFER&&binding.BindPoint==0&&!constants)
        {
            const auto name=overlay?"OverlayConstants":"PostConstants";auto* buffer=shader.GetConstantBufferByName(name);D3D12_SHADER_BUFFER_DESC cb{};
            if(!buffer||FAILED(buffer->GetDesc(&cb))||cb.Type!=D3D_CT_CBUFFER||cb.Size!=48||cb.Variables!=3){reason=std::string(name)+": size/variables";return false;}
            const char* postNames[]{"postParameters","postResolution","postOptions"};const char* overlayNames[]{"uiRectangle","uiColor","uiViewport"};
            for(UINT field=0;field<3;++field)
            {
                auto* variable=buffer->GetVariableByName((overlay?overlayNames:postNames)[field]);D3D12_SHADER_VARIABLE_DESC value{};D3D12_SHADER_TYPE_DESC type{};
                if(!variable||FAILED(variable->GetDesc(&value))||FAILED(variable->GetType()->GetDesc(&type))||value.StartOffset!=field*16||value.Size!=16
                    ||type.Type!=D3D_SVT_FLOAT||type.Class!=D3D_SVC_VECTOR||type.Rows!=1||type.Columns!=4||type.Elements||type.Members){reason=std::string(name)+": vector layout";return false;}
            }
            constants=true;
        }
        else if(!vertex&&binding.Type==D3D_SIT_TEXTURE&&binding.BindPoint<textureCount&&!(textures&(1u<<binding.BindPoint))
            &&binding.ReturnType==D3D_RETURN_TYPE_FLOAT
            &&(kind==rhi::PipelineKind::ToneMap&&binding.BindPoint==3
                ?binding.Dimension==D3D_SRV_DIMENSION_TEXTURE3D:binding.Dimension==D3D_SRV_DIMENSION_TEXTURE2D))textures|=1u<<binding.BindPoint;
        else if(!vertex&&sampled&&binding.Type==D3D_SIT_SAMPLER&&binding.BindPoint==0&&!sampler&&!(binding.uFlags&D3D_SIF_COMPARISON_SAMPLER))sampler=true;
        else {reason="unexpected post binding";return false;}
    }
    return constants==(!vertex||overlay)&&sampler==(!vertex&&sampled)&&textures==(!vertex?((1u<<textureCount)-1):0u);
}
bool OutputLutBindings(ID3D12ShaderReflection& shader,bool shadow,std::string& reason)
{
    // G15：输出 LUT 变体固定 b0 OutputConstants + t0 场景 + t1 阴影预览（仅 ShadowOutput）
    // + t2 shadow == false 时只有 t0/t2，因此分别要求 3/4 个绑定资源。
    D3D12_SHADER_DESC desc{};if(FAILED(shader.GetDesc(&desc)))return false;
    const UINT expected=shadow?4u:3u;
    if(desc.BoundResources!=expected)
    {reason="output lut bindings="+std::to_string(desc.BoundResources)+", expected="+std::to_string(expected);return false;}
    bool constants=false;UINT textures=0;
    for(UINT index=0;index<desc.BoundResources;++index)
    {
        D3D12_SHADER_INPUT_BIND_DESC binding{};if(FAILED(shader.GetResourceBindingDesc(index,&binding))||binding.Space||binding.BindCount!=1)
        {reason="output lut binding: space/count";return false;}
        if(binding.Type==D3D_SIT_CBUFFER&&binding.BindPoint==0&&!constants)
        {
            if(!ExtraFields(shader,true,reason))return false;
            constants=true;
        }
        else if(binding.Type==D3D_SIT_TEXTURE&&binding.ReturnType==D3D_RETURN_TYPE_FLOAT
            &&binding.Dimension==D3D_SRV_DIMENSION_TEXTURE2D
            &&(binding.BindPoint==0||binding.BindPoint==2||(shadow&&binding.BindPoint==1))
            &&!(textures&(1u<<binding.BindPoint)))textures|=1u<<binding.BindPoint;
        else
        {
            reason=std::string(binding.Name?binding.Name:"binding")+": type="+std::to_string(binding.Type)
                +", register="+std::to_string(binding.BindPoint);
            return false;
        }
    }
    const UINT expectedTextures=shadow?0b111u:0b101u;
    return constants&&textures==expectedTextures;
}
bool Bindings(ID3D12ShaderReflection& shader,rhi::PipelineKind kind,bool vertex,UINT layoutVersion,std::string& reason)
{
    if(kind==rhi::PipelineKind::Gui)
    {
        D3D12_SHADER_DESC desc{};if(FAILED(shader.GetDesc(&desc))||desc.BoundResources!=(vertex?1u:2u))return false;
        bool cb=false,texture=false,sampler=false;
        for(UINT i=0;i<desc.BoundResources;++i)
        {
            D3D12_SHADER_INPUT_BIND_DESC b{};if(FAILED(shader.GetResourceBindingDesc(i,&b))||b.Space||b.BindCount!=1||b.BindPoint)return false;
            if(vertex&&b.Type==D3D_SIT_CBUFFER&&!cb)
            {
                auto* buffer=shader.GetConstantBufferByName("GuiConstants");D3D12_SHADER_BUFFER_DESC value{};
                if(!buffer||FAILED(buffer->GetDesc(&value))||value.Size!=16||value.Variables!=1)return false;
                auto* field=buffer->GetVariableByName("guiScaleTranslate");D3D12_SHADER_VARIABLE_DESC v{};D3D12_SHADER_TYPE_DESC type{};
                if(!field||FAILED(field->GetDesc(&v))||FAILED(field->GetType()->GetDesc(&type))||v.StartOffset||v.Size!=16||type.Type!=D3D_SVT_FLOAT||type.Class!=D3D_SVC_VECTOR||type.Columns!=4||type.Rows!=1||type.Elements||type.Members)return false;
                cb=true;
            }
            else if(!vertex&&b.Type==D3D_SIT_TEXTURE&&!texture&&b.Dimension==D3D_SRV_DIMENSION_TEXTURE2D&&b.ReturnType==D3D_RETURN_TYPE_FLOAT)texture=true;
            else if(!vertex&&b.Type==D3D_SIT_SAMPLER&&!sampler&&!(b.uFlags&D3D_SIF_COMPARISON_SAMPLER))sampler=true;
            else return false;
        }
        return cb==vertex&&texture==!vertex&&sampler==!vertex;
    }
    if(rhi::IsPostProcess(kind))return PostBindings(shader,kind,vertex,layoutVersion,reason);
    if(!vertex&&(kind==rhi::PipelineKind::FinalOutput||kind==rhi::PipelineKind::ShadowOutput)
        &&layoutVersion==rhi::OutputLutLayoutVersion)
        return OutputLutBindings(shader,kind==rhi::PipelineKind::ShadowOutput,reason);
    const bool pbr=rhi::UsesLitLayout(kind),mesh=pbr||kind==rhi::PipelineKind::TexturedMesh,forward=rhi::IsForward(kind);
    const bool output=rhi::IsOutput(kind),sky=rhi::IsSky(kind),depth=kind==rhi::PipelineKind::ShadowDepth,shadow=rhi::IsShadowed(kind);
    const bool environment=kind==rhi::PipelineKind::EnvironmentMesh,environmentSky=kind==rhi::PipelineKind::EnvironmentSky;
    const bool instanced=rhi::IsInstanced(kind);
    const bool forwardInstanced=kind==rhi::PipelineKind::PbrForwardInstancedMesh;
    const UINT textureCount=environment?7u:shadow?4u:pbr?3u:kind==rhi::PipelineKind::ShadowOutput?2u:1u;
    D3D12_SHADER_DESC desc{};if(FAILED(shader.GetDesc(&desc)))return false;
    const UINT expected=forwardInstanced?(vertex?1u:7u):instanced?(vertex?1u:6u):environmentSky?(vertex?1u:3u):sky?(vertex?0u:1u):output?(vertex?0u:textureCount+1):mesh?(vertex?1u:environment?12u:shadow?8u:forward?6u:pbr?5u:3u):(vertex?1u:0u);
    if(desc.BoundResources!=expected){reason="bindings="+std::to_string(desc.BoundResources)+", expected="+std::to_string(expected);return false;}
    bool constants=false,frame=false,instances=false;UINT samplers=0,textures=0;
    for(UINT index=0;index<desc.BoundResources;++index)
    {
        D3D12_SHADER_INPUT_BIND_DESC binding{};if(FAILED(shader.GetResourceBindingDesc(index,&binding)))return false;
        if(binding.Space||binding.BindCount!=1)
        {reason=std::string(binding.Name?binding.Name:"binding")+": space/count="+std::to_string(binding.Space)+"/"+std::to_string(binding.BindCount);return false;}
        if(instanced&&binding.Type==D3D_SIT_BYTEADDRESS&&binding.BindPoint==7&&!instances&&binding.Dimension==D3D_SRV_DIMENSION_BUFFER)instances=true;
        else if(binding.Type==D3D_SIT_CBUFFER&&!binding.BindPoint&&!constants&&!(instanced&&vertex)){constants=true;if(!(output||sky?ExtraFields(shader,true,reason,sky,false,environmentSky):Fields(shader,mesh,pbr,reason,depth)))return false;}
        else if(forward&&!vertex&&binding.Type==D3D_SIT_CBUFFER&&binding.BindPoint==1&&!frame){frame=true;if(!ExtraFields(shader,false,reason,false,shadow,environment))return false;}
        else if((mesh||output||environmentSky)&&!vertex&&binding.Type==D3D_SIT_TEXTURE&&binding.BindPoint<textureCount
            &&!(textures&(1u<<binding.BindPoint))&&binding.Dimension==((environment&&(binding.BindPoint==4||binding.BindPoint==5))||environmentSky?D3D_SRV_DIMENSION_TEXTURECUBE:D3D_SRV_DIMENSION_TEXTURE2D)&&binding.ReturnType==D3D_RETURN_TYPE_FLOAT)textures|=1u<<binding.BindPoint;
        else if((mesh||environmentSky)&&!vertex&&binding.Type==D3D_SIT_SAMPLER
            &&(environmentSky?binding.BindPoint==2:binding.BindPoint<(environment?3u:shadow?2u:1u))&&!(samplers&(1u<<binding.BindPoint)))
        {
            if(((binding.uFlags&D3D_SIF_COMPARISON_SAMPLER)!=0)!=(shadow&&binding.BindPoint==1)){reason="sampler comparison type";return false;}
            samplers|=1u<<binding.BindPoint;
        }
        else {reason=std::string(binding.Name?binding.Name:"binding")+": type="+std::to_string(binding.Type)+", register="+std::to_string(binding.BindPoint);return false;}
    }
    return constants==(instanced?!vertex:environmentSky?true:(output||sky)?!vertex:mesh||vertex)&&textures==((mesh||output||environmentSky)&&!vertex?((1u<<textureCount)-1):0u)
        &&samplers==(!vertex?(environmentSky?4u:environment?7u:mesh?(shadow?3u:1u):0u):0u)&&frame==(forward&&!vertex)&&instances==instanced;
}

bool Layout(const D3D12ShaderProgram& program,std::string* detail=nullptr)
{
    std::string reason;
    ComPtr<ID3D12ShaderReflection> vertex,pixel;
    if(!Reflect(*program.vertex.Get(),vertex)||!Reflect(*program.pixel.Get(),pixel))return false;
    const auto kind=program.source.kind;const bool pbr=rhi::UsesLitLayout(kind),sky=rhi::IsSky(kind),output=rhi::IsOutput(kind)||rhi::IsPostProcess(kind),depth=kind==rhi::PipelineKind::ShadowDepth;
    const bool mesh=pbr||kind==rhi::PipelineKind::TexturedMesh;
    if(!Bindings(*vertex.Get(),kind,true,program.source.layoutVersion,reason)){if(detail)*detail="VS "+reason;return false;}
    if(!Bindings(*pixel.Get(),kind,false,program.source.layoutVersion,reason)){if(detail)*detail="PS "+reason;return false;}
    D3D12_SHADER_DESC vs{},ps{};if(FAILED(vertex->GetDesc(&vs))||FAILED(pixel->GetDesc(&ps)))return false;
    if(D3D12_SHVER_GET_TYPE(vs.Version)!=D3D12_SHVER_VERTEX_SHADER||D3D12_SHVER_GET_TYPE(ps.Version)!=D3D12_SHVER_PIXEL_SHADER
        ||D3D12_SHVER_GET_MAJOR(vs.Version)!=5||D3D12_SHVER_GET_MINOR(vs.Version)!=1
        ||D3D12_SHVER_GET_MAJOR(ps.Version)!=5||D3D12_SHVER_GET_MINOR(ps.Version)!=1)
    {if(detail)*detail="profiles: vs_5_1 / ps_5_1";return false;}
    if(vs.InputParameters!=(kind==rhi::PipelineKind::Gui?3u:rhi::IsInstanced(kind)?5u:(output||sky||depth)?1u:pbr?4u:mesh?3u:2u)||ps.OutputParameters!=(depth?0u:1u)){if(detail)*detail="vertex inputs / pixel targets";return false;}
    const char* names[]{"POSITION",mesh?"NORMAL":"COLOR","TEXCOORD","TANGENT"};
    const bool compressedVertex=program.source.compressedVertex;
    const UINT masks[]{compressedVertex?15u:7u,mesh?(compressedVertex?3u:7u):15u,3,15};
    for(UINT index=0;index<vs.InputParameters;++index)
    {
        D3D12_SIGNATURE_PARAMETER_DESC input{};if(FAILED(vertex->GetInputParameterDesc(index,&input)))return false;
        if(kind==rhi::PipelineKind::Gui)
        {const char* guiNames[]{"POSITION","TEXCOORD","COLOR"};const UINT guiMasks[]{3,3,15};if(_stricmp(input.SemanticName,guiNames[index])||input.SemanticIndex||input.ComponentType!=D3D_REGISTER_COMPONENT_FLOAT32||input.Mask!=guiMasks[index])return false;continue;}
        if(rhi::IsInstanced(kind)&&index==4){if(input.SystemValueType!=D3D_NAME_INSTANCE_ID||input.ComponentType!=D3D_REGISTER_COMPONENT_UINT32||input.Mask!=1)return false;continue;}
        if(output||sky){if(input.SystemValueType!=D3D_NAME_VERTEX_ID||input.ComponentType!=D3D_REGISTER_COMPONENT_UINT32||input.Mask!=1)return false;continue;}
        if(_stricmp(input.SemanticName,names[index])
            ||input.SemanticIndex||input.ComponentType!=D3D_REGISTER_COMPONENT_FLOAT32||input.Mask!=masks[index])return false;
    }
    for(UINT inputIndex=0;inputIndex<ps.InputParameters;++inputIndex)
    {
        D3D12_SIGNATURE_PARAMETER_DESC input{};if(FAILED(pixel->GetInputParameterDesc(inputIndex,&input)))return false;
        if(pbr&&input.SystemValueType==D3D_NAME_IS_FRONT_FACE)continue;
        bool matched=false;
        for(UINT outputIndex=0;outputIndex<vs.OutputParameters;++outputIndex)
        {
            D3D12_SIGNATURE_PARAMETER_DESC varying{};if(FAILED(vertex->GetOutputParameterDesc(outputIndex,&varying)))return false;
            matched|=!_stricmp(input.SemanticName,varying.SemanticName)&&input.SemanticIndex==varying.SemanticIndex
                &&input.ComponentType==varying.ComponentType&&(input.Mask&varying.Mask)==input.Mask;
        }
        if(!matched)return false;
    }
    if(depth)return true;
    D3D12_SIGNATURE_PARAMETER_DESC target{};return SUCCEEDED(pixel->GetOutputParameterDesc(0,&target))&&target.SystemValueType==D3D_NAME_TARGET
        &&target.SemanticIndex==0&&target.ComponentType==D3D_REGISTER_COMPONENT_FLOAT32&&target.Mask==15;
}
std::string BlobHash(ID3DBlob& blob)
{return se::Sha256(std::span(static_cast<const std::uint8_t*>(blob.GetBufferPointer()),blob.GetBufferSize()));}
void U32(std::vector<std::uint8_t>& bytes,UINT value){for(UINT shift=0;shift<32;shift+=8)bytes.push_back(static_cast<std::uint8_t>(value>>shift));}
UINT U32(const std::uint8_t* bytes){return UINT(bytes[0])|(UINT(bytes[1])<<8)|(UINT(bytes[2])<<16)|(UINT(bytes[3])<<24);}
bool Atomic(const fs::path& path,std::span<const std::uint8_t> bytes)
{
    std::error_code error;if(!path.parent_path().empty())fs::create_directories(path.parent_path(),error);if(error||bytes.size()>MAXDWORD)return false;
    static std::atomic<UINT64> next{1};auto temporary=path;temporary+=L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(next.fetch_add(1));
    HANDLE file=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    DWORD written=0;const bool wrote=WriteFile(file,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(file);
    const bool closed=CloseHandle(file)!=FALSE;
    if(!wrote||!closed||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
    {DeleteFileW(temporary.c_str());return false;}return true;
}
struct ArtifactParts {Json metadata;ComPtr<ID3DBlob> vertex,pixel;};
bool ReadArtifactParts(const fs::path& path,ArtifactParts& output)
{
    std::vector<std::uint8_t> bytes;if(!Read(path,bytes,maxProgramBytes)||bytes.size()<20||std::memcmp(bytes.data(),"SESHDR1",8))return false;
    const auto jsonBytes=U32(bytes.data()+8),vertexBytes=U32(bytes.data()+12),pixelBytes=U32(bytes.data()+16);
    if(!jsonBytes||!vertexBytes||!pixelBytes||jsonBytes>maxSourceBytes||vertexBytes>maxSourceBytes||pixelBytes>maxSourceBytes
        ||UINT64{20}+jsonBytes+vertexBytes+pixelBytes!=bytes.size())return false;
    try
    {
        ArtifactParts next;next.metadata=Json::parse(bytes.begin()+20,bytes.begin()+20+jsonBytes,
            [](int depth,Json::parse_event_t,Json&){if(depth>32)throw std::invalid_argument("shader metadata depth");return true;});
        if(!next.metadata.is_object()||FAILED(D3DCreateBlob(vertexBytes,next.vertex.GetAddressOf()))||FAILED(D3DCreateBlob(pixelBytes,next.pixel.GetAddressOf())))return false;
        std::memcpy(next.vertex->GetBufferPointer(),bytes.data()+20+jsonBytes,vertexBytes);
        std::memcpy(next.pixel->GetBufferPointer(),bytes.data()+20+jsonBytes+vertexBytes,pixelBytes);
        const auto vertexHash=BlobHash(*next.vertex.Get()),pixelHash=BlobHash(*next.pixel.Get());
        if(vertexHash.empty()||pixelHash.empty()||next.metadata.at("vertex_sha256")!=vertexHash||next.metadata.at("pixel_sha256")!=pixelHash)return false;
        output=std::move(next);return true;
    }
    catch(const std::exception&){return false;}
}
bool EncodeArtifact(const Json& metadata,const D3D12ShaderProgram& program,std::vector<std::uint8_t>& bytes)
{
    if(!program.vertex||!program.pixel)return false;const auto json=metadata.dump(2);
    if(json.size()>maxSourceBytes||program.vertex->GetBufferSize()>maxSourceBytes||program.pixel->GetBufferSize()>maxSourceBytes)return false;
    bytes={'S','E','S','H','D','R','1',0};U32(bytes,static_cast<UINT>(json.size()));
    U32(bytes,static_cast<UINT>(program.vertex->GetBufferSize()));U32(bytes,static_cast<UINT>(program.pixel->GetBufferSize()));
    bytes.insert(bytes.end(),json.begin(),json.end());
    for(const auto code:{program.vertex.Get(),program.pixel.Get()}){const auto* data=static_cast<const std::uint8_t*>(code->GetBufferPointer());bytes.insert(bytes.end(),data,data+code->GetBufferSize());}
    return true;
}
std::shared_ptr<const D3D12ShaderProgram> ReadArtifact(D3D12ShaderSource source,const fs::path& path)
{
    ArtifactParts parts;if(!ReadArtifactParts(path,parts))return {};
    try
    {
        const auto& metadata=parts.metadata;
        if(metadata.at("source_key").get<std::string>()!=source.key||metadata.at("schema")!=1||metadata.at("kind")!=static_cast<int>(source.kind)
            ||metadata.at("flags")!=source.flags||metadata.at("layout")!=source.layoutVersion)return {};
        auto program=std::make_shared<D3D12ShaderProgram>();program->source=std::move(source);
        program->vertex=std::move(parts.vertex);program->pixel=std::move(parts.pixel);
        const auto vertexHash=BlobHash(*program->vertex.Get()),pixelHash=BlobHash(*program->pixel.Get());
        if(vertexHash.empty()||pixelHash.empty()||metadata.at("vertex_sha256")!=vertexHash||metadata.at("pixel_sha256")!=pixelHash||!Layout(*program))return {};
        program->bytecodeKey=se::Sha256(vertexHash+":"+pixelHash);return program;
    }
    catch(const Json::exception&){return {};}
}
}
UINT D3D12ShaderCompileFlags()
{
    UINT flags=D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS;
#if defined(_DEBUG)
    return flags|D3DCOMPILE_DEBUG|D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    return flags|D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
}
bool ReadD3D12ShaderSource(const rhi::PipelineDescription& description,D3D12ShaderSource& output,const Localization& text)
{
    if(description.shaderInput!=rhi::ShaderInput::Source||description.shader.empty()
        ||(description.bindingLayoutVersion!=rhi::BindingLayout(description.kind)
            &&!(rhi::IsOutput(description.kind)&&description.bindingLayoutVersion==rhi::OutputLutLayoutVersion))
        ||!rhi::ValidPipelineKind(description.kind))return false;
    D3D12ShaderSource source;source.path=description.shader;source.kind=description.kind;source.flags=D3D12ShaderCompileFlags();source.layoutVersion=description.bindingLayoutVersion;source.macros=description.macros;source.compressedVertex=description.vertexLayoutVersion==rhi::VertexLayoutVersionCompressed;
    if(source.macros.size()>64){Diagnostic(text,"Shader.SourceInvalid",source.path);return false;}
    std::sort(source.macros.begin(),source.macros.end(),[](const auto& a,const auto& b){return a.name<b.name;});
    for(size_t index=0;index<source.macros.size();++index)
    {
        std::u32string decoded;
        if(!Identifier(source.macros[index].name)||source.macros[index].value.size()>4096||source.macros[index].value.find('\0')!=std::string::npos
            ||!se::DecodeUtf8(source.macros[index].value,decoded)
            ||(index&&source.macros[index].name==source.macros[index-1].name)){Diagnostic(text,"Shader.SourceInvalid",source.path);return false;}
    }
    std::error_code error;const auto path=fs::weakly_canonical(source.path,error);std::vector<std::uint8_t> original;
    if(error||!Read(path,original,maxSourceBytes)){Diagnostic(text,"Shader.SourceInvalid",source.path);return false;}
    Includes includes(path.parent_path());const auto hash=se::Sha256(std::span<const std::uint8_t>(original));if(hash.empty())return false;
    includes.dependencies.emplace(Utf8(path.filename()),hash);
    std::vector<D3D_SHADER_MACRO> macros;for(const auto& macro:source.macros)macros.push_back({macro.name.c_str(),macro.value.c_str()});macros.push_back({nullptr,nullptr});
    ComPtr<ID3DBlob> preprocessed,diagnostics;const auto name=Utf8(path.filename());
    const auto preprocessResult=D3DPreprocess(original.data(),original.size(),name.c_str(),macros.data(),&includes,preprocessed.GetAddressOf(),diagnostics.GetAddressOf());
    bool messages=false;
    if(diagnostics)
    {
        const auto* begin=static_cast<const char*>(diagnostics->GetBufferPointer());
        messages=std::any_of(begin,begin+diagnostics->GetBufferSize(),[](char c){return c!='\0'&&c!='\r'&&c!='\n'&&c!=' '&&c!='\t';});
    }
    // 预处理 API 没有 warnings-as-errors 标志；其诊断也必须使构建失败。
    if(FAILED(preprocessResult)||messages)
    {Diagnostic(text,"D3D12.ShaderCompileFailed",source.path,diagnostics.Get(),"preprocess");return false;}
    source.preprocessed.assign(static_cast<const char*>(preprocessed->GetBufferPointer()),preprocessed->GetBufferSize());
    for(const auto& [relative,digest]:includes.dependencies)source.dependencies.push_back({relative,digest});
    source.key=se::Sha256(Metadata(source).dump());if(source.key.empty())return false;output=std::move(source);return true;
}
std::shared_ptr<const D3D12ShaderProgram> CompileD3D12ShaderProgram(D3D12ShaderSource source,const Localization& text)
{
    auto program=std::make_shared<D3D12ShaderProgram>();program->source=std::move(source);const auto& input=program->source;
    const auto compile=[&](const char* entry,const char* profile,ComPtr<ID3DBlob>& output){
        ComPtr<ID3DBlob> diagnostics;const auto name=Utf8(input.path.filename());
        const auto result=D3DCompile(input.preprocessed.data(),input.preprocessed.size(),name.c_str(),nullptr,nullptr,entry,profile,input.flags,0,output.GetAddressOf(),diagnostics.GetAddressOf());
        if(FAILED(result)){Diagnostic(text,"D3D12.ShaderCompileFailed",input.path,diagnostics.Get(),entry);return false;}return true;
    };
    if(!compile("VSMain","vs_5_1",program->vertex)||!compile("PSMain","ps_5_1",program->pixel))return {};
    std::string reason;if(!Layout(*program,&reason)){Diagnostic(text,"Shader.LayoutMismatch",input.path,nullptr,reason);return {};}
    const auto vertex=BlobHash(*program->vertex.Get()),pixel=BlobHash(*program->pixel.Get());if(vertex.empty()||pixel.empty())return {};
    program->bytecodeKey=se::Sha256(vertex+":"+pixel);return program;
}
std::shared_ptr<const D3D12ShaderProgram> LoadD3D12ShaderProgram(D3D12ShaderSource source,D3D12ShaderLoadStatistics& stats,const Localization& text)
{
    auto artifact=source.path;artifact+=".sebin";std::error_code error;
    if(fs::exists(artifact,error)&&!error)
    {
        if(auto program=ReadArtifact(source,artifact)){++stats.artifactLoads;return program;}
        ++stats.artifactRejections;Log(LogLevel::Warning,text.GetText("Shader.ArtifactRejected")+": "+Utf8(artifact));
    }
    ++stats.compilations;return CompileD3D12ShaderProgram(std::move(source),text);
}
bool WriteD3D12ShaderProgram(const D3D12ShaderProgram& program,const fs::path& artifact,const Localization& text)
{
    auto metadata=Metadata(program.source);metadata["source_key"]=program.source.key;
    metadata["vertex_sha256"]=BlobHash(*program.vertex.Get());metadata["pixel_sha256"]=BlobHash(*program.pixel.Get());
    const auto json=metadata.dump(2);std::vector<std::uint8_t> bytes;if(!EncodeArtifact(metadata,program,bytes))return false;
    auto manifest=artifact;manifest+=".json";
    if(!Atomic(manifest,std::span(reinterpret_cast<const std::uint8_t*>(json.data()),json.size()))||!Atomic(artifact,bytes))
    {Diagnostic(text,"Shader.WriteFailed",artifact);return false;}return true;
}
namespace
{
bool Digest(const std::string& value)
{return value.size()==64&&std::all_of(value.begin(),value.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
bool RelativeDependency(const std::string& value)
{
    std::u32string decoded;
    if(value.empty()||value.size()>4096||value.find_first_of("\\:")!=std::string::npos||value.find('\0')!=std::string::npos||!se::DecodeUtf8(value,decoded))return false;
    const fs::path path(std::u8string_view(reinterpret_cast<const char8_t*>(value.data()),value.size()));
    if(path.is_absolute()||path.has_root_name()||path.filename().empty()||Utf8(path.lexically_normal())!=value)return false;
    for(const auto& part:path)if(part=="."||part=="..")return false;return true;
}
bool PackageSource(const Json& metadata,D3D12ShaderSource& source)
{
    try
    {
        if(metadata.at("package_type")!="SelfEngine.UserPbr"||metadata.at("package_schema")!=1||metadata.at("debug_data_stripped")!=true
            ||metadata.at("interface")!=UserPbrInterface()||metadata.at("interface_sha256")!=D3D12UserPbrInterfaceHash()
            ||!metadata.at("flags").is_number_unsigned()||metadata.at("flags").get<UINT64>()>UINT_MAX)return false;
        source.kind=rhi::PipelineKind::PbrMesh;source.layoutVersion=rhi::PbrMaterialLayoutVersion;source.flags=metadata.at("flags").get<UINT>();
        constexpr UINT strict=D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_WARNINGS_ARE_ERRORS;
        if(source.flags!=(strict|D3DCOMPILE_DEBUG|D3DCOMPILE_SKIP_OPTIMIZATION)&&source.flags!=(strict|D3DCOMPILE_OPTIMIZATION_LEVEL3))return false;
        const auto& macros=metadata.at("macros");const auto& dependencies=metadata.at("dependencies");
        if(!macros.is_array()||macros.size()>64||!dependencies.is_array()||dependencies.empty()||dependencies.size()>257)return false;
        for(const auto& item:macros)
        {
            rhi::ShaderMacro macro{item.at("name").get<std::string>(),item.at("value").get<std::string>()};std::u32string decoded;
            if(!Identifier(macro.name)||macro.value.size()>4096||macro.value.find('\0')!=std::string::npos||!se::DecodeUtf8(macro.value,decoded)
                ||(!source.macros.empty()&&source.macros.back().name>=macro.name))return false;
            source.macros.push_back(std::move(macro));
        }
        for(const auto& item:dependencies)
        {
            D3D12ShaderDependency dependency{item.at("path").get<std::string>(),item.at("sha256").get<std::string>()};
            if(!RelativeDependency(dependency.path)||!Digest(dependency.hash)||(!source.dependencies.empty()&&source.dependencies.back().path>=dependency.path))return false;
            source.dependencies.push_back(std::move(dependency));
        }
        const auto canonical=Metadata(source);if(metadata.size()!=canonical.size()+8)return false;
        for(auto it=canonical.begin();it!=canonical.end();++it)if(metadata.at(it.key())!=it.value())return false;
        source.key=se::Sha256(canonical.dump());return !source.key.empty()&&metadata.at("source_key")==source.key;
    }
    catch(const std::exception&){return false;}
}
bool NoPrivateShaderParts(ID3DBlob& code)
{
    for(const auto part:{D3D_BLOB_DEBUG_INFO,D3D_BLOB_PRIVATE_DATA,D3D_BLOB_ROOT_SIGNATURE})
    {
        ComPtr<ID3DBlob> data;
        if(SUCCEEDED(D3DGetBlobPart(code.GetBufferPointer(),code.GetBufferSize(),part,0,data.GetAddressOf()))&&data&&data->GetBufferSize())return false;
    }
    return true;
}
}
std::string D3D12UserPbrInterfaceDescription(){return UserPbrInterface().dump(2);}
std::string D3D12UserPbrInterfaceHash(){return se::Sha256(UserPbrInterface().dump());}
bool WriteD3D12UserPbrPackage(const D3D12ShaderProgram& program,const fs::path& artifact,const Localization& text)
{
    if(program.source.kind!=rhi::PipelineKind::PbrMesh||program.source.layoutVersion!=rhi::PbrMaterialLayoutVersion
        ||!program.vertex||!program.pixel||!Layout(program)||artifact.empty())
    {Diagnostic(text,"Shader.UserPackageInvalid",artifact);return false;}
    D3D12ShaderProgram packed;packed.source=program.source;packed.source.preprocessed.clear();
    constexpr UINT strip=D3DCOMPILER_STRIP_DEBUG_INFO|D3DCOMPILER_STRIP_PRIVATE_DATA|D3DCOMPILER_STRIP_TEST_BLOBS|D3DCOMPILER_STRIP_ROOT_SIGNATURE;
    if(FAILED(D3DStripShader(program.vertex->GetBufferPointer(),program.vertex->GetBufferSize(),strip,packed.vertex.GetAddressOf()))
        ||FAILED(D3DStripShader(program.pixel->GetBufferPointer(),program.pixel->GetBufferSize(),strip,packed.pixel.GetAddressOf()))
        ||!NoPrivateShaderParts(*packed.vertex.Get())||!NoPrivateShaderParts(*packed.pixel.Get())||!Layout(packed))
    {Diagnostic(text,"Shader.UserPackageInvalid",artifact);return false;}
    auto metadata=Metadata(packed.source);metadata["source_key"]=packed.source.key;
    metadata["vertex_sha256"]=BlobHash(*packed.vertex.Get());metadata["pixel_sha256"]=BlobHash(*packed.pixel.Get());
    metadata["package_type"]="SelfEngine.UserPbr";metadata["package_schema"]=1;metadata["interface"]=UserPbrInterface();
    metadata["interface_sha256"]=D3D12UserPbrInterfaceHash();metadata["debug_data_stripped"]=true;
    D3D12ShaderSource check;std::vector<std::uint8_t> bytes;
    if(!PackageSource(metadata,check)||!EncodeArtifact(metadata,packed,bytes))
    {Diagnostic(text,"Shader.UserPackageInvalid",artifact);return false;}
    if(!Atomic(artifact,bytes)){Diagnostic(text,"Shader.WriteFailed",artifact);return false;}return true;
}
std::shared_ptr<const D3D12ShaderProgram> LoadD3D12UserPbrPackage(const rhi::PipelineDescription& description,
    D3D12ShaderLoadStatistics& stats,const Localization& text)
{
    const auto reject=[&]()->std::shared_ptr<const D3D12ShaderProgram>{++stats.artifactRejections;Diagnostic(text,"Shader.UserPackageInvalid",description.shader);return {};};
    if(description.shaderInput!=rhi::ShaderInput::Artifact||description.kind!=rhi::PipelineKind::PbrMesh
        ||description.bindingLayoutVersion!=rhi::PbrMaterialLayoutVersion||description.shader.empty()||!description.macros.empty())return reject();
    ArtifactParts parts;D3D12ShaderSource source;
    if(!ReadArtifactParts(description.shader,parts)||!PackageSource(parts.metadata,source)
        ||!NoPrivateShaderParts(*parts.vertex.Get())||!NoPrivateShaderParts(*parts.pixel.Get()))return reject();
    auto program=std::make_shared<D3D12ShaderProgram>();source.path=description.shader;program->source=std::move(source);
    program->vertex=std::move(parts.vertex);program->pixel=std::move(parts.pixel);std::string reason;
    if(!Layout(*program,&reason)){++stats.artifactRejections;Diagnostic(text,"Shader.LayoutMismatch",description.shader,nullptr,reason);return {};}
    program->bytecodeKey=se::Sha256(BlobHash(*program->vertex.Get())+":"+BlobHash(*program->pixel.Get()));
    if(program->bytecodeKey.empty())return reject();++stats.artifactLoads;return program;
}
