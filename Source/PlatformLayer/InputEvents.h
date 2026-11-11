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
    char16_t codeUnit = 0; // UTF-16 代码单元，消费者组合代理对。
};
enum class PointerEventType { Move, Down, Up, Wheel, Cancel };
struct PointerEvent
{
    PointerEventType type = PointerEventType::Move;
    int x = 0, y = 0;
    unsigned button = 0; // 左/右/中 = 0/1/2。
    float wheel = 0;
};
