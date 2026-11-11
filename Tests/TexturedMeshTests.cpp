#include "CoreLayer/Localization.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "ResourceLayer/Assets/AssetStore.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "Camera/CameraDemo.h"
#include <d3d12sdklayers.h>
#include <wincodec.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <fstream>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
void Hr(HRESULT value,const char* message){Check(SUCCEEDED(value),message);}
void Size(WindowState& window,UINT width,UINT height)
{
    RECT rect{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    Check(AdjustWindowRectEx(&rect,static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_STYLE)),FALSE,
        static_cast<DWORD>(GetWindowLongPtrW(window.hwnd,GWL_EXSTYLE)))!=FALSE,"client rect adjustment");
    Check(SetWindowPos(window.hwnd,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,
        SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=FALSE,"real hidden HWND resize");
    int code=0;Check(PumpWindowMessages(code)&&window.width==width&&window.height==height,"client dimensions");
}
struct Fixture
{
    WindowState window;
    D3D12Context graphics;
    D3D12Presentation presentation;
    D3D12MeshPipeline pipeline;
    std::deque<D3D12Mesh> meshes;
    ~Fixture(){ReleaseD3D12Presentation(presentation);meshes.clear();pipeline={};window.postQuitOnDestroy=false;DestroyEngineWindow(window);}
};
struct Picture{UINT width=0,height=0;std::vector<std::uint8_t> pixels;};
ComPtr<ID3D12Resource> ReadbackBuffer(ID3D12Device& device,UINT64 bytes)
{
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
    desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc={1,0};desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    Hr(device.CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,
        IID_PPV_ARGS(result.GetAddressOf())),"create readback buffer");return result;
}
Picture ReadRows(ID3D12Resource& buffer,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout,UINT64 bytes,UINT width,UINT height)
{
    Picture image{width,height,std::vector<std::uint8_t>(size_t(width)*height*4)};
    void* mapped=nullptr;const D3D12_RANGE range{0,static_cast<SIZE_T>(bytes)};
    Hr(buffer.Map(0,&range,&mapped),"map completed GPU copy");
    for(UINT y=0;y<height;++y)std::memcpy(image.pixels.data()+size_t(y)*width*4,
        static_cast<const std::byte*>(mapped)+layout.Offset+size_t(y)*layout.Footprint.RowPitch,size_t(width)*4);
    const D3D12_RANGE noWrites{0,0};buffer.Unmap(0,&noWrites);return image;
}
struct CopyGuard
{
    D3D12Presentation& presentation;bool pending=false;
    ~CopyGuard(){if(pending)ReleaseD3D12Presentation(presentation);}
};
se::Mat4 ViewProjection(const Fixture& fixture)
{
    return *se::PerspectiveRH(se::Pi/3,float(fixture.presentation.width)/fixture.presentation.height,0.1f,100)
        *se::Translation({0,0,-3});
}
Picture Capture(Fixture& fixture,std::span<const D3D12MeshDraw> draws,const Localization& text)
{
    auto& p=fixture.presentation;
    const auto desc=p.backBuffers[p.swapChain->GetCurrentBackBufferIndex()]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 bytes=0;
    fixture.graphics.device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
    auto buffer=ReadbackBuffer(*fixture.graphics.device.Get(),bytes);
    CopyGuard guard{p};
    const D3D12FrameCapture capture{buffer.Get(),layout};
    D3D12FrameContent content;content.meshPipeline=&fixture.pipeline;content.meshes=draws;content.viewProjection=ViewProjection(fixture);
    guard.pending=true;
    Check(RenderD3D12Frame(p,text,content,&capture),"record indexed textured draw and capture");
    Check(WaitForD3D12Gpu(p,text),"complete framebuffer capture");guard.pending=false;
    return ReadRows(*buffer.Get(),layout,bytes,p.width,p.height);
}
Picture ReadTexture(Fixture& fixture,const D3D12Mesh& mesh,const Localization& text)
{
    auto& p=fixture.presentation;
    Check(WaitForD3D12Gpu(p,text),"safe texture readback boundary");
    const auto desc=mesh.texture->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};UINT64 bytes=0;
    fixture.graphics.device->GetCopyableFootprints(&desc,0,1,0,&layout,nullptr,nullptr,&bytes);
    auto buffer=ReadbackBuffer(*fixture.graphics.device.Get(),bytes);CopyGuard guard{p};
    Hr(p.frames[0].allocator->Reset(),"reset texture copy allocator");
    Hr(p.commandList->Reset(p.frames[0].allocator.Get(),nullptr),"reset texture copy commands");
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={mesh.texture.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE};
    p.commandList->ResourceBarrier(1,&barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=buffer.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint=layout;
    D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=mesh.texture.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    p.commandList->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);p.commandList->ResourceBarrier(1,&barrier);
    Hr(p.commandList->Close(),"close texture copy");ID3D12CommandList* lists[]{p.commandList.Get()};
    guard.pending=p.gpuWorkPending=p.untrackedSubmission=true;p.queue->ExecuteCommandLists(1,lists);
    Check(WaitForD3D12Gpu(p,text),"complete texture copy");guard.pending=false;
    return ReadRows(*buffer.Get(),layout,bytes,mesh.textureWidth,mesh.textureHeight);
}
void Pixel(const Picture& image,UINT x,UINT y,std::array<int,4> expected,int tolerance=2)
{
    const auto offset=(size_t(y)*image.width+x)*4;
    for(size_t channel=0;channel<4;++channel)
    {
        const int actual=image.pixels[offset+channel];
        if(std::abs(actual-expected[channel])>tolerance)
            throw std::runtime_error("pixel channel "+std::to_string(channel)+" expected "+std::to_string(expected[channel])+" actual "+std::to_string(actual));
    }
}
void Save(const Picture& image,const fs::path& path)
{
    std::ofstream file(path,std::ios::binary);file<<"P6\n"<<image.width<<' '<<image.height<<"\n255\n";
    for(size_t offset=0;offset<image.pixels.size();offset+=4)file.write(reinterpret_cast<const char*>(image.pixels.data()+offset),3);
    Check(bool(file),"save texture GPU pixels");
}
std::vector<D3D12MeshVertex> Vertices(const se::MeshData& mesh)
{
    std::vector<D3D12MeshVertex> result;
    for(const auto& vertex:mesh.vertices)result.push_back({vertex.position,vertex.normal,vertex.uv,vertex.tangent});
    return result;
}
D3D12Mesh& Upload(Fixture& fixture,const se::ImageData& image,const Localization& text)
{
    const auto cube=se::MakeUnitCube();const auto vertices=Vertices(cube);
    auto& mesh=fixture.meshes.emplace_back();
    Check(InitializeD3D12Mesh(mesh,fixture.graphics,fixture.presentation,vertices,cube.indices,
        {image.width,image.height,image.rgba8},text),"upload canonical cube and RGBA texture");
    return mesh;
}
void WriteImage(const fs::path& path,const GUID& container,const se::ImageData& image)
{
    const HRESULT initialized=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    Check(SUCCEEDED(initialized)||initialized==RPC_E_CHANGED_MODE,"image encoder COM apartment");
    struct Apartment{HRESULT status;~Apartment(){if(SUCCEEDED(status))CoUninitialize();}} apartment{initialized};
    ComPtr<IWICImagingFactory> factory;Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(factory.GetAddressOf())),"create fixture encoder factory");
    ComPtr<IWICStream> stream;Hr(factory->CreateStream(&stream),"create fixture image stream");
    Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"open fixture image output");
    ComPtr<IWICBitmapEncoder> encoder;Hr(factory->CreateEncoder(container,nullptr,&encoder),"create fixture image encoder");
    Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"initialize fixture encoder");
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;
    Hr(encoder->CreateNewFrame(&frame,&properties),"create fixture image frame");Hr(frame->Initialize(properties.Get()),"initialize fixture frame");
    Hr(frame->SetSize(image.width,image.height),"fixture image dimensions");
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppRGBA;
    Hr(frame->SetPixelFormat(&format),"fixture encoding pixel format");
    // SetPixelFormat 返回编码器实际接受的格式，用 WIC 转换后再写入。
    ComPtr<IWICBitmap> source;ComPtr<IWICFormatConverter> converter;
    Hr(factory->CreateBitmapFromMemory(image.width,image.height,GUID_WICPixelFormat32bppRGBA,image.width*4,
        static_cast<UINT>(image.rgba8.size()),const_cast<BYTE*>(image.rgba8.data()),&source),"fixture RGBA bitmap");
    Hr(factory->CreateFormatConverter(&converter),"fixture format converter");
    Hr(converter->Initialize(source.Get(),format,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom),"fixture encoder format conversion");
    Hr(frame->WriteSource(converter.Get(),nullptr),"encode converted fixture pixels");
    Hr(frame->Commit(),"commit fixture frame");Hr(encoder->Commit(),"commit fixture image");
}
void NoProblems(ID3D12InfoQueue* info)
{
    if(!info)return;
    UINT problems=0;
    for(UINT64 index=0;index<info->GetNumStoredMessagesAllowedByRetrievalFilter();++index)
    {
        SIZE_T bytes=0;Hr(info->GetMessage(index,nullptr,&bytes),"validation message size");
        std::vector<std::uint8_t> storage(bytes);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Hr(info->GetMessage(index,message,&bytes),"validation message");
        if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){++problems;std::cerr<<message->pDescription<<'\n';}
    }
    std::cout<<"Textured mesh InfoQueue Warning/Error/Corruption="<<problems<<'\n';Check(problems==0,"no native GPU validation problems");
}

