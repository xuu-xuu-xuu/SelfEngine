#pragma once
#include "FunctionLayer/GameUI/UiTree.h"
#include <optional>

namespace se
{
enum class UiCanvasDrag { Move, Resize };
struct UiCanvasAnchors {UiRect rectangle;UiPoint minimum,maximum,pivot;};
// 只保存编辑视图/手势，设计数据和布局始终属于 UiTree。
class UICanvasEditor
{
public:
    bool SetPreview(UiPoint size);
    UiPoint PreviewSize()const noexcept{return preview_;}
    bool Prepare(UiTree& ui,UiRect viewport);
    UiRect Viewport()const noexcept{return viewport_;}
    UiRect Content()const noexcept{return content_;}
    float ViewScale()const noexcept{return scale_;}
    UiPoint ToScreen(UiPoint point)const noexcept;
    UiPoint ToPreview(UiPoint point)const noexcept;
    UiRect ToScreen(UiRect rectangle)const noexcept;
    UiId Hit(const UiTree& ui,UiPoint screen)const;
    std::vector<UiVisual> Visuals(const UiTree& ui)const;
    std::optional<UiCanvasAnchors> Anchors(const UiTree& ui,UiId id)const;
    bool Begin(UiTree& ui,UiId id,UiCanvasDrag kind,UiPoint screen);
    bool Update(UiTree& ui,UiPoint screen);
    bool End(UiTree& ui,bool commit=true);
    bool Active()const noexcept{return dragId_!=0;}
    void Reset()noexcept{dragId_=0;}
    const std::string& LastError()const noexcept{return error_;}
private:
    bool Fail(const char* key){error_=key;return false;}
    UiPoint preview_{1280,720},dragPreview_{},dragStart_{},initialSize_{};
    UiRect viewport_{},content_{},dragViewport_{};
    float scale_=1,dragCanvasScale_=1;
    UiId dragId_=0;UiCanvasDrag dragKind_{};UiStyle initialStyle_;
    std::string error_;
};
}
