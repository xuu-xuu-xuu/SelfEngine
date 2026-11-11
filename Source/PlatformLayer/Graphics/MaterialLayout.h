#pragma once
#include "CoreLayer/Math/Projection.h"
#include <cstddef>
#include <cstdint>
namespace se::rhi
{
inline constexpr std::uint32_t MaterialLayoutVersion=1,MaterialConstantAlignment=256;
inline constexpr std::uint32_t PbrMaterialLayoutVersion=2,PbrConstantStride=512;
inline constexpr std::uint32_t ForwardLayoutVersion=3,MaximumPointLights=8;
inline constexpr std::uint32_t ShadowLayoutVersion=4;
inline constexpr std::uint32_t EnvironmentLayoutVersion=5;
inline constexpr std::uint32_t PostProcessLayoutVersion=6;
inline constexpr std::uint32_t InstancingLayoutVersion=7,InstanceGpuStride=240,MaximumFrameInstances=65536;
inline constexpr std::uint32_t GuiLayoutVersion=8;
inline constexpr std::uint32_t SkyLayoutVersion=9;
struct alignas(16) SkyConstants
{
    Mat4 skyFromClip;
    Vec4 zenithColor,horizonColor,groundColor,sunDirectionIntensity,sunColor,parameters;
};
static_assert(sizeof(SkyConstants)==160&&offsetof(SkyConstants,zenithColor)==64);
static_assert(offsetof(SkyConstants,horizonColor)==80&&offsetof(SkyConstants,groundColor)==96);
static_assert(offsetof(SkyConstants,sunDirectionIntensity)==112&&offsetof(SkyConstants,sunColor)==128);
static_assert(offsetof(SkyConstants,parameters)==144);
struct alignas(16) GuiConstants {Vec4 scaleTranslate;};
static_assert(sizeof(GuiConstants)==16);
struct alignas(16) InstanceGpuData
{
    Mat4 clipFromLocal,worldFromLocal,normalFromLocal;
    Vec4 baseColor,parameters,options;
};
static_assert(sizeof(InstanceGpuData)==InstanceGpuStride&&alignof(InstanceGpuData)==16);
static_assert(offsetof(InstanceGpuData,worldFromLocal)==64&&offsetof(InstanceGpuData,normalFromLocal)==128);
static_assert(offsetof(InstanceGpuData,baseColor)==192&&offsetof(InstanceGpuData,parameters)==208&&offsetof(InstanceGpuData,options)==224);
struct alignas(16) PostProcessConstants {Vec4 parameters,resolution,options;};
struct alignas(16) OverlayConstants {Vec4 rectangle,color,viewport;};
static_assert(sizeof(PostProcessConstants)==48&&sizeof(OverlayConstants)==48);
struct alignas(16) ForwardFrameConstants
{
    Vec4 directionAndEnabled,directionalRadiance,skyAndExposure,pointCount;
    std::array<Vec4,MaximumPointLights> pointPositionRadius,pointRadiance;
};
static_assert(sizeof(ForwardFrameConstants)==320&&offsetof(ForwardFrameConstants,pointPositionRadius)==64);
static_assert(offsetof(ForwardFrameConstants,pointRadiance)==192);
struct alignas(16) ShadowFrameConstants
{
    ForwardFrameConstants lights;
    Mat4 clipFromWorld;
    Vec4 parameters;
};
static_assert(sizeof(ShadowFrameConstants)==400&&offsetof(ShadowFrameConstants,clipFromWorld)==320);
static_assert(offsetof(ShadowFrameConstants,parameters)==384);
struct alignas(16) EnvironmentFrameConstants
{
    ShadowFrameConstants shadow;
    Vec4 environment;
};
struct alignas(16) EnvironmentSkyConstants {Mat4 skyFromClip;Vec4 environment;};
static_assert(sizeof(EnvironmentFrameConstants)==416&&offsetof(EnvironmentFrameConstants,environment)==400);
static_assert(sizeof(EnvironmentSkyConstants)==80);
struct alignas(16) MeshDrawConstants
{
    Mat4 clipFromLocal,normalFromLocal;
    Vec4 tint,options;
};
static_assert(sizeof(MeshDrawConstants)==160&&alignof(MeshDrawConstants)==16);
static_assert(offsetof(MeshDrawConstants,clipFromLocal)==0&&offsetof(MeshDrawConstants,normalFromLocal)==64);
static_assert(offsetof(MeshDrawConstants,tint)==128&&offsetof(MeshDrawConstants,options)==144);
static_assert(sizeof(Mat4)==64);
struct alignas(16) PbrDrawConstants
{
    Mat4 clipFromLocal,worldFromLocal,normalFromLocal;
    Vec4 baseColor,cameraPosition,directionToLight,radiance,parameters,options;
};
static_assert(sizeof(PbrDrawConstants)==288&&alignof(PbrDrawConstants)==16);
static_assert(offsetof(PbrDrawConstants,worldFromLocal)==64&&offsetof(PbrDrawConstants,normalFromLocal)==128);
static_assert(offsetof(PbrDrawConstants,baseColor)==192&&offsetof(PbrDrawConstants,cameraPosition)==208);
static_assert(offsetof(PbrDrawConstants,directionToLight)==224&&offsetof(PbrDrawConstants,radiance)==240);
static_assert(offsetof(PbrDrawConstants,parameters)==256&&offsetof(PbrDrawConstants,options)==272);

// G 系列预留的绑定布局版本。G00 只登记，不创建对应描述符布局；
// 新版本不得复用 1–9 的既有布局，也不得与 BasicLit 技术号混用。
inline constexpr std::uint32_t VertexCompressedLayoutVersion=20;
inline constexpr std::uint32_t ClusteredForwardLayoutVersion=21;
inline constexpr std::uint32_t CascadedShadowLayoutVersion=22;
inline constexpr std::uint32_t AtmosphereLayoutVersion=23;
inline constexpr std::uint32_t AmbientOcclusionLayoutVersion=24;
inline constexpr std::uint32_t VolumetricFogLayoutVersion=25;
inline constexpr std::uint32_t TemporalAALayoutVersion=26;
inline constexpr std::uint32_t ExposureColorLayoutVersion=27;
inline constexpr std::uint32_t MeshletLayoutVersion=28;
// G15：输出 LUT 的显示线性 strip。t0 场景、t1 阴影预览（仅 ShadowOutput）、t2 33³ 2D strip；
// 编辑器 BasicLit（ShadowOutput）与 Lighting 演示（FinalOutput）共用此布局，t1 在 FinalOutput 中不使用。
// G15.09：strip 可为 Rgba8（兼容）或 Rgba16Float（保留 >1 的显示线性值与精度）。
inline constexpr std::uint32_t OutputLutLayoutVersion=29;
// G06.10：Forward 实例化的组合布局。在 InstancingLayoutVersion 的实例 SRV（t7）
// 之外追加 ForwardConstants（b1），用于 HDR 前向场景；b0 仍提供相机位置等逐绘制常量。
// 与 7/3 号布局都不兼容（参数表为 b0/纹理表/b1/实例 SRV 四槽），因此单独占用 30。
inline constexpr std::uint32_t ForwardInstancingLayoutVersion=30;
// G15：ToneMap 的扩展绑定布局。t0 场景、t1 Bloom、t2 1×1 曝光、t3 33³ LUT；
// 与旧 PostProcessLayoutVersion 的 t0/t1 布局不兼容，因此单独占用 27 而不复用 6。
inline constexpr std::uint32_t ToneMapLayoutVersion=ExposureColorLayoutVersion;

struct LayoutVersionRecord
{
    std::uint32_t version;
    const char* system;
    const char* fields;
    std::uint32_t bytes; // 0 表示尚未冻结字节数
};

inline constexpr LayoutVersionRecord GSeriesLayoutVersions[] =
{
    {VertexCompressedLayoutVersion,"G03","position/normal/uv/tangent 压缩布局",0},
    {ClusteredForwardLayoutVersion,"G09","cluster 网格、灯光索引与范围",0},
    {CascadedShadowLayoutVersion,"G08","级联矩阵、分割与混合参数",0},
    {AtmosphereLayoutVersion,"G11","大气 LUT 与太阳参数",0},
    {AmbientOcclusionLayoutVersion,"G10","AO 半径、深度重建与升采样参数",0},
    {VolumetricFogLayoutVersion,"G11","雾密度、风与光线步进参数",0},
    {TemporalAALayoutVersion,"G14","抖动、历史与邻域夹取参数",0},
    {ExposureColorLayoutVersion,"G15","直方图、曝光与 LUT 参数",0},
    {MeshletLayoutVersion,"G07","cluster 描述与间接参数",0},
    {OutputLutLayoutVersion,"G15","输出 LUT strip（t0 场景/t1 阴影/t2 33³，Rgba8 或 Rgba16F）",0},
    {ForwardInstancingLayoutVersion,"G06","Forward 实例化（b0/纹理表/b1/实例 SRV）",0},
};
}