void Tests(const fs::path& output,const Localization& text)
{
    const auto cube=se::MakeUnitCube();Check(cube.vertices.size()==24&&cube.indices.size()==36,"cube face and index counts");
    for(const auto& vertex:cube.vertices)
    {
        Check(std::abs(vertex.position[0])<=0.5f&&std::abs(vertex.position[1])<=0.5f&&std::abs(vertex.position[2])<=0.5f,"unit cube bounds");
        Check(std::abs(vertex.normal[0]*vertex.normal[0]+vertex.normal[1]*vertex.normal[1]+vertex.normal[2]*vertex.normal[2]-1)<1e-6f,"face normal unit length");
    }
    for(size_t index=0;index<cube.indices.size();index+=3)
    {
        const auto& a=cube.vertices[cube.indices[index]];const auto& b=cube.vertices[cube.indices[index+1]];const auto& c=cube.vertices[cube.indices[index+2]];
        const se::Vec3 p0{a.position[0],a.position[1],a.position[2]},p1{b.position[0],b.position[1],b.position[2]},p2{c.position[0],c.position[1],c.position[2]};
        Check(se::Dot(se::Cross(p1-p0,p2-p0),{a.normal[0],a.normal[1],a.normal[2]})>0,"cube triangle outward winding");
    }
    Fixture fixture;Check(CreateEngineWindow(fixture.window,false),"create texture test window");Size(fixture.window,320,240);
    Check(InitializeD3D12Context(fixture.graphics,text),"create texture test hardware device");
    ComPtr<ID3D12InfoQueue> info;
#if defined(_DEBUG)
    Hr(fixture.graphics.device.As(&info),"Debug InfoQueue required");
#endif
    Check(InitializeD3D12Presentation(fixture.presentation,fixture.graphics,fixture.window.hwnd,text),"initialize color/depth/frame constants");
    Check(InitializeD3D12MeshPipeline(fixture.pipeline,fixture.graphics,GetExecutableDirectory()/"Resources/Shaders/TexturedMesh.hlsl",text),"initialize indexed texture pipeline");
    auto& p=fixture.presentation;
    se::ImageData gray{3,2,std::vector<std::uint8_t>(3*2*4,128)};
    for(size_t index=3;index<gray.rgba8.size();index+=4)gray.rgba8[index]=255;
    const auto vertices=Vertices(cube);auto badIndices=cube.indices;badIndices[0]=999;
    D3D12Mesh invalid;
    Check(!InitializeD3D12Mesh(invalid,fixture.graphics,p,vertices,badIndices,{gray.width,gray.height,gray.rgba8},text)
        &&!invalid.vertices&&!invalid.texture,"bad index rejected without output");
    Check(!InitializeD3D12Mesh(invalid,fixture.graphics,p,vertices,cube.indices,{0,2,gray.rgba8},text),"bad image size rejected");
    auto badVertices=vertices;badVertices[0].normal={0,0,0};
    Check(!InitializeD3D12Mesh(invalid,fixture.graphics,p,badVertices,cube.indices,{gray.width,gray.height,gray.rgba8},text)
        &&!invalid.vertices,"zero normal rejected before GPU allocation");
    auto& mesh=Upload(fixture,gray,text);auto* existing=mesh.vertices.Get();
    Check(!InitializeD3D12Mesh(mesh,fixture.graphics,p,vertices,cube.indices,{gray.width,gray.height,gray.rgba8},text)
        &&mesh.vertices.Get()==existing,"repeated mesh initialization preserves original");
    for(const auto dimensions:{std::array<UINT,2>{3,2},{7,5},{257,3}})
    {
        se::ImageData image{dimensions[0],dimensions[1],std::vector<std::uint8_t>(size_t(dimensions[0])*dimensions[1]*4)};
        for(UINT y=0;y<image.height;++y)for(UINT x=0;x<image.width;++x)
        {
            const auto offset=(size_t(y)*image.width+x)*4;
            image.rgba8[offset]=static_cast<std::uint8_t>((x*37+y*13)%256);
            image.rgba8[offset+1]=static_cast<std::uint8_t>((x*11+y*61)%256);
            image.rgba8[offset+2]=static_cast<std::uint8_t>((x+y*97)%256);image.rgba8[offset+3]=255;
        }
        const auto& uploaded=Upload(fixture,image,text);
        Check(ReadTexture(fixture,uploaded,text).pixels==image.rgba8,"unaligned rows round trip exact RGBA bytes");
        std::cout<<"Texture rows passed: "<<image.width<<'x'<<image.height<<'\n';
    }
    std::array<D3D12MeshDraw,1> draw{{{&mesh,{}, {1,1,1,1},false,true}}};
    auto color=Capture(fixture,draw,text);Pixel(color,160,120,{128,128,128,255});Save(color,output/"srgb.ppm");
    draw[0].textureIsData=true;color=Capture(fixture,draw,text);Pixel(color,160,120,{188,188,188,255});Save(color,output/"linear-data.ppm");
    draw[0].textureIsData=false;
    std::array<D3D12MeshDraw,2> depth{{{&mesh,{}, {1,0,0,1},false,true},
        {&mesh,se::Translation({0.1f,0,-1.5f})*se::Scaling({2,2,2}),{0,0,1,1},false,true}}};
    auto first=Capture(fixture,depth,text);Pixel(first,160,120,{128,0,0,255});
    std::swap(depth[0],depth[1]);auto reversed=Capture(fixture,depth,text);
    Check(first.pixels==reversed.pixels,"depth image independent of near/far draw order");Save(first,output/"depth-order.ppm");
    auto depthDescription=p.depth->GetDesc();Check(depthDescription.Width==320&&depthDescription.Height==240
        &&depthDescription.Format==DXGI_FORMAT_D32_FLOAT,"D32 target matches surface");
    D3D12FrameContent content;content.meshPipeline=&fixture.pipeline;content.viewProjection=ViewProjection(fixture);content.meshes=draw;
    const auto flushes=p.flushCounts;
    for(UINT frame=0;frame<64;++frame)
    {
        draw[0].world=se::Rotation(se::AxisAngle({0,1,0},float(frame)*0.01f));
        draw[0].tint=frame%2?se::Vec4{1,0,0,1}:se::Vec4{0,0,1,1};
        Check(RenderD3D12Frame(p,text,content),"64 ordinary constant-buffer frames");
    }
    Check(p.flushCounts==flushes,"ordinary mesh frames do not flush queue");
    std::vector<D3D12MeshDraw> many(16,{&mesh,{}, {0,1,0,1},false,true});many[0].tint={1,0,0,1};
    color=Capture(fixture,many,text);Pixel(color,160,120,{128,0,0,255});
    Check(p.frames[(p.frameCursor+1)%2].constantCursor==4096,"16 distinct 256-byte constant allocations");
    many.push_back(many[0]);content.meshes=many;
    Check(RenderD3D12Frame(p,text,content),"17 draws use dynamic constant slices");
    content.meshes=draw;Check(RenderD3D12Frame(p,text,content),"valid frame after rejected draw count");
    for(const auto dimensions:{std::array<UINT,2>{511,257},{256,256},{640,360},{320,240}})
    {
        Size(fixture.window,dimensions[0],dimensions[1]);Check(ResizeD3D12Presentation(p,dimensions[0],dimensions[1],text),"resize color and depth together");
        depthDescription=p.depth->GetDesc();Check(depthDescription.Width==dimensions[0]&&depthDescription.Height==dimensions[1],"resized depth dimensions");
        content.viewProjection=ViewProjection(fixture);Check(RenderD3D12Frame(p,text,content),"draw after depth resize");
    }
    const auto marker=p.nextFenceValue;auto* oldDepth=p.depth.Get();
    Check(ResizeD3D12Presentation(p,0,240,text)&&ResizeD3D12Presentation(p,320,240,text)
        &&p.depth.Get()==oldDepth&&p.nextFenceValue==marker,"zero/same sizes preserve depth and submit nothing");
    for(const auto& format:{std::pair<const char*,const GUID*>{"fixture.png",&GUID_ContainerFormatPng},
        {"fixture.jpg",&GUID_ContainerFormatJpeg}})
    {
        const auto path=output/format.first;WriteImage(path,*format.second,gray);
        se::ImageData decoded;std::vector<se::AssetDiagnostic> diagnostics;
        Check(se::DecodeImage(path,decoded,diagnostics)&&decoded.width==3&&decoded.height==2,"PNG/JPEG real WIC decode");
        auto& decodedMesh=Upload(fixture,decoded,text);draw[0]={&decodedMesh,{}, {1,1,1,1},false,true};
        Pixel(Capture(fixture,draw,text),160,120,{128,128,128,255},3);
    }
    se::ImageData unchanged{77,88,{99}};std::vector<se::AssetDiagnostic> diagnostics;
    Check(!se::DecodeImage(output/"missing.png",unchanged,diagnostics)&&unchanged.width==77&&unchanged.rgba8==std::vector<std::uint8_t>{99},"decode failure preserves output");
    {std::ofstream broken(output/"broken.png",std::ios::binary);broken<<"not an image";}
    Check(!se::DecodeImage(output/"broken.png",unchanged,diagnostics)&&unchanged.width==77,"corrupt image failure preserves output");
    se::ImageData checker;diagnostics.clear();
    Check(se::DecodeImage(GetExecutableDirectory()/"Resources/Textures/checker.png",checker,diagnostics),"load actual sample texture");
    auto& checkered=Upload(fixture,checker,text);draw[0]={&checkered,se::Rotation(se::AxisAngle({0,1,0},0.6f))*se::Rotation(se::AxisAngle({1,0,0},0.35f)),{1,1,1,1},false,false};
    Save(Capture(fixture,draw,text),output/"textured-cube.ppm");
    content.meshes=draw;content.viewProjection=ViewProjection(fixture);Check(RenderD3D12Frame(p,text,content),"submit immediately before cleanup");
    ReleaseD3D12Presentation(p);fixture.meshes.clear();fixture.pipeline={};
    Check(!p.depth&&!p.frames[0].uploads.Stats().reservedBytes&&!p.frames[1].uploads.Stats().reservedBytes&&IsWindow(fixture.window.hwnd),"drain before releasing color/depth/constant/mesh resources");
    ReleaseD3D12Presentation(p);NoProblems(info.Get());
}
void HostSmoke()
{
    se::CameraDemo demo(true);SandboxRenderer renderer(SandboxScene::TexturedCube);
    auto services=demo.Services();services.maximumFrames=8;
    Check(RunSandboxApp("en-US",renderer,false,&services)==0&&demo.Scene().Size()==3,"actual cube World and renderer host");
    const auto handle=demo.Scene().FindByStableId(2);Check(handle&&demo.Scene().GetTransform(handle)->rotation.y!=0,"cube motion owned by World update");
}
}
int main(int argc,char* argv[])
{
    try
    {
        Check(argc==2,"output directory argument required");const auto output=fs::absolute(argv[1]);fs::create_directories(output);
        Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"load language resources");
        Tests(output,text);HostSmoke();std::cout<<"TexturedMeshTests passed (rows, sRGB, depth, frame constants, resize, PNG/JPEG, host)\n";return 0;
    }
    catch(const std::exception& error){std::cerr<<"TexturedMeshTests failed: "<<error.what()<<'\n';return 1;}
}
