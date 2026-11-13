# 控件状态、列表与叠层

## 单值控件

[UiButton](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L18) 将按下和释放转换成一次点击，[UiCheckbox](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L38) 保存勾选值和变化标志。[UiCombo](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L56) 在展开、选择与关闭之间维护状态。控件提供可消费的变化，外壳据此执行命令或提交属性。

## 列表与树如何限定工作量

[UiScrollView](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L81) 约束滚动偏移，[UiListView](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L107) 依据偏移、行高和视口计算可见范围。[UiTreeView](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L145) 保存树项的展开状态，[UiTableView](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L166) 组织列和行；布局和数据变化由面板模型驱动。

## 叠层、页签与画布

[UiOverlay](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L186) 管理弹窗等叠层，[UiTabBar](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L310) 跟踪页签。[UiCanvas](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L252) 收集画布图元，[UiDragDrop](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/FunctionLayer/UI/Widgets/UiWidgets.h#L218) 保存拖放过程的状态。它们由[编辑器外壳](../editor/workspace.md)组合，实际跨窗口移动由[布局树](../editor/docking.md#editordock)负责。
