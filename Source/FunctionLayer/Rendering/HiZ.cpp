#include "HiZ.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace se
{
bool HiZBuffer::Build(std::uint32_t width,std::uint32_t height,std::span<const float> depth)
{
    Clear();
    if(width==0||height==0||depth.size()<static_cast<std::size_t>(width)*height)return false;
    HiZMipLevel base;base.width=width;base.height=height;
    base.depth.assign(depth.begin(),depth.begin()+static_cast<std::size_t>(width)*height);
    for(const auto value:base.depth)if(!std::isfinite(value)||value<0||value>1)return false;
    mips_.push_back(std::move(base));
    width_=width;height_=height;
    while(mips_.back().width>1||mips_.back().height>1)
    {
        const auto& source=mips_.back();
        HiZMipLevel next;
        // G06.03：采用 D3D12 mip 链的向下取整尺寸（3→1 而不是 3→2），
        // 使 GPU HZB 与 CPU 参考逐位一致；被截掉的边缘条带由遮挡测试保守放弃。
        next.width=(std::max)(1u,source.width/2);next.height=(std::max)(1u,source.height/2);
        next.depth.resize(static_cast<std::size_t>(next.width)*next.height,0.0f);
        for(std::uint32_t y=0;y<next.height;++y)for(std::uint32_t x=0;x<next.width;++x)
        {
            float maximum=0;
            for(std::uint32_t dy=0;dy<2;++dy)for(std::uint32_t dx=0;dx<2;++dx)
            {
                const auto sx=x*2+dx,sy=y*2+dy;
                if(sx<source.width&&sy<source.height)
                    maximum=(std::max)(maximum,source.depth[static_cast<std::size_t>(sy)*source.width+sx]);
            }
            next.depth[static_cast<std::size_t>(y)*next.width+x]=maximum;
        }
        mips_.push_back(std::move(next));
    }
    return true;
}
const HiZMipLevel& HiZBuffer::Mip(std::uint32_t level) const
{
    return mips_[level<mips_.size()?level:mips_.size()-1];
}
float HiZBuffer::MaxDepth(std::uint32_t mip,std::uint32_t x,std::uint32_t y) const
{
    if(!Ready())return 1.0f;
    const auto& level=Mip(mip);
    if(x>=level.width||y>=level.height)return 1.0f;
    return level.depth[static_cast<std::size_t>(y)*level.width+x];
}
void HiZBuffer::Clear() noexcept
{
    mips_.clear();width_=height_=0;
}
bool ProjectScreenRect(const Aabb& worldBox,const Mat4& clipFromWorld,std::uint32_t width,std::uint32_t height,
    ScreenRect& output)
{
    if(!IsValid(worldBox)||!IsFinite(clipFromWorld)||width==0||height==0)return false;
    float minimumX=std::numeric_limits<float>::max(),maximumX=-minimumX;
    float minimumY=minimumX,maximumY=-minimumX,minimumDepth=minimumX;
    for(int corner=0;corner<8;++corner)
    {
        const Vec3 point{(corner&1)?worldBox.maximum.x:worldBox.minimum.x,
            (corner&2)?worldBox.maximum.y:worldBox.minimum.y,(corner&4)?worldBox.maximum.z:worldBox.minimum.z};
        const Vec4 clip=clipFromWorld*Vec4{point.x,point.y,point.z,1};
        if(!(clip.w>1e-6f))return false;
        const float x=clip.x/clip.w,y=clip.y/clip.w,z=clip.z/clip.w;
        if(!std::isfinite(x)||!std::isfinite(y)||!std::isfinite(z))return false;
        if(z<-1e-5f||z>1.0f+1e-5f)return false;
        minimumX=(std::min)(minimumX,x);maximumX=(std::max)(maximumX,x);
        minimumY=(std::min)(minimumY,y);maximumY=(std::max)(maximumY,y);
        minimumDepth=(std::min)(minimumDepth,z);
    }
    ScreenRect rect;
    rect.left=(minimumX*0.5f+0.5f)*float(width);
    rect.right=(maximumX*0.5f+0.5f)*float(width);
    rect.top=(0.5f-maximumY*0.5f)*float(height);
    rect.bottom=(0.5f-minimumY*0.5f)*float(height);
    rect.left=std::clamp(rect.left,0.0f,float(width));
    rect.right=std::clamp(rect.right,0.0f,float(width));
    rect.top=std::clamp(rect.top,0.0f,float(height));
    rect.bottom=std::clamp(rect.bottom,0.0f,float(height));
    if(!rect.Valid())return false;
    output=rect;return true;
}
HiZTestResult TestOcclusion(const HiZBuffer& hzb,const Aabb& worldBox,const Mat4& clipFromWorld,
    std::uint32_t width,std::uint32_t height)
{
    HiZTestResult result;
    if(!hzb.Ready()||hzb.Width()!=width||hzb.Height()!=height)return result;
    ScreenRect rect;
    if(!ProjectScreenRect(worldBox,clipFromWorld,width,height,rect))return result;
    float objectDepth=1.0f;
    for(int corner=0;corner<8;++corner)
    {
        const Vec3 point{(corner&1)?worldBox.maximum.x:worldBox.minimum.x,
            (corner&2)?worldBox.maximum.y:worldBox.minimum.y,(corner&4)?worldBox.maximum.z:worldBox.minimum.z};
        const Vec4 clip=clipFromWorld*Vec4{point.x,point.y,point.z,1};
        if(!(clip.w>1e-6f))return result;
        objectDepth=(std::min)(objectDepth,clip.z/clip.w);
    }
    std::uint32_t mip=0;
    float scale=1.0f;
    while(mip+1<hzb.MipCount()&&((rect.right-rect.left)/scale>2.0f||(rect.bottom-rect.top)/scale>2.0f))
    {
        ++mip;scale*=2.0f;
    }
    const auto& level=hzb.Mip(mip);
    // 向下取整的 mip 只覆盖 scale 的整数倍区域；屏幕 AABB 触及未覆盖的右/下边缘时放弃遮挡。
    if(rect.right>float(level.width)*scale+1e-4f||rect.bottom>float(level.height)*scale+1e-4f)
    {
        result.mip=mip;result.maxDepth=1;result.objectDepth=objectDepth;return result;
    }
    const auto x0=static_cast<std::uint32_t>(std::clamp(std::floor(rect.left/scale),0.0f,float(level.width-1)));
    const auto y0=static_cast<std::uint32_t>(std::clamp(std::floor(rect.top/scale),0.0f,float(level.height-1)));
    const auto x1=static_cast<std::uint32_t>(std::clamp(std::ceil(rect.right/scale)-1.0f,0.0f,float(level.width-1)));
    const auto y1=static_cast<std::uint32_t>(std::clamp(std::ceil(rect.bottom/scale)-1.0f,0.0f,float(level.height-1)));
    float maximum=0;
    for(std::uint32_t y=y0;y<=y1;++y)for(std::uint32_t x=x0;x<=x1;++x)
        maximum=(std::max)(maximum,level.depth[static_cast<std::size_t>(y)*level.width+x]);
    result.mip=mip;result.objectDepth=objectDepth;result.maxDepth=maximum;
    result.occluded=objectDepth>maximum+1e-6f;
    return result;
}
HiZHistory::HiZHistory(float teleportMeters,float maximumFovRatio,float maximumTurnDegrees)
    :teleportMeters_(teleportMeters>0?teleportMeters:25.0f),
     maximumFovRatio_(maximumFovRatio>0?maximumFovRatio:0.10f),
     maximumTurnDegrees_(maximumTurnDegrees>0?maximumTurnDegrees:60.0f)
{
}
HiZInvalidation HiZHistory::Update(const HiZFrameState& state)
{
    reason_=HiZInvalidation::None;usable_=true;
    if(!initialized_)
    {
        previous_=state;initialized_=true;
        reason_=HiZInvalidation::FirstFrame;usable_=false;
        return reason_;
    }
    if(!IsFinite(state.viewProjection)||!IsFinite(state.cameraPosition)||!IsFinite(state.cameraForward)
        ||!std::isfinite(state.verticalFov)||state.verticalFov<=0||state.width==0||state.height==0)
    {
        reason_=HiZInvalidation::CameraCut;usable_=false;
    }
    else if(state.width!=previous_.width||state.height!=previous_.height)reason_=HiZInvalidation::Resize;
    else if(std::abs(state.verticalFov-previous_.verticalFov)
        >previous_.verticalFov*maximumFovRatio_)reason_=HiZInvalidation::FieldOfView;
    else if(Length(state.cameraPosition-previous_.cameraPosition)>teleportMeters_)reason_=HiZInvalidation::CameraCut;
    else
    {
        const auto current=Normalize(state.cameraForward),last=Normalize(previous_.cameraForward);
        if(Length(current)>0.5f&&Length(last)>0.5f)
        {
            const auto cosine=std::clamp(Dot(current,last),-1.0f,1.0f);
            const auto degrees=std::acos(cosine)*180.0f/Pi;
            if(degrees>maximumTurnDegrees_)reason_=HiZInvalidation::CameraCut;
        }
    }
    usable_=reason_==HiZInvalidation::None;
    previous_=state;
    return reason_;
}
bool BuildHiZPreview(const HiZBuffer& hzb,HiZPreview& output)
{
    if(!hzb.Ready()){output={};return false;}
    HiZPreview candidate;
    candidate.width=hzb.Width();candidate.height=hzb.Height();candidate.levels=hzb.MipCount();
    candidate.levelInfo.reserve(candidate.levels);
    for(std::uint32_t level=0;level<candidate.levels;++level)
    {
        const auto& mip=hzb.Mip(level);
        HiZPreviewLevel info;info.width=mip.width;info.height=mip.height;
        float minimum=1,maximum=0;
        for(const auto depth:mip.depth)
        {
            if(!std::isfinite(depth))continue;
            minimum=std::min(minimum,depth);maximum=std::max(maximum,depth);
        }
        info.minimumDepth=minimum;info.maximumDepth=maximum;
        candidate.levelInfo.push_back(info);
    }
    const auto& mip0=hzb.Mip(0);
    const auto& range=candidate.levelInfo.empty()?HiZPreviewLevel{}:candidate.levelInfo[0];
    const auto span=range.maximumDepth-range.minimumDepth;
    candidate.mip0.resize(std::size_t{mip0.width}*mip0.height*4);
    for(std::size_t index=0;index<mip0.depth.size();++index)
    {
        // 每级按自身深度范围归一化：近处为白、远处为黑。正交/窄深度场景若直接用原始
        // 深度会整幅接近白色，归一化后仍保留 min/max 元数据供逐级核对。
        const auto depth=std::isfinite(mip0.depth[index])?mip0.depth[index]:range.maximumDepth;
        const auto normalized=span>1e-6f?std::clamp((depth-range.minimumDepth)/span,0.0f,1.0f):0.5f;
        const auto value=static_cast<std::uint8_t>(std::lround((1.0f-normalized)*255.0f));
        candidate.mip0[index*4+0]=value;candidate.mip0[index*4+1]=value;candidate.mip0[index*4+2]=value;
        candidate.mip0[index*4+3]=255;
    }
    output=std::move(candidate);return true;
}
void HiZHistory::Reset() noexcept
{
    initialized_=false;previous_=HiZFrameState{};
    reason_=HiZInvalidation::FirstFrame;usable_=false;
}
}
