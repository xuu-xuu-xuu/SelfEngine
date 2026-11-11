#include "ImGuiGeometry.h"
#include <imgui/imgui.h>
#include <limits>
namespace se
{
bool CopyImGuiGeometry(const ImDrawData& data,GuiGeometry& output)
{
    if(!data.Valid||data.TotalVtxCount<0||data.TotalIdxCount<0||data.CmdListsCount<0
        ||data.TotalVtxCount>2*1024*1024||data.TotalIdxCount>4*1024*1024)return false;
    GuiGeometry result;result.displayPosition={data.DisplayPos.x,data.DisplayPos.y};result.displaySize={data.DisplaySize.x,data.DisplaySize.y};result.framebufferScale={data.FramebufferScale.x,data.FramebufferScale.y};
    result.vertices.reserve(static_cast<size_t>(data.TotalVtxCount));result.indices.reserve(static_cast<size_t>(data.TotalIdxCount));
    for(const auto* list:data.CmdLists)
    {
        if(!list||list->VtxBuffer.Size<0||list->IdxBuffer.Size<0||result.vertices.size()+list->VtxBuffer.Size>2*1024*1024||result.indices.size()+list->IdxBuffer.Size>4*1024*1024)return false;
        const auto vertexBase=static_cast<std::uint32_t>(result.vertices.size()),indexBase=static_cast<std::uint32_t>(result.indices.size());
        for(const auto& vertex:list->VtxBuffer)result.vertices.push_back({{vertex.pos.x,vertex.pos.y},{vertex.uv.x,vertex.uv.y},vertex.col});
        for(const auto index:list->IdxBuffer)result.indices.push_back(static_cast<std::uint32_t>(index));
        for(const auto& command:list->CmdBuffer)
        {
            if(command.UserCallback)
            {if(command.UserCallback!=ImDrawCallback_ResetRenderState)return false;continue;}
            if(command.IdxOffset>static_cast<unsigned>(list->IdxBuffer.Size)||command.ElemCount>static_cast<unsigned>(list->IdxBuffer.Size)-command.IdxOffset||command.VtxOffset>static_cast<unsigned>(list->VtxBuffer.Size))return false;
            for(unsigned i=command.IdxOffset;i<command.IdxOffset+command.ElemCount;++i)
                if(std::uint64_t(list->IdxBuffer[static_cast<int>(i)])+command.VtxOffset>=static_cast<unsigned>(list->VtxBuffer.Size))return false;
            if(result.commands.size()>=65536)return false;
            result.commands.push_back({{command.ClipRect.x,command.ClipRect.y,command.ClipRect.z,command.ClipRect.w},static_cast<std::uint64_t>(command.GetTexID()),
                indexBase+command.IdxOffset,command.ElemCount,vertexBase+command.VtxOffset,false});
        }
    }
    if(result.vertices.size()!=static_cast<size_t>(data.TotalVtxCount)||result.indices.size()!=static_cast<size_t>(data.TotalIdxCount))return false;
    output=std::move(result);return true;
}
}
