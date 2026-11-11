#include "ToolLayer/Editor/EditorPlayInput.h"
#include <imgui/imgui_internal.h>
#include <Windows.h>
#include <imm.h>
#include <algorithm>
#include <iostream>

#define CHECK(condition) do { if (!(condition)) { std::cerr << "FAIL line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while(false)
static LPARAM Point(float x,float y){return MAKELPARAM(static_cast<short>(x),static_cast<short>(y));}
int main()
{
    HWND window=CreateWindowExW(0,L"STATIC",L"SelfEngine Play input test",WS_POPUP,0,0,800,600,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    CHECK(window);
    auto* context=ImGui::CreateContext();auto& io=ImGui::GetIO();io.IniFilename=nullptr;io.LogFilename=nullptr;
    io.DisplaySize={800,600};io.DeltaTime=1.0f/60;unsigned char* pixels;int width,height;io.Fonts->GetTexDataAsRGBA32(&pixels,&width,&height);
    se::UiRect viewport;
    const auto draw=[&](bool overlay){
        ImGui::NewFrame();ImGui::SetNextWindowPos({100,100},ImGuiCond_Always);ImGui::SetNextWindowSize({500,350},ImGuiCond_Always);
        ImGui::Begin("Scene###EditorUi.Viewport",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);
        const auto pos=ImGui::GetCursorScreenPos(),size=ImGui::GetContentRegionAvail();viewport={pos.x,pos.y,size.x,size.y};
        ImGui::InvisibleButton("scene-content",size);ImGui::End();
        if(overlay){ImGui::SetNextWindowPos({viewport.x+200,viewport.y+30},ImGuiCond_Always);ImGui::SetNextWindowSize({160,150},ImGuiCond_Always);ImGui::SetNextWindowFocus();
            ImGui::Begin("Overlay",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoResize);ImGui::Button("Editor action");ImGui::End();}
        ImGui::Render();
    };
    draw(true);draw(true);
    se::UiTree ui;
    const auto text=ui.Create(se::UiKind::TextInput),button=ui.Create(se::UiKind::Button),covered=ui.Create(se::UiKind::Button);
    se::UiStyle style;style.offset={10,10};style.size={120,30};CHECK(ui.SetStyle(text,style));
    style.offset={10,70};CHECK(ui.SetStyle(button,style));style.offset={220,70};CHECK(ui.SetStyle(covered,style));
    {
        se::EditorPlayInput route(context);route.Attach(&ui);route.SetViewport(window,viewport);
        CHECK(!route.Active()&&route.KeyboardConsumed()&&route.PointerConsumed());
        const auto click=[&](float x,float y){route.Process(window,WM_LBUTTONDOWN,MK_LBUTTON,Point(x,y));return route.Process(window,WM_LBUTTONUP,0,Point(x,y));};
        // 覆盖面板必须阻断后面的场景控件。
        click(viewport.x+230,viewport.y+80);CHECK(!route.Active());
        auto events=ui.TakeEvents();CHECK(std::none_of(events.begin(),events.end(),[&](const auto&e){return e.target==covered&&e.kind==se::UiEventKind::Click;}));
        // 点击露出的视口立即激活并换算客户区原点，同批文字只交给 Game UI。
        io.AddKeyEvent(ImGuiKey_W,true);io.AddInputCharacter('X');io.AddMousePosEvent(viewport.x+20,viewport.y+20);
        click(viewport.x+20,viewport.y+20);CHECK(route.Active()&&ui.Focused()==text&&route.KeyboardConsumed());
        CHECK(std::none_of(context->InputEventsQueue.begin(),context->InputEventsQueue.end(),[](const auto& event){return event.Type==ImGuiInputEventType_Key||event.Type==ImGuiInputEventType_Text;}));
        CHECK(std::any_of(context->InputEventsQueue.begin(),context->InputEventsQueue.end(),[](const auto& event){return event.Type==ImGuiInputEventType_MousePos;}));
        const auto charCount=context->InputEventsQueue.Size;
        CHECK(!route.Process(window,WM_CHAR,0x4e2d,0).forwardEditor);
        route.Process(window,WM_CHAR,0xd83d,0);route.Process(window,WM_CHAR,0xde00,0);
        CHECK(ui.Get(text)->text=="中😀"&&context->InputEventsQueue.Size==charCount);
        if(HIMC ime=ImmGetContext(window))
        {CANDIDATEFORM candidate{};if(ImmGetCandidateWindow(ime,0,&candidate))CHECK(candidate.ptCurrentPos.x>=viewport.x&&candidate.ptCurrentPos.y>=viewport.y);ImmReleaseContext(window,ime);}
        InputState raw;raw.keysDown['W']=true;raw.isLeftDown=true;raw.mouseButtons[0]=true;raw.mouseX=static_cast<int>(viewport.x+350);raw.mouseY=static_cast<int>(viewport.y+220);
        raw.keyEvents.push_back({KeyEventType::Pressed,'W'});raw.textEvents.push_back({u'A'});raw.pointerEvents.push_back({PointerEventType::Down,raw.mouseX,raw.mouseY,0,0});
        auto filtered=route.GameplayInput(raw);CHECK(!filtered.keysDown['W']&&filtered.textEvents.empty()&&!filtered.isLeftDown);
        route.EndFrame();
        click(viewport.x+350,viewport.y+220);CHECK(route.Active()&&!ui.Focused()&&!route.KeyboardConsumed()&&!route.PointerConsumed());
        CHECK(!route.Process(window,WM_KEYDOWN,'W',0).forwardEditor);
        filtered=route.GameplayInput(raw);CHECK(filtered.keysDown['W']&&filtered.mouseX==350&&filtered.mouseY==220&&filtered.pointerEvents[0].x==350);
        route.EndFrame();
        // 没推进模拟时间也可操作暂停菜单；按钮消费指针，不能触发游戏点击。
        click(viewport.x+20,viewport.y+80);events=ui.TakeEvents();
        CHECK(std::count_if(events.begin(),events.end(),[&](const auto&e){return e.target==button&&e.kind==se::UiEventKind::Click;})==1);
        CHECK(route.PointerConsumed()&&!route.GameplayInput(raw).isLeftDown);
        route.EndFrame();
        draw(true);route.SetViewport(window,viewport);CHECK(!route.Active()&&!ui.Focused());
        click(viewport.x+230,viewport.y+80);CHECK(!route.Active());
        click(viewport.x+20,viewport.y+20);CHECK(route.Active());route.Process(window,WM_KILLFOCUS,0,0);
        CHECK(!route.Active()&&!ui.Focused()&&!route.GameplayInput(raw).keysDown['W']);
        route.Attach(nullptr);
    }
    ImGui::DestroyContext(context);DestroyWindow(window);
    std::cout<<"EditorPlayInputTests passed (viewport focus/z-order, relative input, UTF16, consumption, pause UI, focus reset).\n";
    return 0;
}
