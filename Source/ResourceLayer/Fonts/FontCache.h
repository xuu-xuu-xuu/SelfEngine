#pragma once
#include "FunctionLayer/GameUI/UiTree.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace se
{
struct FontPage
{
    unsigned width = 1024, height = 1024;
    std::uint64_t revision = 0;
    std::vector<unsigned char> alpha;
};
struct TextQuad { unsigned page; UiRect destination, pixels; };
class TextLayout
{
public:
    std::vector<TextQuad> quads;
    float width = 0, height = 0;
    std::uint32_t lineCount = 0;
    std::size_t HitTest(float x, float y) const;
    UiRect Caret(std::size_t codepoint) const;
    std::vector<UiRect> Selection(std::size_t first,std::size_t last)const;
private:
    struct State;
    std::shared_ptr<State> state_;
    friend class FontCache;
};
// DirectWrite 负责字体回退、整形与 CPU 栅格化；不创建 GPU 纹理。
// 图集页固定尺寸、只追加；旧像素坐标不移动，渲染器按 revision 上传脏页。
class FontCache
{
public:
    FontCache();
    ~FontCache();
    FontCache(const FontCache&) = delete;
    FontCache& operator=(const FontCache&) = delete;
    bool Layout(std::string_view utf8, float fontSize, float maximumWidth, TextLayout& output,
        std::string& error, std::wstring_view family = L"Microsoft YaHei UI");
    const std::vector<FontPage>& Pages() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
