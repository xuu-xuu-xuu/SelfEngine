#include "CoreLayer/Localization.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Context.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Presentation.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Triangle.h"
#include "PlatformLayer/Graphics/D3D12/D3D12Dred.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "PlatformLayer/Windows/Window.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Application/SandboxApp.h"
#include "Camera/CameraDemo.h"
#include <d3d12sdklayers.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{
void Check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void CheckHr(HRESULT value, const char* message) { Check(SUCCEEDED(value), message); }

struct Fixture
{
    WindowState window;
    D3D12Context graphics;
    D3D12Presentation presentation;
    D3D12Triangle triangle;
    D3D12Triangle variant;
    ~Fixture()
    {
        ReleaseD3D12Presentation(presentation);
        triangle = {};
        variant = {};
        window.postQuitOnDestroy = false;
        DestroyEngineWindow(window);
    }
};

void SetClientSize(WindowState& window, UINT width, UINT height)
{
    RECT outer{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    const auto style = static_cast<DWORD>(GetWindowLongPtrW(window.hwnd, GWL_STYLE));
    const auto extended = static_cast<DWORD>(GetWindowLongPtrW(window.hwnd, GWL_EXSTYLE));
    Check(AdjustWindowRectEx(&outer, style, FALSE, extended) != FALSE, "adjust client size");
    Check(SetWindowPos(window.hwnd, nullptr, 0, 0, outer.right - outer.left, outer.bottom - outer.top,
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE, "resize real HWND");
    int exitCode = 0;
    Check(PumpWindowMessages(exitCode), "window pump during resize");
    Check(window.width == width && window.height == height, "platform client size matches");
}

struct Image
{
    UINT width, height;
    std::vector<unsigned char> pixels;
};

Image ReadBack(Fixture& fixture, const D3D12Triangle* triangle, const Localization& localization,
    const se::Mat4& clipFromWorld = {})
{
    auto& presentation = fixture.presentation;
    const UINT index = presentation.swapChain->GetCurrentBackBufferIndex();
    const auto textureDescription = presentation.backBuffers[index]->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 total = 0;
    fixture.graphics.device->GetCopyableFootprints(&textureDescription, 0, 1, 0, &footprint,
        nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = total;
    buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
    buffer.SampleDesc = { 1, 0 };
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    // 若异常发生在复制提交后，先同步，随后局部复制资源才析构。
    struct CopyLifetime
    {
        D3D12Presentation& presentation;
        bool pending = false;
        ~CopyLifetime() { if (pending) ReleaseD3D12Presentation(presentation); }
    } lifetime{ presentation };
    CheckHr(fixture.graphics.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(readback.GetAddressOf())), "create readback");
    D3D12FrameCapture capture{ readback.Get(), footprint };
    auto invalidCapture = capture;
    invalidCapture.footprint.Footprint.RowPitch += D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
    const auto beforeInvalid = presentation.submittedFrames;
    Check(!RenderD3D12Frame(presentation, localization, triangle, &invalidCapture, clipFromWorld)
        && presentation.submittedFrames == beforeInvalid && !presentation.failed,
        "invalid capture rejected before submitting commands");
    lifetime.pending = true;
    Check(RenderD3D12Frame(presentation, localization, triangle, &capture, clipFromWorld), "capture before Present");
    Check(WaitForD3D12Gpu(presentation, localization), "readback GPU completion");
    lifetime.pending = false;

    Image image{ presentation.width, presentation.height,
        std::vector<unsigned char>(static_cast<size_t>(presentation.width) * presentation.height * 4) };
    void* mapped = nullptr;
    const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(total) };
    CheckHr(readback->Map(0, &readRange, &mapped), "map GPU pixels");
    for (UINT row = 0; row < image.height; ++row)
        std::memcpy(image.pixels.data() + static_cast<size_t>(row) * image.width * 4,
            static_cast<const unsigned char*>(mapped) + footprint.Offset
                + static_cast<size_t>(row) * footprint.Footprint.RowPitch, static_cast<size_t>(image.width) * 4);
    const D3D12_RANGE noWrites{ 0, 0 };
    readback->Unmap(0, &noWrites);
    return image;
}

void Pixel(const Image& image, UINT x, UINT y, const std::array<int, 4>& expected, int tolerance)
{
    const size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
    for (size_t channel = 0; channel < expected.size(); ++channel)
    {
        const int actual = image.pixels[offset + channel];
        if (std::abs(actual - expected[channel]) > tolerance)
        {
            std::ostringstream error;
            error << "pixel (" << x << ',' << y << ") channel " << channel << ": expected "
                << expected[channel] << ", actual " << actual;
            throw std::runtime_error(error.str());
        }
    }
}

void TrianglePixels(const Image& image)
{
    Pixel(image, 2, 2, { 20, 46, 82, 255 }, 1);
    Pixel(image, image.width / 2, image.height / 2, { 128, 64, 64, 255 }, 3);
    size_t colored = 0;
    for (size_t offset = 0; offset < image.pixels.size(); offset += 4)
        if (std::abs(int(image.pixels[offset]) + image.pixels[offset + 1] + image.pixels[offset + 2] - 255) <= 2)
            ++colored;
    const size_t area = static_cast<size_t>(image.width) * image.height;
    Check(colored > area / 5 && colored < area / 4, "triangle covers expected pixel region");
}

void SaveImage(const Image& image, const fs::path& output)
{
    std::ofstream file(output, std::ios::binary);
    file << "P6\n" << image.width << ' ' << image.height << "\n255\n";
    for (size_t offset = 0; offset < image.pixels.size(); offset += 4)
        file.write(reinterpret_cast<const char*>(image.pixels.data() + offset), 3);
    Check(bool(file), "save GPU pixel image");
}

void ProjectedTrianglePixels(const Image& image, const se::CameraSnapshot& camera)
{
    const se::Vec3 vertices[]{ {0,0.65f,0}, {0.65f,-0.65f,0}, {-0.65f,-0.65f,0} };
    float expectedMinX=float(image.width), expectedMinY=float(image.height), expectedMaxX=0, expectedMaxY=0;
    for(const auto& vertex:vertices)
    {
        const auto screen=se::ProjectWorldPoint(camera,vertex);
        Check(screen.has_value(),"CPU camera projects each world vertex");
        expectedMinX=(std::min)(expectedMinX,screen->x); expectedMaxX=(std::max)(expectedMaxX,screen->x);
        expectedMinY=(std::min)(expectedMinY,screen->y); expectedMaxY=(std::max)(expectedMaxY,screen->y);
    }
    UINT minX=image.width,minY=image.height,maxX=0,maxY=0;
    size_t count=0;
    for(UINT y=0;y<image.height;++y)for(UINT x=0;x<image.width;++x)
    {
        const size_t offset=(static_cast<size_t>(y)*image.width+x)*4;
        if(std::abs(int(image.pixels[offset])+image.pixels[offset+1]+image.pixels[offset+2]-255)>2)continue;
        ++count; minX=(std::min)(minX,x);minY=(std::min)(minY,y);maxX=(std::max)(maxX,x);maxY=(std::max)(maxY,y);
    }
    std::cout<<"CPU bounds="<<expectedMinX<<','<<expectedMinY<<".."<<expectedMaxX<<','<<expectedMaxY
        <<" GPU bounds="<<minX<<','<<minY<<".."<<maxX<<','<<maxY<<'\n';
    Check(count>500 && std::abs(float(minX)-expectedMinX)<=2 && std::abs(float(maxX)-expectedMaxX)<=2
        && std::abs(float(minY)-expectedMinY)<=2 && std::abs(float(maxY)-expectedMaxY)<=2,
        "CPU projection and GPU raster bounds agree within two pixels");
    const auto centroid=se::ProjectWorldPoint(camera,{0,-0.65f/3,0});
    Check(centroid.has_value(),"project world-space color centroid");
    Pixel(image,static_cast<UINT>(centroid->x),static_cast<UINT>(centroid->y),{85,85,85,255},6);
    Pixel(image,2,2,{20,46,82,255},1);
}

void CameraGpuTests(Fixture& fixture,const fs::path& output,const Localization& localization)
{
    auto build=[&](const se::Mat4& world,se::CameraComponent lens=se::CameraComponent{se::Pi/2,0.1f,100,true}) {
        se::CameraSnapshot result;
        se::CameraError error;
        Check(se::BuildCameraSnapshot({1,world,lens,true},{0,0,float(fixture.presentation.width),float(fixture.presentation.height)},
            1,result,error),"build GPU reference camera");
        return result;
    };
    auto baseline=build(se::Translation({0,0,2}));
    auto image=ReadBack(fixture,&fixture.triangle,localization,baseline.viewProjection);
    ProjectedTrianglePixels(image,baseline); SaveImage(image,output/"camera.ppm");
    const auto moved=build(se::Translation({0.5f,0,2}));
    image=ReadBack(fixture,&fixture.triangle,localization,moved.viewProjection);
    ProjectedTrianglePixels(image,moved); SaveImage(image,output/"camera-moved.ppm");
    const auto rotated=build(se::Translation({0,0,2})*se::Rotation(se::AxisAngle({0,1,0},-0.2f)));
    image=ReadBack(fixture,&fixture.triangle,localization,rotated.viewProjection);
    ProjectedTrianglePixels(image,rotated); SaveImage(image,output/"camera-look.ppm");
    SetClientSize(fixture.window,640,240);
    Check(ResizeD3D12Presentation(fixture.presentation,640,240,localization),"resize camera aspect");
    const auto wide=build(se::Translation({0,0,2}));
    image=ReadBack(fixture,&fixture.triangle,localization,wide.viewProjection);
    ProjectedTrianglePixels(image,wide); SaveImage(image,output/"camera-wide.ppm");
    SetClientSize(fixture.window,320,240);
    Check(ResizeD3D12Presentation(fixture.presentation,320,240,localization),"restore reference viewport");
    const se::CameraSnapshot clipped[]{
        build(se::Translation({0,0,2}),{se::Pi/2,3,100,true}),
        build(se::Translation({0,0,2}),{se::Pi/2,0.1f,1,true}),
        build(se::Translation({0,0,2})*se::Rotation(se::AxisAngle({0,1,0},se::Pi)))
    };
    for(const auto& camera:clipped)
    {
        const auto clear=ReadBack(fixture,&fixture.triangle,localization,camera.viewProjection);
        for(UINT y=0;y<clear.height;++y)for(UINT x=0;x<clear.width;++x)Pixel(clear,x,y,{20,46,82,255},1);
    }
    auto bad=baseline.viewProjection;bad(0,0)=std::numeric_limits<float>::quiet_NaN();
    const auto submitted=fixture.presentation.submittedFrames;
    Check(!RenderD3D12Frame(fixture.presentation,localization,&fixture.triangle,nullptr,bad)
        && fixture.presentation.submittedFrames==submitted && !fixture.presentation.failed,
        "nonfinite matrix rejected without poisoning renderer");
}

void NoValidationProblems(ID3D12InfoQueue* info)
{
    if (!info) return;
    unsigned problems = 0;
    for (UINT64 index = 0; index < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++index)
    {
        SIZE_T size = 0;
        CheckHr(info->GetMessage(index, nullptr, &size), "query validation message size");
        std::vector<unsigned char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        CheckHr(info->GetMessage(index, message, &size), "read validation message");
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
        { ++problems; std::cerr << "GPU validation: " << message->pDescription << '\n'; }
    }
    std::cout << "InfoQueue Warning/Error/Corruption=" << problems << '\n';
    Check(problems == 0, "no GPU validation problems");
}

void GpuTests(const fs::path& shaderPath, const fs::path& output, const Localization& localization)
{
    Fixture fixture;
    Check(CreateEngineWindow(fixture.window, false), "create hidden GPU window");
    SetClientSize(fixture.window, 320, 240);
    D3D12Context emptyContext;
    Check(!InitializeD3D12Presentation(fixture.presentation, emptyContext, fixture.window.hwnd, localization),
        "reject missing device");
    Check(InitializeD3D12Context(fixture.graphics, localization), "initialize hardware device");
    const auto dredReport = output / "dred-probe.txt";
    Check(se::d3d12::DumpDred(fixture.graphics.device.Get(), dredReport), "DRED probe writes a report");
    Check(fs::exists(dredReport), "DRED probe report exists");
    ComPtr<ID3D12InfoQueue> info;
#if defined(_DEBUG)
    CheckHr(fixture.graphics.device.As(&info), "Debug InfoQueue required");
#endif
    Check(InitializeD3D12Presentation(fixture.presentation, fixture.graphics, fixture.window.hwnd, localization),
        "initialize presentation");
    Check(InitializeD3D12Triangle(fixture.triangle, fixture.graphics, shaderPath, localization), "initialize triangle");
    auto* originalPipeline = fixture.triangle.pipeline.Get();
    Check(!InitializeD3D12Triangle(fixture.triangle, fixture.graphics, shaderPath, localization)
        && fixture.triangle.pipeline.Get() == originalPipeline, "repeat initialization preserves pipeline");

    fs::path badShader = output / "bad.hlsl";
    { std::ofstream bad(badShader); bad << "float4 VSMain( : SV_Position { invalid }\n"; }
    D3D12Triangle failedTriangle;
    std::ostringstream diagnostics;
    auto* previous = std::cerr.rdbuf(diagnostics.rdbuf());
    bool badAccepted = false;
    try { badAccepted = InitializeD3D12Triangle(failedTriangle, fixture.graphics, badShader, localization); }
    catch (...) { std::cerr.rdbuf(previous); throw; }
    std::cerr.rdbuf(previous);
    std::cout << diagnostics.str();
    Check(!badAccepted && !failedTriangle.pipeline && !failedTriangle.vertices
        && diagnostics.str().find("bad.hlsl(") != std::string::npos, "Shader error includes file and line");
    Check(!InitializeD3D12Triangle(failedTriangle, fixture.graphics, output / "missing.hlsl", localization)
        && !failedTriangle.rootSignature, "missing Shader leaves empty output");

    auto& presentation = fixture.presentation;
    for (UINT frame = 0; frame < 32; ++frame)
    {
        Check(RenderD3D12Frame(presentation, localization, &fixture.triangle), "submit triangle frame");
    }
    Check(presentation.submittedFrames == 32 && presentation.signalCount == 32
        && presentation.frames[0].fenceValue == 31 && presentation.frames[1].fenceValue == 32
        && presentation.frameCursor == 0, "two frame contexts with monotonic submission markers");
    for (const auto flushes : presentation.flushCounts) Check(flushes == 0, "ordinary frames do not flush GPU");
    const auto firstImage = ReadBack(fixture, &fixture.triangle, localization);
    TrianglePixels(firstImage);
    SaveImage(firstImage, output / "triangle.ppm");
    CameraGpuTests(fixture,output,localization);

    auto* firstBuffer = presentation.backBuffers[0].Get();
    const auto noOpFence = presentation.nextFenceValue;
    Check(ResizeD3D12Presentation(presentation, 320, 240, localization)
        && ResizeD3D12Presentation(presentation, 0, 240, localization)
        && ResizeD3D12Presentation(presentation, 320, 0, localization)
        && presentation.backBuffers[0].Get() == firstBuffer && presentation.nextFenceValue == noOpFence,
        "same/zero sizes preserve buffers and do not submit");

    const UINT sizes[][2]{ { 513, 257 }, { 640, 360 }, { 300, 520 }, { 320, 240 } };
    for (unsigned cycle = 0; cycle < 3; ++cycle)
        for (const auto& size : sizes)
        {
            SetClientSize(fixture.window, size[0], size[1]);
            Check(ResizeD3D12Presentation(presentation, size[0], size[1], localization), "resize with in-flight work");
            DXGI_SWAP_CHAIN_DESC1 actual{};
            CheckHr(presentation.swapChain->GetDesc1(&actual), "query resized swap chain");
            Check(actual.Width == size[0] && actual.Height == size[1], "actual swap-chain dimensions");
            for (unsigned frame = 0; frame < 8; ++frame)
            {
                Check(RenderD3D12Frame(presentation, localization, &fixture.triangle), "draw resized triangle");
            }
            TrianglePixels(ReadBack(fixture, &fixture.triangle, localization));
        }

    const auto variantPath = output / "variant.hlsl";
    { std::ofstream variant(variantPath);
      // T23 固定绑定：变体保留相机 b0 和顶点语义，像素仍为原来的固定颜色。
      variant << "cbuffer CameraConstants:register(b0) { column_major float4x4 clipFromWorld; };\n"
          "struct V { float3 p:POSITION; float4 c:COLOR; };\n"
          "struct P { float4 p:SV_POSITION; float4 c:COLOR; };\n"
          "P VSMain(V v) { P o; o.p=mul(clipFromWorld,float4(v.p,1)); o.c=v.c; return o; }\n"
          "float4 PSMain(P p):SV_TARGET { return float4(0.25,0.75,0.5,1); }\n"; }
    auto& variant = fixture.variant;
    Check(InitializeD3D12Triangle(variant, fixture.graphics, variantPath, localization), "compile changed color Shader");
    const auto variantImage = ReadBack(fixture, &variant, localization);
    Pixel(variantImage, variantImage.width / 2, variantImage.height / 2, { 64, 191, 128, 255 }, 1);
    SaveImage(variantImage, output / "variant.ppm");
    variant = {};

    // 再提交一次后立刻关闭，不能依靠先前的读回等待来保证这次退出安全。
    Check(RenderD3D12Frame(presentation, localization, &fixture.triangle), "submit immediately before shutdown");
    const auto shutdownFlushes = presentation.flushCounts[static_cast<size_t>(D3D12WaitReason::Shutdown)];
    ReleaseD3D12Presentation(presentation);
    fixture.triangle = {};
    Check(!presentation.queue && !presentation.swapChain && !presentation.frames[0].allocator
        && !presentation.frames[1].allocator && !presentation.gpuWorkPending && IsWindow(fixture.window.hwnd),
        "GPU shutdown precedes HWND destruction");
    Check(presentation.flushCounts[static_cast<size_t>(D3D12WaitReason::Shutdown)] == shutdownFlushes + 1,
        "shutdown drains outstanding queue work");
    ReleaseD3D12Presentation(presentation);
    std::cout << "submitted=" << presentation.submittedFrames << " signals=" << presentation.signalCount;
    const char* reasons[]{ "FrameReuse", "Resize", "Shutdown", "Readback" };
    for (size_t reason = 0; reason < std::size(reasons); ++reason)
        std::cout << ' ' << reasons[reason] << "Waits=" << presentation.waitCounts[reason]
            << " Flushes=" << presentation.flushCounts[reason];
    std::cout << '\n';

    Check(InitializeD3D12Presentation(presentation, fixture.graphics, fixture.window.hwnd, localization),
        "reinitialize presentation on the same HWND");
    Check(RenderD3D12ClearFrame(presentation, localization), "existing clear-frame entry");
    Pixel(ReadBack(fixture, nullptr, localization), 160, 120, { 20, 46, 82, 255 }, 1);
    ReleaseD3D12Presentation(presentation);
    NoValidationProblems(info.Get());
}

void RendererTests(const Localization& localization)
{
    WindowState window;
    Check(CreateEngineWindow(window, false), "create renderer integration window");
    struct WindowLifetime { WindowState& window; ~WindowLifetime() {
        window.postQuitOnDestroy = false; DestroyEngineWindow(window); } } lifetime{ window };
    SandboxRenderer renderer(true);
    Check(renderer.Initialize({ window.hwnd, window.width, window.height }, localization), "renderer Shader resource path");
    Check(renderer.Render({ { window.hwnd, 0, window.height } }, localization) == RenderResult::Occluded,
        "renderer skips zero-width frame");
    Check(renderer.Render({ { window.hwnd, window.width, 0 } }, localization) == RenderResult::Occluded,
        "renderer skips zero-height frame");
    SetClientSize(window, 410, 310);
    Check(renderer.Render({ { window.hwnd, window.width, window.height } }, localization) != RenderResult::Failed,
        "renderer routes new surface size");
    se::FrameData data;
    data.cameras.push_back({1,se::Translation({0,0,2}),{se::Pi/2,0.1f,100,true},true});
    RenderFrame cameraFrame;
    cameraFrame.surface={window.hwnd,window.width,window.height};cameraFrame.data=&data;cameraFrame.updateIndex=50;
    Check(renderer.Render(cameraFrame,localization)!=RenderResult::Failed && renderer.QueryEditorView().hasCamera,
        "renderer consumes and publishes a camera snapshot");
    se::CameraSnapshot snapshot;se::CameraError error;
    Check(se::BuildCameraSnapshot(data.cameras[0],{0,0,float(window.width),float(window.height)},50,snapshot,error)
        && renderer.QueryEditorView().viewProjection==snapshot.viewProjection.values,"single CPU/render matrix convention");
    data.cameras[0].rigidHierarchy=false;
    Check(renderer.Render(cameraFrame,localization)!=RenderResult::Failed && !renderer.QueryEditorView().hasCamera,
        "invalid camera uses no-camera background");
    data.cameraBinding=99;
    Check(renderer.Render(cameraFrame,localization)!=RenderResult::Failed && !renderer.QueryEditorView().hasCamera,
        "invalid camera binding has no hidden identity fallback");
    renderer.Shutdown();
    renderer.Shutdown();
    Check(IsWindow(window.hwnd) != FALSE, "renderer borrows HWND");
}

void CameraHostSmoke()
{
    se::CameraDemo demo;
    SandboxRenderer renderer(true);
    auto services=demo.Services();
    const auto opened=services.windowOpened;
    services.windowOpened=[opened](HWND window) {
        if(!opened(window))return false;
        SendMessageW(window,WM_SETFOCUS,0,0);SendMessageW(window,WM_KEYDOWN,'W',0);
        return true;
    };
    services.maximumFrames=8;
    Check(RunSandboxApp("en-US",renderer,false,&services)==0 && demo.State().position.z<2,
        "actual sample, World, input, camera and D3D12 host smoke");
}
}

int main(int argc, char* argv[])
{
    try
    {
        Check(argc == 3 || argc == 4, "arguments: ShaderPath OutputDirectory [locale]");
        const fs::path output = fs::absolute(argv[2]);
        fs::create_directories(output);
        Localization localization;
        std::string error;
        Check(localization.Load(GetExecutableDirectory() / "Resources/Localization"
            / ((argc == 4 ? std::string(argv[3]) : "en-US") + ".txt"), error), "load language table");
        GpuTests(fs::absolute(argv[1]), output, localization);
        RendererTests(localization);
        CameraHostSmoke();
        std::cout << "D3D12Tests passed (GPU pixels, Shader diagnostics, frames, resize, shutdown)\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << "D3D12Tests failed: " << error.what() << '\n'; return 1; }
}
