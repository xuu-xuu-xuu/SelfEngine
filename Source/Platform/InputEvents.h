#pragma once

enum class KeyEventType
{
    Pressed,
    Released,
    Reset // 失焦取消全部按键状态，不表示物理按键全部松开。
};

struct KeyEvent
{
    KeyEventType type = KeyEventType::Pressed;
    unsigned int keyCode = 0; // Reset 不使用此字段。
};

struct TextInputEvent
{
    char16_t codeUnit = 0; // 一个 UTF-16 代码单元；当前只收集 w/W。
};
