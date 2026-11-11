#include "D3D12DepthPrepass.h"
#include "D3D12Mesh.h"
#include "D3D12Result.h"
#include "D3D12Shader.h"
#include "CoreLayer/Localization.h"

using Microsoft::WRL::ComPtr;
HRESULT D3D12DepthPrepass::Initialize(ID3D12Device& device,const std::filesystem::path& shader,
    ID3D12RootSignature& root,const Localization& text,UINT instanceParameter)
{
    if(pipeline_)return E_UNEXPECTED;
    if(instanceParameter>3)return E_INVALIDARG;
    ComPtr<ID3DBlob> vertex,pixel;
    if(!CompileD3D12Shader(shader,"VSMain","vs_5_1",vertex,text,D3DCOMPILE_IEEE_STRICTNESS)
        ||!CompileD3D12Shader(shader,"PSMain","ps_5_1",pixel,text,D3DCOMPILE_IEEE_STRICTNESS))
    {CheckGraphicsResult(E_FAIL,text,"D3D12.DepthPrepassFailed");return E_FAIL;}
    const D3D12_INPUT_ELEMENT_DESC inputs[]{
        {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
    description.pRootSignature=&root;
    description.VS={vertex->GetBufferPointer(),vertex->GetBufferSize()};
    description.PS={pixel->GetBufferPointer(),pixel->GetBufferSize()};
    description.InputLayout={inputs,1};
    description.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    description.NumRenderTargets=0;description.RTVFormats[0]=DXGI_FORMAT_UNKNOWN;description.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    description.SampleDesc={1,0};description.SampleMask=UINT_MAX;
    description.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;
    description.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;
    description.RasterizerState.DepthClipEnable=TRUE;
    description.RasterizerState.FrontCounterClockwise=TRUE;
    description.DepthStencilState.DepthEnable=TRUE;
    description.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;
    description.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_LESS;
    description.DepthStencilState.FrontFace=description.DepthStencilState.BackFace=
        {D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
    description.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D12PipelineState> pipeline;
    const auto result=device.CreateGraphicsPipelineState(&description,IID_PPV_ARGS(pipeline.GetAddressOf()));
    if(FAILED(result)){CheckGraphicsResult(result,text,"D3D12.DepthPrepassFailed");return result;}
    pipeline->SetName(L"SelfEngine.DepthPrepass");
    root_=&root;pipeline_=std::move(pipeline);instanceParameter_=instanceParameter;return S_OK;
}
void D3D12DepthPrepass::Record(ID3D12GraphicsCommandList& commands,const D3D12Mesh& mesh,
    D3D12_GPU_VIRTUAL_ADDRESS instanceSrv,UINT width,UINT height,UINT instanceCount) const
{
    const D3D12_VIEWPORT viewport{0,0,float(width),float(height),0,1};
    const D3D12_RECT scissor{0,0,static_cast<LONG>(width),static_cast<LONG>(height)};
    commands.SetPipelineState(pipeline_.Get());
    commands.SetGraphicsRootSignature(root_.Get());
    commands.SetGraphicsRootShaderResourceView(instanceParameter_,instanceSrv);
    commands.IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    commands.RSSetViewports(1,&viewport);commands.RSSetScissorRects(1,&scissor);
    commands.IASetVertexBuffers(0,1,&mesh.vertexView);
    commands.IASetIndexBuffer(&mesh.indexView);
    commands.DrawIndexedInstanced(mesh.indexCount,instanceCount,0,0,0);
}
