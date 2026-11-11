#include "SceneDefaults.h"
#include "FunctionLayer/Runtime/World.h"
#include "ResourceLayer/Assets/Primitives.h"
#include <algorithm>
#include <cmath>

namespace se
{
Quat DirectionToRotation(Vec3 directionToLight)
{
    const auto target=Normalize(directionToLight);
    if(LengthSquared(target)<1e-12f)return {};
    constexpr Vec3 source{0,0,1};
    const float cosine=Dot(source,target);
    if(cosine>0.999999f)return {};
    if(cosine<-0.999999f)return AxisAngle({0,1,0},Pi);
    const auto axis=Normalize(Cross(source,target));
    if(LengthSquared(axis)<1e-12f)return AxisAngle({0,1,0},Pi);
    return AxisAngle(axis,std::acos(std::clamp(cosine,-1.0f,1.0f)));
}
bool CreateDefaultScene(World& world,std::string& errorKey)
{
    errorKey.clear();
    if(world.Size()!=0){errorKey="Editor.SceneDefaultNotEmpty";return false;}
    const auto fail=[&](const char* key){errorKey=key;return false;};
    const auto camera=world.Create("Main Camera",static_cast<StableEntityId>(DefaultSceneEntity::Camera));
    const auto light=world.Create("Directional Light",static_cast<StableEntityId>(DefaultSceneEntity::Light));
    const auto ground=world.Create("Ground",static_cast<StableEntityId>(DefaultSceneEntity::Ground));
    const auto cube=world.Create("Cube",static_cast<StableEntityId>(DefaultSceneEntity::Cube));
    if(!camera||!light||!ground||!cube)return fail("Editor.SceneDefaultFailed");
    Transform cameraTransform;cameraTransform.position={0,1.6f,4.5f};cameraTransform.rotation=AxisAngle({1,0,0},-0.15f);
    if(!world.SetTransform(camera,cameraTransform)||!world.SetCamera(camera,{Pi/3,0.1f,1000,true}))return fail("Editor.SceneDefaultFailed");
    Transform lightTransform;lightTransform.rotation=DirectionToRotation({0.4f,0.7f,-0.55f});
    if(!world.SetTransform(light,lightTransform)||!world.SetLight(light,{{1,0.96f,0.9f},1.4f,LightType::Directional,10}))return fail("Editor.SceneDefaultFailed");
    Transform groundTransform;groundTransform.position={0,-0.5f,0};groundTransform.scale={10,1,10};
    if(!world.SetTransform(ground,groundTransform)||!world.SetMesh(ground,{BuiltinCubeAssetId,{},false,true}))return fail("Editor.SceneDefaultFailed");
    Transform cubeTransform;cubeTransform.position={0,0.5f,0};
    if(!world.SetTransform(cube,cubeTransform)||!world.SetMesh(cube,{BuiltinCubeAssetId,{},true,true}))return fail("Editor.SceneDefaultFailed");
    return true;
}
}
