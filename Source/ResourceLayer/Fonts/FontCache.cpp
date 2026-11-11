#include "FontCache.h"
#include "CoreLayer/Utf.h"
#include <Windows.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace se
{
using Microsoft::WRL::ComPtr;
struct TextLayout::State
{
    ComPtr<IDWriteTextLayout> layout;
    std::vector<UINT32> positions;
};
std::size_t TextLayout::HitTest(float x, float y) const
{
    if (!state_ || !std::isfinite(x) || !std::isfinite(y)) return 0;
    BOOL trailing = FALSE, inside = FALSE; DWRITE_HIT_TEST_METRICS metric{};
    if (FAILED(state_->layout->HitTestPoint(x,y,&trailing,&inside,&metric))) return 0;
    const UINT32 position = metric.textPosition + (trailing ? metric.length : 0);
    return static_cast<std::size_t>(std::lower_bound(state_->positions.begin(),state_->positions.end(),position)-state_->positions.begin());
}
UiRect TextLayout::Caret(std::size_t index) const
{
    if (!state_) return {};
    index = std::min(index,state_->positions.size()-1);
    float x = 0, y = 0; DWRITE_HIT_TEST_METRICS metric{};
    if (FAILED(state_->layout->HitTestTextPosition(state_->positions[index],FALSE,&x,&y,&metric))) return {};
    return {x,y,1,metric.height};
}
std::vector<UiRect> TextLayout::Selection(std::size_t first,std::size_t last)const
{
    std::vector<UiRect> result;if(!state_)return result;
    const auto count=state_->positions.size()-1;first=std::min(first,count);last=std::min(last,count);if(first>last)std::swap(first,last);if(first==last)return result;
    const auto start=state_->positions[first],length=state_->positions[last]-start;UINT32 needed=0;
    state_->layout->HitTestTextRange(start,length,0,0,nullptr,0,&needed);
    if(!needed||needed>65536)return result;std::vector<DWRITE_HIT_TEST_METRICS> metrics(needed);
    if(FAILED(state_->layout->HitTestTextRange(start,length,0,0,metrics.data(),needed,&needed)))return result;
    for(UINT32 i=0;i<needed;++i)if(metrics[i].width>0&&metrics[i].height>0)result.push_back({metrics[i].left,metrics[i].top,metrics[i].width,metrics[i].height});
    return result;
}
struct FontCache::Impl : IDWriteTextRenderer
{
    ComPtr<IDWriteFactory> factory;
    std::vector<FontPage> pages;
    // run key 含完整字形/advance/offset；支持整形后的连字和字体回退。
    struct Entry { unsigned page; UiRect pixels; RECT bounds; };
    std::map<std::string,std::vector<Entry>> entries;
    std::vector<ComPtr<IDWriteFontFace>> faces;
    unsigned cursorX=1,cursorY=1,rowHeight=0;
    std::string lastError;
    Impl() { DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,__uuidof(IDWriteFactory),reinterpret_cast<IUnknown**>(factory.GetAddressOf())); }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** output) override
    {
        if(!output)return E_POINTER; *output=nullptr;
        if(id==__uuidof(IUnknown)||id==__uuidof(IDWriteTextRenderer)||id==__uuidof(IDWritePixelSnapping))
        {*output=static_cast<IDWriteTextRenderer*>(this);return S_OK;} return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* disabled) override { *disabled=TRUE;return S_OK; }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* m) override { *m={1,0,0,1,0,0};return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* value) override { *value=1;return S_OK; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void* context,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE mode,
        const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION*,IUnknown*) override
    {
        try
        {
            std::string key;
            const auto append=[&](const auto& value){key.append(reinterpret_cast<const char*>(&value),sizeof(value));};
            append(run->fontFace);append(run->fontEmSize);append(run->isSideways);append(run->bidiLevel);append(mode);
            append(run->glyphCount);append(run->glyphAdvances!=nullptr);append(run->glyphOffsets!=nullptr);
            for(UINT32 i=0;i<run->glyphCount;++i)
            {
                append(run->glyphIndices[i]);
                if(run->glyphAdvances)append(run->glyphAdvances[i]);
                if(run->glyphOffsets){append(run->glyphOffsets[i].advanceOffset);append(run->glyphOffsets[i].ascenderOffset);}
            }
            auto found=entries.find(key);
            if(found==entries.end())
            {
                ComPtr<IDWriteGlyphRunAnalysis> analysis;
                HRESULT hr=factory->CreateGlyphRunAnalysis(run,1,nullptr,DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,mode,0,0,&analysis);
                if(FAILED(hr))return hr;
                RECT bounds{}; hr=analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_CLEARTYPE_3x1,&bounds);
                if(FAILED(hr))return hr;
                const unsigned w=static_cast<unsigned>(bounds.right-bounds.left),h=static_cast<unsigned>(bounds.bottom-bounds.top);
                if(!w||!h)return S_OK;
                if(static_cast<std::uint64_t>(w)*h>32*1024*1024){lastError="Font.RunTooLarge";return E_INVALIDARG;}
                std::vector<unsigned char> pixels(static_cast<std::size_t>(w)*h*3);
                hr=analysis->CreateAlphaTexture(DWRITE_TEXTURE_CLEARTYPE_3x1,&bounds,pixels.data(),static_cast<UINT32>(pixels.size()));
                if(FAILED(hr))return hr;
                std::vector<Entry> tiles;
                for(unsigned tileY=0;tileY<h;tileY+=1022)for(unsigned tileX=0;tileX<w;tileX+=1022)
                {
                    const unsigned tw=std::min(1022u,w-tileX),th=std::min(1022u,h-tileY);
                    if(cursorX+tw+1>1024){cursorX=1;cursorY+=rowHeight+1;rowHeight=0;}
                    if(pages.empty()||cursorY+th+1>1024)
                    {
                        if(pages.size()>=64){lastError="Font.AtlasFull";return E_OUTOFMEMORY;}
                        FontPage page;page.alpha.resize(1024*1024);pages.push_back(std::move(page));cursorX=cursorY=1;rowHeight=0;
                    }
                    auto& page=pages.back();
                    for(unsigned row=0;row<th;++row)for(unsigned col=0;col<tw;++col)
                    {const auto p=((row+tileY)*w+col+tileX)*3;page.alpha[(cursorY+row)*1024+cursorX+col]=static_cast<unsigned char>((unsigned(pixels[p])+pixels[p+1]+pixels[p+2])/3);}
                    ++page.revision;
                    RECT tileBounds{bounds.left+static_cast<LONG>(tileX),bounds.top+static_cast<LONG>(tileY),
                        bounds.left+static_cast<LONG>(tileX+tw),bounds.top+static_cast<LONG>(tileY+th)};
                    tiles.push_back({static_cast<unsigned>(pages.size()-1),{float(cursorX),float(cursorY),float(tw),float(th)},tileBounds});
                    cursorX+=tw+1;rowHeight=std::max(rowHeight,th);
                }
                faces.emplace_back(run->fontFace);found=entries.emplace(std::move(key),std::move(tiles)).first;
            }
            for(const auto& entry:found->second)
                static_cast<TextLayout*>(context)->quads.push_back({entry.page,
                    {x+entry.bounds.left,y+entry.bounds.top,entry.pixels.width,entry.pixels.height},entry.pixels});
            return S_OK;
        }
        catch(...){return E_OUTOFMEMORY;}
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) override { return E_NOTIMPL; }
};
FontCache::FontCache():impl_(std::make_unique<Impl>()){}
FontCache::~FontCache()=default;
const std::vector<FontPage>& FontCache::Pages() const{return impl_->pages;}
bool FontCache::Layout(std::string_view utf8,float fontSize,float maximumWidth,TextLayout& output,std::string& error,std::wstring_view family)
{
    error.clear();std::u32string decoded;
    if(!impl_->factory){error="Font.InitializeFailed";return false;}
    if(utf8.size()>65536||!DecodeUtf8(utf8,decoded)||!std::isfinite(fontSize)||fontSize<1||fontSize>256
        ||!std::isfinite(maximumWidth)||maximumWidth<=0||maximumWidth>100000){error="Font.InvalidText";return false;}
    auto state=std::make_shared<TextLayout::State>();
    UINT32 position=0;for(char32_t c:decoded){state->positions.push_back(position);position+=c>0xffff?2:1;}state->positions.push_back(position);
    const auto utf16=EncodeUtf16(decoded);ComPtr<IDWriteTextFormat> format;const std::wstring familyName(family);
    HRESULT hr=impl_->factory->CreateTextFormat(familyName.c_str(),nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,fontSize,L"zh-CN",&format);
    if(SUCCEEDED(hr))hr=impl_->factory->CreateTextLayout(reinterpret_cast<const wchar_t*>(utf16.data()),static_cast<UINT32>(utf16.size()),format.Get(),maximumWidth,100000,&state->layout);
    if(FAILED(hr)){error="Font.LayoutFailed";return false;}
    DWRITE_TEXT_METRICS metrics{};state->layout->GetMetrics(&metrics);
    TextLayout next;next.state_=state;next.width=metrics.widthIncludingTrailingWhitespace;next.height=metrics.height;next.lineCount=metrics.lineCount;
    impl_->lastError.clear();hr=state->layout->Draw(&next,impl_.get(),0,0);
    if(FAILED(hr)){error=impl_->lastError.empty()?"Font.RasterizeFailed":impl_->lastError;return false;}
    output=std::move(next);return true;
}
}
