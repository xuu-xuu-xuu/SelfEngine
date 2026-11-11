#include "FrameData.h"
namespace se
{
void BuildFrameData(const Environment& environment,const UiTree* ui,const PhysicsWorld* physics,FrameData& output)
{
    BuildFrameData(environment.GetWorld(),ui,physics,output);
    if(environment.TimeOfDay().Profile())output.timeOfDay=environment.Snapshot();
}
void BuildFrameData(const World& world,const UiTree* ui,const PhysicsWorld* physics,FrameData& output)
{
    FrameData next;
    for(auto entity:world.Entities())
    {
        const auto* record=world.Get(entity);bool enabled=true,rigidHierarchy=true;
        for(auto p=entity;p;){const auto* r=world.Get(p);if(!r||!r->enabled){enabled=false;break;}
            const auto* transform=world.GetTransform(p);
            if(!transform||!HasUnitScale(transform->scale))rigidHierarchy=false;
            p=r->parent;}
        if(!enabled)continue;
        const auto matrix=world.GetWorldMatrix(entity);
        const auto active=[&](ComponentTypeId type){const auto* component=world.GetComponent(world.FindComponent(entity,type));return component&&component->Active();};
        if(const auto* mesh=world.GetMesh(entity);mesh&&active(ComponentTypes::Mesh))next.meshes.push_back({record->stableId,matrix,*mesh});
        if(const auto* camera=world.GetCamera(entity);camera&&active(ComponentTypes::Camera))next.cameras.push_back({record->stableId,matrix,*camera,rigidHierarchy});
        if(const auto* light=world.GetLight(entity);light&&active(ComponentTypes::Light))next.lights.push_back({record->stableId,matrix,*light});
    }
    if(ui)next.ui=ui->Visuals();
    if(physics)next.debugLines=physics->DebugLines();
    output=std::move(next);
}
}
