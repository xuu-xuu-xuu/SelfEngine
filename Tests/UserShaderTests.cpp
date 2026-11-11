#include "PlatformLayer/Graphics/D3D12/D3D12Diagnostics.h"
#include "PlatformLayer/Graphics/D3D12/D3D12PipelineCache.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Memory.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "PlatformLayer/Windows/Window.h"
#include "CoreLayer/Hash.h"
#include "CoreLayer/Localization.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <tuple>

namespace rhi=se::rhi;
namespace fs=std::filesystem;
using Json=nlohmann::json;
using Microsoft::WRL::ComPtr;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Ok(rhi::Result value,const char* message){if(!value)throw std::runtime_error(std::string(message)+": "+value.Key());}
void Hr(HRESULT value,const char* message){Check(SUCCEEDED(value),message);}
std::string Read(const fs::path& path)
{std::ifstream input(path,std::ios::binary);Check(bool(input),"read user Shader fixture");return {std::istreambuf_iterator<char>(input),{}};}
void Write(const fs::path& path,const std::string& bytes)
{std::ofstream output(path,std::ios::binary);output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));Check(bool(output),"write user Shader fixture");}
void Replace(std::string& text,const std::string& before,const std::string& after)
{const auto pos=text.find(before);Check(pos!=std::string::npos,"known fixture fragment");text.replace(pos,before.size(),after);}
UINT U32(const std::string& bytes,size_t offset)
{UINT result=0;for(UINT i=0;i<4;++i)result|=UINT(static_cast<unsigned char>(bytes.at(offset+i)))<<(8*i);return result;}
void Put32(std::string& bytes,UINT value){for(UINT i=0;i<4;++i)bytes+=static_cast<char>(value>>(8*i));}
struct Parts{Json metadata;std::string vertex,pixel;};
Parts Unpack(const std::string& bytes)
{
    const auto json=U32(bytes,8),vertex=U32(bytes,12),pixel=U32(bytes,16);
    Check(20ull+json+vertex+pixel==bytes.size(),"valid package fixture lengths");
    return {Json::parse(bytes.substr(20,json)),bytes.substr(20+json,vertex),bytes.substr(20+json+vertex,pixel)};
}
std::string Pack(Parts parts,bool rehash=false,const std::string& rawJson={})
{
    if(rehash){parts.metadata["vertex_sha256"]=se::Sha256(parts.vertex);parts.metadata["pixel_sha256"]=se::Sha256(parts.pixel);}
    const auto metadata=rawJson.empty()?parts.metadata.dump():rawJson;std::string bytes("SESHDR1\0",8);
    Put32(bytes,static_cast<UINT>(metadata.size()));Put32(bytes,static_cast<UINT>(parts.vertex.size()));Put32(bytes,static_cast<UINT>(parts.pixel.size()));
    return bytes+metadata+parts.vertex+parts.pixel;
}
DWORD Build(const fs::path& shader,const fs::path& artifact,const fs::path& log,bool user=true,const std::wstring& macros={})
{
    const auto executable=GetExecutableDirectory()/"ShaderBuilder.exe";
    auto command=L"\""+executable.wstring()+L"\" "+(user?L"user-pbr":L"pbr")+L" \""+shader.wstring()+L"\" \""+artifact.wstring()+L"\" "+macros;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),nullptr,TRUE};
    HANDLE file=CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ,&attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    Check(file!=INVALID_HANDLE_VALUE,"create child diagnostic log");
    struct FileGuard{HANDLE value;~FileGuard(){CloseHandle(value);}} fileGuard{file};
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESTDHANDLES;startup.hStdOutput=file;startup.hStdError=file;
    PROCESS_INFORMATION process{};
    Check(CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,CREATE_NO_WINDOW,nullptr,GetExecutableDirectory().c_str(),&startup,&process)!=FALSE,"run actual user ShaderBuilder");
    struct Guard{PROCESS_INFORMATION& value;~Guard(){CloseHandle(value.hThread);CloseHandle(value.hProcess);}} guard{process};
    if(WaitForSingleObject(process.hProcess,30000)!=WAIT_OBJECT_0)
    {TerminateProcess(process.hProcess,1);WaitForSingleObject(process.hProcess,30000);Check(false,"own ShaderBuilder child timeout");}
    DWORD exit=0;Check(GetExitCodeProcess(process.hProcess,&exit)!=FALSE,"ShaderBuilder exit");return exit;
}
rhi::PipelineDescription Artifact(const fs::path& path)
{rhi::PipelineDescription result{rhi::PipelineKind::PbrMesh,path,rhi::PbrMaterialLayoutVersion};result.shaderInput=rhi::ShaderInput::Artifact;return result;}
std::string Strip(ID3DBlob& blob)
{
    ComPtr<ID3DBlob> packed;Hr(D3DStripShader(blob.GetBufferPointer(),blob.GetBufferSize(),D3DCOMPILER_STRIP_DEBUG_INFO|D3DCOMPILER_STRIP_PRIVATE_DATA|
        D3DCOMPILER_STRIP_TEST_BLOBS|D3DCOMPILER_STRIP_ROOT_SIGNATURE,packed.GetAddressOf()),"strip test bytecode");
    return {static_cast<const char*>(packed->GetBufferPointer()),packed->GetBufferSize()};
}
Parts CompileUnchecked(const D3D12ShaderSource& source,const Parts& declaration,const char* vertexProfile="vs_5_1")
{
    Parts parts=declaration;ComPtr<ID3DBlob> vertex,pixel,errors;
    Hr(D3DCompile(source.preprocessed.data(),source.preprocessed.size(),"fixture.hlsl",nullptr,nullptr,"VSMain",vertexProfile,
        source.flags,0,vertex.GetAddressOf(),errors.GetAddressOf()),"compile intentionally mismatched VS");errors.Reset();
    Hr(D3DCompile(source.preprocessed.data(),source.preprocessed.size(),"fixture.hlsl",nullptr,nullptr,"PSMain","ps_5_1",
        source.flags,0,pixel.GetAddressOf(),errors.GetAddressOf()),"compile intentionally mismatched PS");
    parts.vertex=Strip(*vertex.Get());parts.pixel=Strip(*pixel.Get());return parts;
}
void Cpu(const fs::path& source,const fs::path& runtime,const Localization& text,Json& evidence)
{
    const auto artifact=runtime/"full.sebin",half=runtime/"half.sebin",shader=source/"Custom.hlsl";
    Check(Build(shader,artifact,source/"build-full.log")==0,"actual compiler builds user package");
    Check(Build(shader,half,source/"build-half.log",true,L"USER_GAIN=0.5")==0,"actual compiler macro variant");
    Check(Build(shader,fs::path(shader.wstring()+L".sebin"),source/"build-internal.log",false)==0,"existing internal package path");
    Check(!fs::exists(fs::path(artifact.wstring()+L".json")),"single user artifact commit point");
    const auto good=Read(artifact),original=Read(shader),bindings=Read(source/"PbrBindings.hlsli");const auto parts=Unpack(good);
    Check(parts.metadata.at("interface")==Json::parse(D3D12UserPbrInterfaceDescription())&&parts.metadata.at("interface_sha256")==D3D12UserPbrInterfaceHash(),"embedded fixed interface and hash");
    for(const auto* code:{&parts.vertex,&parts.pixel})for(const auto part:{D3D_BLOB_DEBUG_INFO,D3D_BLOB_PRIVATE_DATA,D3D_BLOB_ROOT_SIGNATURE})
    {ComPtr<ID3DBlob> data;Check(FAILED(D3DGetBlobPart(code->data(),code->size(),part,0,data.GetAddressOf()))||!data||!data->GetBufferSize(),"no source debug/private/root chunks");}
    Check(good.find("cbuffer PbrConstants")==std::string::npos&&good.find("UserShader source marker") ==std::string::npos&&good.find(source.string())==std::string::npos,"no source body or absolute authoring path");
    D3D12ShaderLoadStatistics stats;const auto description=Artifact(artifact);
    Check(bool(LoadD3D12UserPbrPackage(description,stats,text))&&stats.artifactLoads==1&&!stats.compilations,"artifact load without compiler");
    const auto badPath=runtime/"rejected.sebin";Json rejected=Json::array();
    const auto reject=[&](const std::string& name,const std::string& bytes){Write(badPath,bytes);const auto before=stats.artifactRejections;
        Check(!LoadD3D12UserPbrPackage(Artifact(badPath),stats,text)&&stats.artifactRejections==before+1&&!stats.compilations,"malformed artifact rejected without compile");rejected.push_back(name);};
    const std::vector<std::pair<std::string,std::function<void(Json&)>>> changes{
        {"package-type",[](Json& j){j["package_type"]="Other";}},{"package-version",[](Json& j){j["package_schema"]=2;}},
        {"internal-schema",[](Json& j){j["schema"]=2;}},{"backend",[](Json& j){j["interface"]["backend"]="Other";}},
        {"vertex-profile",[](Json& j){j["vertex_profile"]="vs_5_0";}},{"pixel-entry",[](Json& j){j["pixel_entry"]="Other";}},
        {"compiler",[](Json& j){j["compiler"]="Unknown";}},{"kind",[](Json& j){j["kind"]=0;}},
        {"layout",[](Json& j){j["layout"]=1;}},{"interface-field",[](Json& j){j["interface"]["constants"]["bytes"]=256;}},
        {"interface-hash",[](Json& j){j["interface_sha256"]=std::string(64,'0');}},{"source-hash",[](Json& j){j["source_key"]=std::string(64,'0');}},
        {"bytecode-hash",[](Json& j){j["pixel_sha256"]=std::string(64,'0');}},{"flags",[](Json& j){j["flags"]=UINT64_MAX;}},
        {"flags-type",[](Json& j){j["flags"]="0";}},{"unstripped",[](Json& j){j["debug_data_stripped"]=false;}},
        {"absolute-dependency",[](Json& j){j["dependencies"][0]["path"]="C:/private/source.hlsl";}},
        {"escape-dependency",[](Json& j){j["dependencies"][0]["path"]="../private.hlsl";}},
        {"dependency-digest",[](Json& j){j["dependencies"][0]["sha256"]="bad";}},{"empty-dependencies",[](Json& j){j["dependencies"]=Json::array();}},
        {"macro-duplicate",[](Json& j){j["macros"]=Json::array({{{"name","A"},{"value","1"}},{{"name","A"},{"value","2"}}});}},
        {"macro-unsorted",[](Json& j){j["macros"]=Json::array({{{"name","Z"},{"value","1"}},{{"name","A"},{"value","2"}}});}},
        {"macro-name",[](Json& j){j["macros"]=Json::array({{{"name","A B"},{"value","1"}}});}},
        {"unknown-field",[](Json& j){j["unexpected"]=true;}},{"missing-interface",[](Json& j){j.erase("interface");}}
    };
    for(const auto& [name,change]:changes){auto malformed=parts;change(malformed.metadata);reject(name,Pack(malformed));}
    auto bytes=good;bytes[0]='X';reject("magic",bytes);reject("short-header",good.substr(0,19));reject("truncated-code",good.substr(0,good.size()-1));
    reject("trailing-byte",good+"x");bytes=good;for(size_t i=8;i<12;++i)bytes[i]=static_cast<char>(255);reject("oversize-metadata-header",bytes);
    bytes=good;for(size_t i=12;i<16;++i)bytes[i]=0;reject("zero-vertex-length",bytes);
    reject("deep-json",Pack(parts,false,std::string(40,'[')+"0"+std::string(40,']')));reject("array-json",Pack(parts,false,"[]"));
    auto switched=parts;std::swap(switched.vertex,switched.pixel);reject("swapped-stages-rehashed",Pack(switched,true));
    switched=parts;switched.pixel[0]='X';reject("bad-dxbc-rehashed",Pack(switched,true));
    rhi::PipelineDescription sourceDescription{rhi::PipelineKind::PbrMesh,shader,2};
    for(const auto& [name,before,after]:std::vector<std::tuple<std::string,std::string,std::string>>{
        {"row-major","column_major float4x4 clipFromLocal","row_major float4x4 clipFromLocal"},{"wrong-register","register(t0)","register(t3)"}})
    {
        auto changed=bindings;Replace(changed,before,after);Write(source/"PbrBindings.hlsli",changed);D3D12ShaderSource invalid;
        Check(ReadD3D12ShaderSource(sourceDescription,invalid,text),"preprocess invalid reflection fixture");
        reject(name+"-forged-declaration",Pack(CompileUnchecked(invalid,parts),true));
        Check(Build(shader,artifact,source/(name+".log"))!=0&&Read(artifact)==good,"reflection build failure preserves existing artifact");
        Write(source/"PbrBindings.hlsli",bindings);
    }
    D3D12ShaderSource valid;Check(ReadD3D12ShaderSource(sourceDescription,valid,text),"preprocess valid profile fixture");
    reject("wrong-actual-profile",Pack(CompileUnchecked(valid,parts,"vs_5_0"),true));
    const auto raw=CompileD3D12ShaderProgram(valid,text);Check(bool(raw),"source program for private data test");
    ComPtr<ID3DBlob> privatePixel;const char secret[]="must not ship";
    Hr(D3DSetBlobPart(raw->pixel->GetBufferPointer(),raw->pixel->GetBufferSize(),D3D_BLOB_PRIVATE_DATA,0,secret,sizeof(secret),privatePixel.GetAddressOf()),"attach private test data");
    switched=parts;switched.pixel.assign(static_cast<const char*>(privatePixel->GetBufferPointer()),privatePixel->GetBufferSize());
    reject("actual-private-chunk",Pack(switched,true));
    Write(shader,"UserShader source marker: invalid syntax\n");
    Check(Build(shader,artifact,source/"syntax.log")!=0&&Read(artifact)==good,"compile error leaves old package bytes");Write(shader,original);
    Check(SetFileAttributesW(artifact.c_str(),FILE_ATTRIBUTE_READONLY)!=FALSE,"make owned package readonly");
    struct Restore{fs::path path;~Restore(){SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_NORMAL);}} restore{artifact};
    Check(Build(shader,artifact,source/"readonly.log")!=0&&Read(artifact)==good,"readonly target preserved");
    Check(SetFileAttributesW(artifact.c_str(),FILE_ATTRIBUTE_NORMAL)!=FALSE,"restore owned fixture attributes");
    const auto directory=runtime/"directory-target";fs::create_directory(directory);
    Check(Build(shader,directory,source/"directory.log")!=0&&fs::is_directory(directory),"directory target rejected");
    for(const auto& entry:fs::directory_iterator(runtime))Check(entry.path().filename().wstring().find(L".tmp.")==std::wstring::npos,"failed atomic publication leaves no temporary file");
    Check(Read(artifact)==good,"negative suite never replaces good package");
    evidence["rejected_packages"]=rejected;evidence["interface_sha256"]=D3D12UserPbrInterfaceHash();
    evidence["package_sha256"]=se::Sha256(good);evidence["half_package_sha256"]=se::Sha256(Read(half));evidence["compiler_failure_preserves_package"]=true;
}
struct Fixture
{
    WindowState window;std::shared_ptr<rhi::Device> device;
    explicit Fixture(const Localization& text)
    {
        Check(CreateEngineWindow(window,false),"user Shader hidden window");RECT rect{0,0,320,240};
        AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,0);
        Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"user Shader window size");
        int exit=0;Check(PumpWindowMessages(exit),"user Shader messages");rhi::Result result;
        device=rhi::Device::CreateD3D12({window.hwnd,320,240},text,result);Ok(result,"user Shader device");
    }
    ~Fixture(){if(device)device->Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
struct Geometry{rhi::Buffer vertices,indices;rhi::Texture base,normal,mr;};
Geometry Create(rhi::Device& device)
{
    Geometry result;
    const std::array<rhi::Vertex,4> vertices{{{{-1,-1,0},{0,0,1},{0,1},{1,0,0,1}},{{1,-1,0},{0,0,1},{1,1},{1,0,0,1}},
        {{1,1,0},{0,0,1},{1,0},{1,0,0,1}},{{-1,1,0},{0,0,1},{0,0},{1,0,0,1}}}};
    const std::array<UINT,6> indices{0,1,2,0,2,3};Ok(device.BeginUploadBatch(),"user geometry batch");
    Ok(device.CreateBuffer({rhi::BufferUsage::Vertex,sizeof(rhi::Vertex),"UserShader.vertices"},std::as_bytes(std::span(vertices)),result.vertices),"user vertices");
    Ok(device.CreateBuffer({rhi::BufferUsage::Index,4,"UserShader.indices"},std::as_bytes(std::span(indices)),result.indices),"user indices");
    Ok(device.CreateTexture({1,1},std::array<std::uint8_t,4>{128,64,220,255},result.base),"user base texture");
    Ok(device.CreateTexture({1,1},std::array<std::uint8_t,4>{166,204,226,255},result.normal),"user normal texture");
    Ok(device.CreateTexture({1,1},std::array<std::uint8_t,4>{17,128,192,255},result.mr),"user metallic texture");
    rhi::CompletionToken token;Ok(device.EndUploadBatch(token),"user batch end");return result;
}
rhi::DrawIndexed Draw(const Geometry& geometry,rhi::PipelineHandle pipeline)
{
    rhi::DrawIndexed draw{*geometry.vertices,*geometry.indices,*geometry.base,pipeline,{},*se::OrthographicRH(4,3,0.1f,100)*se::Translation({0,0,-3})};
    draw.normalTexture=*geometry.normal;draw.metallicRoughnessTexture=*geometry.mr;
    draw.pbr.cameraPosition={0,0,3};draw.pbr.directionToLight={0.4f,0.5f,1};draw.pbr.radiance={1.5f,1.2f,0.9f};return draw;
}
std::unique_ptr<rhi::CommandContext> Record(rhi::Device& device,const rhi::DrawIndexed& draw)
{
    auto commands=device.BeginCommands();Check(bool(commands),"user draw packet");Ok(commands->BeginPass({rhi::PassKind::Clear}),"user clear");
    Ok(commands->EndPass(),"user clear end");Ok(commands->BeginPass({rhi::PassKind::Opaque}),"user opaque");
    Ok(commands->Draw(draw),"user draw");Ok(commands->EndPass(),"user opaque end");Ok(commands->Close(),"user packet close");return commands;
}
rhi::Image Capture(rhi::Device& device,const rhi::DrawIndexed& draw)
{auto commands=Record(device,draw);rhi::CompletionToken token;rhi::Image image;Ok(device.Submit(*commands,token,&image),"user capture");return image;}
void Save(const rhi::Image& image,const fs::path& path)
{
    std::ofstream output(path,std::ios::binary);output<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(size_t i=0;i<image.rgba8.size();i+=4)output.write(reinterpret_cast<const char*>(image.rgba8.data()+i),3);
    Check(bool(output),"user capture file");
}
void Gpu(const fs::path& source,const fs::path& runtime,const fs::path& output,const Localization& text,Json& evidence)
{
    Fixture fixture(text);auto& device=*fixture.device;auto geometry=Create(device);const auto package=runtime/"full.sebin";
    const auto good=Read(package),halfBytes=Read(runtime/"half.sebin");const auto description=Artifact(package);
    rhi::Pipeline full,half,original,duplicate;Ok(device.CreatePipeline(description,full),"runtime only artifact");
    Ok(device.CreatePipeline(Artifact(runtime/"half.sebin"),half),"runtime custom pixel artifact");
    Check(!device.Stats().shaderProgramCompilations&&device.Stats().shaderArtifactLoads==2,"runtime used artifacts, no compilation");
    Ok(device.CreatePipeline({rhi::PipelineKind::PbrMesh,source/"Custom.hlsl",2},original),"internal source+artifact remains compatible");
    auto draw=Draw(geometry,*full);draw.unlit=true;const auto image=Capture(device,draw);
    draw.pipeline=*original;Check(Capture(device,draw).rgba8==image.rgba8,"stripped artifact equals existing full source path pixels");
    const auto pixel=(size_t(120)*320+160)*4;const int expected[]{128,64,220};
    for(size_t channel=0;channel<3;++channel)Check(std::abs(int(image.rgba8[pixel+channel])-expected[channel])<=1,"base map unlit pixel reference");
    draw.pipeline=*half;const auto dimmed=Capture(device,draw);Check(dimmed.rgba8!=image.rgba8,"custom shader visibly changes image");
    for(size_t channel=0;channel<3;++channel)Check(std::abs(int(dimmed.rgba8[pixel+channel])-expected[channel]/2)<=1,"custom shader gain reference");
    Save(image,output/"source-equal.ppm");Save(dimmed,output/"custom-half.ppm");Json variations=Json::array();
    draw.unlit=false;draw.pipeline=*full;const auto lit=Capture(device,draw);
    for(int variant=0;variant<6;++variant)
    {
        auto changed=draw;if(variant==0)changed.tint={0.7f,0.5f,0.25f,1};if(variant==1)changed.pbr.metallic=1;
        if(variant==2)changed.pbr.roughness=0.2f;if(variant==3)changed.pbr.normalMapping=true;
        if(variant==4){changed.pbr.normalMapping=true;changed.pbr.normalScale=0.3f;}if(variant==5)changed.world=se::Rotation(se::AxisAngle({0,1,0},0.3f));
        auto custom=changed;custom.pipeline=*original;const auto actual=Capture(device,changed);
        Check(actual.rgba8==Capture(device,custom).rgba8&&actual.rgba8!=lit.rgba8,"material fields and transforms preserved by user interface");
        variations.push_back(se::Sha256(std::span(actual.rgba8)));if(variant==3)Save(actual,output/"normal-mapped.ppm");
    }
    original.reset();
    // 只重命名本次测试自己创建的目录；运行目录从始至终没有任何HLSL/include。
    auto offline=source;offline+=L"-offline";fs::rename(source,offline);
    struct RestoreSource{fs::path offline,source;~RestoreSource(){std::error_code error;fs::rename(offline,source,error);}} restoreSource{offline,source};
    for(const auto& entry:fs::recursive_directory_iterator(runtime))Check(entry.path().extension()!=L".hlsl"&&entry.path().extension()!=L".hlsli","runtime directory contains no source");
    Ok(device.CreatePipeline(description,duplicate),"source directory absent, cache revalidates artifact");
    const auto firstPso=rhi::D3D12DiagnosticsAccess::PipelineState(device,*full);
    Check(firstPso!=nullptr,"diagnostics expose actual PBR PSO");
    Check(rhi::D3D12DiagnosticsAccess::PipelineState(device,*duplicate)==firstPso,"valid identical package shares prepared PSO");
    const auto creations=device.Stats().pipelineCreations;Write(package,"broken while old pipeline is live");rhi::Pipeline rejected;
    Check(!device.CreatePipeline(description,rejected)&&!rejected&&device.Stats().pipelineCreations==creations,"changed corrupt package cannot hit live cache");
    draw=Draw(geometry,*full);draw.unlit=true;Check(Capture(device,draw).rgba8==image.rgba8,"last good pipeline survives bad package and missing source");
    for(int invalid=0;invalid<4;++invalid)
    {
        auto bad=description;if(invalid==0)bad.shaderInput=static_cast<rhi::ShaderInput>(99);
        if(invalid==1){bad.kind=rhi::PipelineKind::TexturedMesh;bad.bindingLayoutVersion=1;}
        if(invalid==2)bad.macros={{"EXTRA","1"}};if(invalid==3)bad.bindingLayoutVersion=1;
        Check(!device.CreatePipeline(bad,rejected)&&!rejected,"unsupported artifact modes rejected");
    }
    // 元数据不是签名：合法字节码仍须按真实字节码身份分缓存，不能只相信source_key。
    auto changed=Unpack(good);const auto halfParts=Unpack(halfBytes);changed.vertex=halfParts.vertex;changed.pixel=halfParts.pixel;
    Write(package,Pack(changed,true));rhi::Pipeline replaced;Ok(device.CreatePipeline(description,replaced),"same declared source identity with different valid code");
    Check(rhi::D3D12DiagnosticsAccess::PipelineState(device,*replaced)!=firstPso,"actual bytecode identity differentiates PSO cache");
    draw.pipeline=*replaced;Check(Capture(device,draw).rgba8==dimmed.rgba8,"replaced artifact uses current bytecode");replaced.reset();
    Write(package,good);draw.pipeline=*full;
    // 删除文件后普通帧仍可画，直接证明Draw/Submit不重新读取产物。
    const auto moved=runtime/"full.offline";fs::rename(package,moved);
    struct RestorePackage{fs::path offline,path;~RestorePackage(){std::error_code error;fs::rename(offline,path,error);}} restorePackage{moved,package};
    const auto before=device.Stats();rhi::CompletionToken token;
    for(UINT frame=0;frame<32;++frame){auto commands=Record(device,draw);Ok(device.Submit(*commands,token),"ordinary artifact-only frame");}
    const auto after=device.Stats();Check(after.shaderArtifactLoads==before.shaderArtifactLoads&&after.shaderProgramCompilations==before.shaderProgramCompilations
        &&after.pipelineCreations==before.pipelineCreations&&after.globalWaits==before.globalWaits,"prepared frames do not read, compile, create PSOs or flush");
    Check(Capture(device,draw).rgba8==image.rgba8,"file absent ordinary draw reference");fs::rename(moved,package);
    Ok(device.WaitIdle(),"before user PSO lifetime gate");const auto prepared=rhi::D3D12DiagnosticsAccess::Prepared(device,*full);
    Check(!prepared.expired(),"diagnostics expose live PBR prepared pipeline");
    auto commands=Record(device,draw);ComPtr<ID3D12Fence> fence;
    Hr(rhi::D3D12DiagnosticsAccess::NativeDevice(device)->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(fence.GetAddressOf())),"user PSO gate");
    struct Gate
    {
        ComPtr<ID3D12Fence> fence;HANDLE event;std::atomic<bool> timedOut{false};std::thread worker;
        explicit Gate(ComPtr<ID3D12Fence> value):fence(std::move(value)),event(CreateEventW(nullptr,TRUE,FALSE,nullptr))
        {Check(event!=nullptr,"user gate event");worker=std::thread([this]{if(WaitForSingleObject(event,3000)==WAIT_TIMEOUT){timedOut=true;fence->Signal(1);}});}
        void Release(){Hr(fence->Signal(1),"release user gate");SetEvent(event);}
        ~Gate(){fence->Signal(1);SetEvent(event);if(worker.joinable())worker.join();CloseHandle(event);}
    } gate{fence};
    Hr(rhi::D3D12DiagnosticsAccess::Queue(device)->Wait(fence.Get(),1),"hold user PSO in flight");Ok(device.Submit(*commands,token),"submit gated user PSO");
    Check(!device.IsComplete(token)&&!gate.timedOut,"user GPU frame actually in flight");full.reset();duplicate.reset();device.Collect();
    Check(!prepared.expired(),"in-flight prepared PSO retained after logical owner release");gate.Release();Ok(device.WaitIdle(),"drain user PSO");device.Collect();
    Check(prepared.expired()&&!gate.timedOut,"prepared PSO retires exactly after GPU completion");
    const auto stats=device.Stats();Check(!stats.shaderProgramCompilations&&!stats.debugWarnings&&!stats.debugErrors,"zero runtime compiles and GPU warnings/errors");
    evidence["runtime_compilations"]=stats.shaderProgramCompilations;evidence["gpu_warnings"]=stats.debugWarnings;evidence["gpu_errors"]=stats.debugErrors;
    evidence["material_variants"]=variations;evidence["source_equal_rgba_sha256"]=se::Sha256(std::span(image.rgba8));evidence["custom_half_rgba_sha256"]=se::Sha256(std::span(dimmed.rgba8));
    evidence["no_source_load"]=true;evidence["ordinary_frames_without_file"]=32;evidence["bytecode_cache_identity"]=true;evidence["fence_retirement"]=true;
    auto allocator=rhi::D3D12DiagnosticsAccess::Memory(device);const std::weak_ptr<D3D12MemoryAllocator> allocatorLifetime=allocator;
    half.reset();geometry={};commands.reset();device.Shutdown();const auto memory=allocator->Stats();
    Check(!memory.used&&!memory.resources,"user Shader shutdown frees DEFAULT resource leases");
    allocator.reset();Check(allocatorLifetime.expired(),"diagnostic owner release destroys allocator and cached empty heaps");
    evidence["shutdown_default_zero"]=true;
}
void CrossLoad(const fs::path& package,const fs::path& output,const Localization& text)
{
    Check(fs::create_directory(output),"new cross-configuration output");Fixture fixture(text);auto& device=*fixture.device;
    auto geometry=Create(device);rhi::Pipeline pipeline;Ok(device.CreatePipeline(Artifact(package),pipeline),"other build configuration artifact");
    auto draw=Draw(geometry,*pipeline);draw.unlit=true;const auto image=Capture(device,draw);const auto offset=(size_t(120)*320+160)*4;
    const int expected[]{128,64,220};for(size_t channel=0;channel<3;++channel)Check(std::abs(int(image.rgba8[offset+channel])-expected[channel])<=1,"cross-build pixel reference");
    const auto stats=device.Stats();Check(stats.shaderArtifactLoads==1&&!stats.shaderProgramCompilations&&!stats.debugWarnings&&!stats.debugErrors,"cross-build uses artifact only");
    Save(image,output/"reference.ppm");Write(output/"verification.json",Json{{"success",true},{"package_sha256",se::Sha256(Read(package))},
        {"rgba_sha256",se::Sha256(std::span(image.rgba8))},{"runtime_compilations",stats.shaderProgramCompilations},{"gpu_warnings",stats.debugWarnings},{"gpu_errors",stats.debugErrors}}.dump(2)+"\n");
}
}
int wmain(int argc,wchar_t** argv)
{
    try
    {
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"user Shader localization");
        if(argc==4&&std::wstring_view(argv[1])==L"--load")
        {CrossLoad(fs::absolute(argv[2]),fs::absolute(argv[3]),text);std::cout<<"Cross-configuration user Shader load passed.\n";return 0;}
        Check(argc==2,"supply UserShader output directory");const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        const auto root=output/(L"用户Shader-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        const auto source=root/"authoring",runtime=root/"runtime";fs::create_directories(source);fs::create_directories(runtime);
        for(const auto* name:{L"Pbr.hlsl",L"PbrBindings.hlsli"})fs::copy_file(GetExecutableDirectory()/"Resources/Shaders"/name,source/name);
        Write(source/"Custom.hlsl","// UserShader source marker\n#ifndef USER_GAIN\n#define USER_GAIN 1.0\n#endif\n#define PSMain EnginePbrPS\n#include \"Pbr.hlsl\"\n#undef PSMain\nfloat4 PSMain(PixelInput input,bool frontFace:SV_IsFrontFace):SV_TARGET {float4 color=EnginePbrPS(input,frontFace);return float4(color.rgb*USER_GAIN,color.a);}\n");
        Json evidence{{"schema",1},{"success",false}};Cpu(source,runtime,text,evidence);Gpu(source,runtime,output,text,evidence);
        const auto relative=(runtime/"full.sebin").lexically_relative(output).generic_u8string();
        evidence["package_relative"]=std::string(reinterpret_cast<const char*>(relative.data()),relative.size());
        evidence["success"]=true;Write(output/"verification.json",evidence.dump(2)+"\n");
        std::cout<<"UserShaderTests passed: "<<evidence["rejected_packages"].size()<<" malformed packages, source-free GPU, six material variants, bytecode cache identity, Fence lifetime; GPU warnings/errors=0.\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"UserShaderTests: "<<error.what()<<'\n';return 1;}
}
