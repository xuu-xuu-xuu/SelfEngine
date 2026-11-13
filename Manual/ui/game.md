# 游戏 UI、绑定与合成

## 场景 UI 树

[UiTree](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/GameUI/UiTree.h#L76) 保存节点、布局、可见性和输入状态，支持按钮、勾选、滑条、进度条、滚动和文本输入。[UiEventSystem](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/GameUI/UiEventSystem.h#L7) 从指针位置解析目标与传播路径。[UiTransition](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/GameUI/UiTransition.h#L7) 随时间推进属性过渡；[BuildControlsGallery()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/GameUI/ControlsGallery.cpp#L6) 展示各控件的组合，[BuildUiTransitionDemo()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/GameUI/UiTransitionDemo.cpp#L6) 构建带过渡的菜单。

## 文本、图像与游戏事件

[FontCache](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ResourceLayer/Fonts/FontCache.h#L33) 为游戏文字建立图集并返回字形几何。[GameUIRenderer](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/GameUIRenderer.h#L17) 把布局与文本转为绘制批次，配置游戏 UI 层。[UiElement](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Scripting/Managed/GameUi.cs#L41) 是脚本侧代理，修改文字或图像后由原生树参与下次布局。绑定动作的转发见[脚本事件](../scripts/behaviours.md)。

## 编辑器与运行画面合成

[EditorViewportTarget](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/EditorViewportTarget.h#L11) 管理视口离屏目标。[GuiCompositor](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/GuiCompositor.h#L10) 组织场景图像和 GUI 层，D3D12 经 [RecordD3D12GuiLayer()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/PlatformLayer/Graphics/D3D12/D3D12Gui.cpp#L60) 记录具体命令。这样窗口尺寸和编辑器面板内的可用视口可以分别调整。
