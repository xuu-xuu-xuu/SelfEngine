# 编辑会话、撤销与播放

## EditorSession

[EditorSession](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSession.h#L18) 持有编辑世界、当前选择和事务历史。层级视图通过 [BuildEditorHierarchy()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSession.cpp#L14) 展平父子关系。连续拖动属性时用 [BeginEdit()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSession.cpp#L182) 和 [CommitEdit()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSession.cpp#L189) 将多次变化合成一个历史项，[Undo()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSession.cpp#L201)、[Redo()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSession.cpp#L207) 恢复对应快照。

## 播放世界

[EditorSession::StartPlay()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSession.cpp#L288) 创建独立运行环境，复制编辑世界，同步场景与昼夜系统后才启动回调。[EditorPlaybackController](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorPlayback.h#L10) 表达播放、暂停、恢复、单步和停止。[EditorPlayInput](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorPlayInput.h#L15) 根据当前视口和捕获状态转交游戏输入。[StopPlay()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSession.h#L22) 释放播放状态，后续编辑继续落在编辑世界。

## 场景文档与恢复

[SceneFileWorkflow](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/SceneFileWorkflow.h#L9) 管理打开、另存、未保存修改和外部覆盖决定。[ExternalChangeMonitor](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/ExternalChangeMonitor.h#L8) 监视当前文件时间和大小；[EditorSessionStore](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/EditorSessionStore.h#L31) 保存偏好、会话与恢复组，[ApplyRecoveryScene()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/e7ca801292037b33172d094cc4a434682ac83c9c/Source/ToolLayer/Editor/RecoveryApply.cpp#L7) 把恢复内容交回场景会话。
