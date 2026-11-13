# 面板拖出与吸附

把场景页签拖出主窗口后，它仍属于同一个编辑器会话。页签在哪个组、窗口放在哪里，由 [EditorDock](#editordock) 记录。屏幕上的独立窗口由 [EditorFloatingWindows](#editorfloatingwindows) 管理。

## EditorDock

[`se::ui::editor::EditorDock`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorDock.h#L65) 保存主布局树和浮动窗口布局。树中的 [DockNode](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorDock.h#L35) 可以表示分屏或一组页签，每个 [FloatingDockWindow](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorDock.h#L57) 各有自己的布局树和窗口边界。[查看类定义](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorDock.h#L65)

| 拖放位置 | 对应接口 |
| --- | --- |
| 拖出窗口，成为独立窗口 | [FloatTab()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L441) 为面板建立浮动布局，返回窗口 ID |
| 拖到目标页签组，合并或重排 | [MoveTab()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L294) 将页签插入目标组 |
| 拖到内容区域边缘，左右或上下分屏 | [SplitTab()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorUi/EditorDock.cpp#L346) 按指定轴和方向插入新组 |

## EditorFloatingWindows

[`se::EditorFloatingWindows`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorFloatingWindows.h#L15) 连接布局和 Win32 窗口。[Synchronize()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorFloatingWindows.cpp#L71) 按布局同步原生窗口。[RoutePointer()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorFloatingWindows.cpp#L163) 在拖动时寻找指针下的目标窗口，把屏幕位置换算成目标客户区坐标，使页签能拖回主窗口或其他浮动窗口。[查看类定义](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorFloatingWindows.h#L15)
