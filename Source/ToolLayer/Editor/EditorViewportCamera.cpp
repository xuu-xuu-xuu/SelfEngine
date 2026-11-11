#include "EditorViewportCamera.h"
#include "ResourceLayer/Assets/StaticModelPreparation.h"
#include "ResourceLayer/Assets/Primitives.h"
#include <limits>
namespace se
{
std::optional<CameraSnapshot> EditorViewportCamera::Snapshot(CameraViewport viewport,std::uint64_t frame) const
{
    CameraSnapshot snapshot;CameraError error;
    if(!BuildCameraSnapshot({UINT64_MAX,FreeCameraTransform(state).Matrix(),{},true},viewport,frame,snapshot,error))return {};
    return snapshot;
}
StableEntityId PickEditorBounds(const World& world,AssetStore& assets,const CameraSnapshot& camera,float x,float y)
{
    if(!IsFinite(x)||!IsFinite(y)||x<0||x>1||y<0||y>1)return 0;
    const auto ray=ViewportPointToRay(camera,{camera.viewport.x+x*camera.viewport.width,camera.viewport.y+y*camera.viewport.height});if(!ray)return 0;
    StableEntityId hit=0;double nearest=std::numeric_limits<double>::infinity();
    for(const auto entity:world.Entities())
    {
        const auto* mesh=world.GetMesh(entity);if(!mesh)continue;
        bool enabled=true;for(auto p=entity;p;){const auto* r=world.Get(p);if(!r||!r->enabled){enabled=false;break;}p=r->parent;}if(!enabled)continue;
        Aabb local{{-0.5f,-0.5f,-0.5f},{0.5f,0.5f,0.5f}};
        if(!IsBuiltinMesh(mesh->meshAsset))
        {
            std::vector<AssetDiagnostic> diagnostics;const auto model=assets.LoadModel(mesh->meshAsset,diagnostics);StaticModelLayout layout;
            if(!model||!BuildStaticModelLayout(*model,layout,diagnostics))continue;local={layout.minimum,layout.maximum};
        }
        const auto box=TransformBounds(local,world.GetWorldMatrix(entity));if(!box)continue;
        double low=0,high=nearest;const double origin[]{ray->origin.x,ray->origin.y,ray->origin.z},direction[]{ray->direction.x,ray->direction.y,ray->direction.z};
        const double minimum[]{box->minimum.x,box->minimum.y,box->minimum.z},maximum[]{box->maximum.x,box->maximum.y,box->maximum.z};
        bool intersects=true;
        for(int axis=0;axis<3;++axis)
        {
            if(direction[axis]==0){if(origin[axis]<minimum[axis]||origin[axis]>maximum[axis])intersects=false;continue;}
            auto a=(minimum[axis]-origin[axis])/direction[axis],b=(maximum[axis]-origin[axis])/direction[axis];if(a>b)std::swap(a,b);
            low=std::max(low,a);high=std::min(high,b);if(low>high){intersects=false;break;}
        }
        const auto id=world.Get(entity)->stableId;
        if(intersects&&high>=0&&(low<nearest||(low==nearest&&(!hit||id<hit)))){nearest=low;hit=id;}
    }
    return hit;
}
}
