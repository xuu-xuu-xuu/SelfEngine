#include "ToolLayer/Editor/EditorPanelRegistry.h"
#include "ToolLayer/Editor/SceneDefaults.h"
#include "FunctionLayer/Rendering/EditorViewportTarget.h"
#include "PlatformLayer/Graphics/Rhi.h"
#include <cmath>
#include <array>
#include <iostream>
#include <set>
#include <string>

#define CHECK(condition) do { if(!(condition)){std::cerr<<"FAIL line "<<__LINE__<<": "<<#condition<<'\n';return 1;} } while(false)
static bool Near(const se::Vec3& a,const se::Vec3& b){return se::LengthSquared(a-b)<1e-8f;}

int main()
{
    using namespace se;
    // 注册表：每一项都必须能被稳定键和枚举双向找到，窗口 ID 唯一。
    std::set<std::string> ids,keys;
    CHECK(EditorPanels().size()==EditorPanelCount);
    std::size_t visible=0;
    for(std::size_t index=0;index<EditorPanels().size();++index)
    {
        const auto& panel=EditorPanels()[index];
        CHECK(static_cast<std::size_t>(panel.id)==index);
        CHECK(ids.insert(panel.windowId).second);
        CHECK(keys.insert(panel.stableKey).second);
        CHECK(std::string(panel.windowId).rfind("###EditorUi.",0)==0);
        const auto* byEnum=FindEditorPanel(panel.id);
        const auto* byKey=FindEditorPanel(std::string_view(panel.stableKey));
        CHECK(byEnum==&panel&&byKey==&panel);
        CHECK(!EditorPanelWindowName(panel,"标题").empty());
        visible+=panel.defaultVisible?1u:0u;
    }
    CHECK(visible==5);
    CHECK(FindEditorPanel(EditorPanel::Viewport)->defaultVisible);
    CHECK(!FindEditorPanel(EditorPanel::MaterialEditor)->defaultVisible);
    CHECK(FindEditorPanel("missing")==nullptr);

    // 方向光本地 +Z 指向太阳；退化输入不产生 NaN。
    CHECK(Near(Rotate(DirectionToRotation({0,0,1}),{0,0,1}),{0,0,1}));
    for(const Vec3 direction:std::array<Vec3,4>{{{0.4f,0.7f,0.55f},{-1,0,0},{0,-1,0},{0,0,-1}}})
    {
        const auto q=DirectionToRotation(direction);
        CHECK(std::isfinite(q.x)&&std::isfinite(q.y)&&std::isfinite(q.z)&&std::isfinite(q.w));
        CHECK(Near(Rotate(q,{0,0,1}),Normalize(direction)));
    }
    CHECK(DirectionToRotation({0,0,0}).w==1);

    // R00 视口骨架不创建资源，失败保留上一有效目标。
    NullEditorViewportTarget target;
    std::string error;
    CHECK(!target.Resize(320,180,error));
    CHECK(error=="Rendering.EditorTargetUnavailable");
    CHECK(!target.Targets().hdrColor&&!target.LdrColor()&&target.Targets().width==0);
    std::cout<<"EditorContractTests passed (panel registry, direction rotation, viewport target skeleton).\n";
    return 0;
}
