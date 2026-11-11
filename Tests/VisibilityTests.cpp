#include "FunctionLayer/Rendering/Visibility.h"
#include "FunctionLayer/Application/FrameData.h"
#include "FunctionLayer/Rendering/SandboxRenderer.h"
#include "PlatformLayer/Windows/Window.h"
#include "PlatformLayer/Windows/ExecutablePath.h"
#include "ResourceLayer/Assets/AssetIO.h"
#include "ResourceLayer/Assets/Primitives.h"
#include "CoreLayer/Localization.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace se;
namespace
{
void Check(bool value,const char* message){if(!value)throw std::runtime_error(message);}
// 直接计算八角点的六个齐次不等式，不复用待测平面提取或正顶点算法。
bool ClipReject(const Mat4& matrix,const Aabb& box)
{
    bool outside[6]{true,true,true,true,true,true};
    for(int corner=0;corner<8;++corner)
    {
        const double point[]{corner&1?box.maximum.x:box.minimum.x,corner&2?box.maximum.y:box.minimum.y,corner&4?box.maximum.z:box.minimum.z,1};double clip[4]{};
        for(size_t row=0;row<4;++row)for(size_t column=0;column<4;++column)clip[row]+=matrix(row,column)*point[column];
        const double distances[]{clip[0]+clip[3],clip[3]-clip[0],clip[1]+clip[3],clip[3]-clip[1],clip[2],clip[3]-clip[2]};
        for(size_t plane=0;plane<6;++plane)outside[plane]&=distances[plane]<0;
    }
    return std::any_of(std::begin(outside),std::end(outside),[](bool value){return value;});
}
void Cpu()
{
    const auto perspective=PerspectiveRH(Pi/2,1,0.1f,100);Check(perspective.has_value(),"perspective");const auto frustum=BuildFrustum(*perspective);Check(frustum.has_value(),"extract frustum");
    for(Vec3 point:{Vec3{0,0,-1},Vec3{1,0,-1},Vec3{0,0,-0.1f},Vec3{0,0,-100}})Check(Intersects(*frustum,{point,point}),"inside or grazing point is retained");
    for(Vec3 point:{Vec3{0,0,1},Vec3{0,0,-0.01f},Vec3{0,0,-101},Vec3{3,0,-1},Vec3{0,3,-1}})Check(!Intersects(*frustum,{point,point}),"outside plane is rejected");
    Check(Intersects(*frustum,{{-1,-1,-1},{1,1,1}}),"camera inside AABB remains visible");
    std::uint32_t random=131313;const auto unit=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return float(random>>8)/16777216;};size_t rejected=0;
    for(int index=0;index<12000;++index)
    {
        Transform transform;transform.position={(unit()-0.5f)*120,(unit()-0.5f)*120,(unit()-0.5f)*160};transform.rotation=AxisAngle(Normalize(Vec3{unit(),unit(),unit()}),unit()*2*Pi);
        transform.scale={(index%2?-1:1)*(0.1f+unit()*5),0.1f+unit()*5,0.1f+unit()*5};const Aabb local{{-0.7f,-0.3f,-0.9f},{0.4f,0.8f,0.5f}};
        const auto matrix=transform.Matrix();const auto box=TransformBounds(local,matrix);Check(box.has_value(),"valid mirrored/nonuniform bounds");
        for(int corner=0;corner<8;++corner)
        {
            const double p[]{corner&1?local.maximum.x:local.minimum.x,corner&2?local.maximum.y:local.minimum.y,corner&4?local.maximum.z:local.minimum.z};
            const double lo[]{box->minimum.x,box->minimum.y,box->minimum.z},hi[]{box->maximum.x,box->maximum.y,box->maximum.z};
            for(size_t row=0;row<3;++row){double value=matrix(row,3);for(size_t col=0;col<3;++col)value+=matrix(row,col)*p[col];Check(value>=lo[row]&&value<=hi[row],"outward rounded world box contains transformed corners");}
        }
        const auto projection=index%2?*perspective:*OrthographicRH(40,30,0.1f,100);const auto view=Translation({(unit()-0.5f)*10,unit()*5,-unit()*3})*Rotation(AxisAngle({0,1,0},unit()*2*Pi));const auto clip=projection*view;
        const auto planes=BuildFrustum(clip);Check(planes.has_value(),"random camera planes");if(!Intersects(*planes,*box)){++rejected;Check(ClipReject(clip,*box),"fast reject must agree with independent homogeneous corner oracle");}
    }
    Check(rejected>7000,"oracle exercises many rejected boxes");Mat4 invalid;invalid(3,0)=0.001f;Check(!TransformBounds({{-1,-1,-1},{1,1,1}},invalid),"projective matrix fails open");
    invalid=Scaling({1,0,1});Check(!TransformBounds({{},{}},invalid),"singular matrix fails open");invalid=Mat4{};invalid(0,0)=std::numeric_limits<float>::quiet_NaN();Check(!BuildFrustum(invalid),"nonfinite camera rejected");
    Check(Intersects(*frustum,{{1,1,1},{-1,-1,-1}}),"invalid bounds retained");
    WorldBoundsCache cache;auto model=std::make_shared<StaticRenderModel>();model->layout.minimum={-1,-1,-1};model->layout.maximum={1,1,1};cache.BeginFrame();Check(cache.Resolve(1,Mat4{},model).has_value()&&cache.Updates()==1,"initial bounds build");
    cache.Resolve(1,Mat4{},model);Check(cache.Updates()==1,"same frame hit");cache.EndFrame();cache.BeginFrame();cache.Resolve(1,Mat4{},model);Check(cache.Updates()==0,"static world does not rebuild bounds");
    cache.Resolve(1,Translation({2,0,0}),model);Check(cache.Updates()==1,"transform change invalidates");auto revision=std::make_shared<StaticRenderModel>(*model);cache.Resolve(1,Translation({2,0,0}),revision);Check(cache.Updates()==2,"model revision invalidates");cache.EndFrame();
    cache.BeginFrame();cache.EndFrame();Check(cache.Size()==0,"deleted entities pruned");cache.BeginFrame();cache.Resolve(2,Mat4{},model);std::weak_ptr<const StaticRenderModel> weak=model;model.reset();Check(weak.expired(),"bounds cache never extends model lifetime");cache.EndFrame();Check(cache.Size()==0,"expired revision pruned");
}
void Gpu(const Localization& text,const std::filesystem::path& output)
{
    WindowState window;SandboxRenderer renderer(SandboxScene::Shadows);
    struct Cleanup{WindowState& window;SandboxRenderer& renderer;~Cleanup(){renderer.Shutdown();window.postQuitOnDestroy=false;DestroyEngineWindow(window);}} cleanup{window,renderer};
    Check(CreateEngineWindow(window,false)&&SetEngineWindowClientSize(window,320,240),"visibility real window");int exit=0;Check(PumpWindowMessages(exit),"visibility window messages");
    Check(renderer.Initialize({window.hwnd,320,240},text),"production shadow renderer");FrameData data;
    const FreeCameraState initial{{0,3,9},0,-0.3f};data.cameras.push_back({1,FreeCameraTransform(initial).Matrix(),{Pi/3,0.1f,100,true},true});
    data.lights.push_back({2,Rotation(AxisAngle({0,0,1},-Pi/4)*AxisAngle({1,0,0},-Pi/2)),{{1,1,1},4}});
    data.shadow.center={2,2,0};data.shadow.halfExtent=12;data.shadow.lightDistance=24;data.shadow.resolution=512;
    data.meshes.push_back({10,Translation({0,-1,0})*Scaling({16,0.2f,16}),{BuiltinCubeAssetId,{},false,true}});
    data.meshes.push_back({11,Scaling({1.5f,1.5f,1.5f}),{BuiltinSphereAssetId,{}}});
    data.meshes.push_back({12,Translation({7,7,0})*Scaling({2,2,2}),{BuiltinCubeAssetId,{}}});
    std::uint64_t frame=0;
    const auto capture=[&](const std::string& name,bool culling){data.frustumCulling=culling;const auto path=output/(name+".ppm");++frame;renderer.SetCaptureOutput(path,frame);
        const RenderFrame input{{window.hwnd,320,240},1.0/60,1.0/60,double(frame)/60,frame,&data};Check(renderer.Render(input,text)!=RenderResult::Failed&&renderer.CaptureSucceeded(),"production visibility capture");
        std::vector<std::uint8_t> bytes;Check(asset_io::Read(path,bytes),"read visibility GPU image");return bytes;};
    const auto off=capture("shadow-off",false),on=capture("shadow-on",true);Check(off==on,"outside caster retained: full GPU image equals culling disabled");
    auto counts=renderer.DrawStatistics();Check(counts.candidates==3&&counts.culled==1&&counts.visible==2&&counts.draws==2,"camera rejects caster opaque only");
    data.meshes[2].mesh.castsShadow=false;const auto noCaster=capture("shadow-no-caster",true);Check(noCaster!=on,"invisible caster actually changes visible receiver");data.meshes[2].mesh.castsShadow=true;
    data.meshes.push_back({13,Mat4{},{BuiltinCubeAssetId,{}}});data.meshes.push_back({99,Translation({60,0,0}),{BuiltinCubeAssetId,{}}});
    for(int step=0;step<10;++step)
    {
        FreeCameraState pose=initial;if(step>=6){pose.position={0,0,0};pose.pitch=0;}
        data.cameras[0].world=FreeCameraTransform(pose).Matrix();
        const float x=step<6?-6.0f+float(step)*2.0f:0;const float z=step<6?0:-0.03f-float(step-6)*0.08f;
        data.meshes[3].world=Translation({x,step<6?1.0f:0,z})*Rotation(AxisAngle({0,1,0},float(step)*0.4f))*Scaling({step%2?-2.0f:0.3f,0.2f+float(step)*0.25f,0.7f});
        const auto before=capture("moving-"+std::to_string(step)+"-off",false),after=capture("moving-"+std::to_string(step)+"-on",true);
        Check(before==after,"moving, boundary, mirrored/nonuniform scale and near-camera images remain identical");
        counts=renderer.DrawStatistics();Check(counts.candidates==counts.culled+counts.visible&&counts.culled>0,"candidate accounting");
    }
    data.meshes={{100,Translation({1000,1000,-1000}),{BuiltinCubeAssetId,{}}}};
    data.cameras[0].world=FreeCameraTransform(initial).Matrix();
    data.lights.clear();data.shadow.enabled=false;
    const auto emptyOff=capture("empty-off",false),emptyOn=capture("empty-on",true);
    Check(emptyOff.size()==emptyOn.size()&&emptyOff.size()>1000,"empty opaque list produces a complete background image");
    counts=renderer.DrawStatistics();Check(counts.candidates==1&&counts.visible==0&&counts.draws==0&&counts.culled==1,"all candidates outside frustum");
    data.frustumCulling=true;renderer.SetCaptureOutput({},0);const auto baseline=renderer.Statistics();
    for(int index=0;index<16;++index){++frame;Check(renderer.Render({{window.hwnd,320,240},1.0/60,1.0/60,double(frame)/60,frame,&data},text)!=RenderResult::Failed,"ordinary culled frame");Check(renderer.DrawStatistics().boundsUpdates==0,"unchanged world boxes reused");}
    const auto final=renderer.Statistics();Check(final.globalWaits==baseline.globalWaits&&final.memoryResources==baseline.memoryResources&&final.pipelineCreations==baseline.pipelineCreations,"ordinary culling has no GPU allocation or global wait");
    Check(final.debugWarnings==0&&final.debugErrors==0,"visibility GPU diagnostics are clean");
}
}
int wmain(int argc,wchar_t** argv)
{
    try{Check(argc==2,"supply visibility output");const auto output=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(output);Localization text;std::string error;Check(text.Load(GetExecutableDirectory()/"Resources/Localization/en-US.txt",error),"visibility localization");
        Cpu();Gpu(text,output);std::cout<<"VisibilityTests passed (12000 independent clip/corner cases, conservative cache, exact GPU toggles, outside caster and near/moving/scaled objects).\n";return 0;}
    catch(const std::exception& error){std::cerr<<"VisibilityTests: "<<error.what()<<'\n';return 1;}
}
