#include "EditorRenderer.h"

namespace se
{
EditorRenderer::EditorRenderer():renderer_(SandboxScene::BasicLit){}
bool EditorRenderer::Initialize(const RenderSurface& surface,const Localization& localization)
{return renderer_.Initialize(surface,localization);}
RenderResult EditorRenderer::Render(const RenderFrame& frame,const Localization& localization)
{return renderer_.Render(frame,localization);}
EditorRenderView EditorRenderer::QueryEditorView()const{return renderer_.QueryEditorView();}
void EditorRenderer::Shutdown()noexcept{renderer_.Shutdown();}
}
