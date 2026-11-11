#pragma once

#include "FunctionLayer/UI/Core/UiDocument.h"
#include "FunctionLayer/UI/Core/UiInput.h"
#include "FunctionLayer/UI/Interfaces/UiPlatformServices.h"
#include "FunctionLayer/UI/Interfaces/UiTextService.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace se::ui
{

// 单一文本编辑模型；TextField/TextArea 复用，不直接读全局状态。
class UiTextEdit
{
public:
    UiTextEdit(UiDocument& document, UiElementHandle element, UiTextService* textService = nullptr,
               UiPlatformServices* platform = nullptr);
    ~UiTextEdit();

    UiTextEdit(UiTextEdit&&) noexcept;
    UiTextEdit& operator=(UiTextEdit&&) noexcept;
    UiTextEdit(const UiTextEdit&) = delete;
    UiTextEdit& operator=(const UiTextEdit&) = delete;

    void SetText(std::string_view text);
    [[nodiscard]] std::string_view Text() const noexcept;
    [[nodiscard]] std::string DisplayText() const;

    [[nodiscard]] std::uint32_t Caret() const noexcept;
    [[nodiscard]] std::uint32_t Anchor() const noexcept;
    void SetCaret(std::uint32_t codepoint);
    void Select(std::uint32_t anchor, std::uint32_t caret);
    [[nodiscard]] std::string SelectedText() const;

    bool HandleKey(UiKeyCode key, bool shift, bool control);
    bool InsertText(std::string_view text);
    bool Copy();
    bool Cut();
    bool Paste();
    bool Undo();
    bool Redo();
    [[nodiscard]] bool CanUndo() const noexcept;
    [[nodiscard]] bool CanRedo() const noexcept;

    bool SetComposition(std::string_view text);
    bool CommitComposition();
    void CancelComposition();
    [[nodiscard]] std::string_view Composition() const noexcept;
    [[nodiscard]] bool Composing() const noexcept;

    void SetViewport(float width, float height);
    void ScrollBy(float delta);
    [[nodiscard]] float ScrollOffset() const noexcept;
    [[nodiscard]] float ViewportWidth() const noexcept;
    [[nodiscard]] float ViewportHeight() const noexcept;
    void EnsureCaretVisible();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace se::ui
