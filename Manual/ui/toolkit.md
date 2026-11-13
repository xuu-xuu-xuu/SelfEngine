# 自研 UI 文档、样式与布局

## 从文档到矩形

[LoadUiDocumentJson()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Core/UiDocumentLoader.cpp#L172) 把 JSON 转成 [UiDocument](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Core/UiDocument.h#L26) 的元素树。[UiStyleEngine](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Core/UiStyle.h#L373) 解析并应用样式，[UiLayoutEngine](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Core/UiLayout.h#L31) 按父子关系、流向、弹性尺寸或绝对定位计算矩形。[UiBindingTable](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Core/UiBinding.h#L23) 按绑定更新文字、可见性或启用状态。

## 从输入到控件

[UiHitTest](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Core/UiInput.h#L88) 根据布局命中元素，[UiDispatcher](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Core/UiInput.h#L106) 负责事件路由、焦点和指针捕获。基础控件各自保存交互状态，例如 [UiButton](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L18) 记录按压和点击，[UiListView](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L107) 计算可见行，[UiDragDrop](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L218) 管理拖放，[UiSplitter](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L291) 更新分隔位置。同一控件集还提供树、表格、选项卡、下拉框、叠层与画布。

## 从绘制列表到三角形

[UiDisplayList](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/UiPaint.h#L68) 实现 [UiPaintSink](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Interfaces/UiPaintSink.h#L14)，收集裁剪、文字、图像、线和矩形。[UiGeometryBuilder](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/Rendering/UiPaint.h#L134) 通过 [UiImageProvider](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Interfaces/UiImageProvider.h#L17) 解析图像并生成 GUI 顶点、索引和批次；渲染器据此绘制。
