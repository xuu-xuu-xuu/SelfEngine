#include "LoadingRibbon.h"
#include "FlarePalette.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace se
{
namespace
{
using namespace Gdiplus;
constexpr int Samples=137;
constexpr int Strands=23;
using Line=std::array<PointF,Samples>;
using Sheet=std::array<Line,Strands>;
constexpr float Tau=2.0f*std::numbers::pi_v<float>;

// u 沿着飘带，v 横跨布面。扭转方向翻转时，各股金线会自然收束并交叉。
void Shape(Sheet& sheet,float phase,float width,float height)
{
    for(int i=0;i<Samples;++i)
    {
        const float u=static_cast<float>(i)/(Samples-1);
        const float angle=Tau*u;
        const float center=0.5f+0.15f*std::sin(1.22f*angle-phase)
                                +0.065f*std::sin(2.18f*angle+phase);
        const float spread=(0.16f+0.065f*std::sin(angle+phase))*std::sin(1.54f*angle+phase);
        const float bow=0.09f*std::cos(1.17f*angle-phase);
        for(int j=0;j<Strands;++j)
        {
            const float v=2.0f*j/(Strands-1)-1.0f;
            sheet[j][i]={width*u,height*(center+v*spread+(1.0f-v*v)*bow)};
        }
    }
}

void DrawSheet(Graphics& g,const Sheet& sheet,flare::Color ink,bool lit,float penScale)
{
    const auto color=[ink](BYTE alpha){return Color(alpha,ink.r,ink.g,ink.b);};
    std::array<PointF,Samples*2> outline;
    std::copy(sheet.front().begin(),sheet.front().end(),outline.begin());
    std::reverse_copy(sheet.back().begin(),sheet.back().end(),outline.begin()+Samples);
    SolidBrush body(color(lit?36:18));
    g.FillPolygon(&body,outline.data(),static_cast<INT>(outline.size()),FillModeAlternate);

    for(int j=0;j<Strands;++j)
    {
        const bool edge=j==0 || j==Strands-1;
        const float v=static_cast<float>(j)/(Strands-1);
        const BYTE alpha=edge ? (lit?246:94) : static_cast<BYTE>((lit?112:48)+44*v);
        Pen thread(color(alpha),(edge?1.25f:0.68f)*penScale);
        g.DrawLines(&thread,sheet[j].data(),Samples);
    }
}
}

void DrawLoadingRibbon(HDC dc,const RECT& bounds,int percent,float phase)
{
    const float width=static_cast<float>(bounds.right-bounds.left);
    const float height=static_cast<float>(bounds.bottom-bounds.top);
    if(width<=0 || height<=0)return;
    const float filled=width*std::clamp(percent,0,100)/100.0f;
    Sheet sheet;
    Shape(sheet,phase,width,height);
    Graphics g(dc);
    g.SetPageUnit(UnitPixel);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.TranslateTransform(static_cast<float>(bounds.left),static_cast<float>(bounds.top));
    // 两段使用同一帧几何和一个切点，时间只移动纹理，不移动进度边界。
    if(filled<width)
    {
        g.SetClip(RectF(filled,0,width-filled,height));
        DrawSheet(g,sheet,flare::Muted,false,height/64.0f);
    }
    if(filled>0)
    {
        g.SetClip(RectF(0,0,filled,height));
        DrawSheet(g,sheet,flare::Gold,true,height/64.0f);
    }
}
}
