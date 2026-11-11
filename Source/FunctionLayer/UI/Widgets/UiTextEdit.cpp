#include "FunctionLayer/UI/Widgets/UiTextEdit.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace se::ui
{
namespace
{
std::size_t NextByte(std::string_view text, std::size_t offset) noexcept
{
    if (offset >= text.size())
    {
        return text.size();
    }
    const unsigned char lead = static_cast<unsigned char>(text[offset]);
    std::size_t length = 1;
    if ((lead & 0xE0u) == 0xC0u) length = 2;
    else if ((lead & 0xF0u) == 0xE0u) length = 3;
    else if ((lead & 0xF8u) == 0xF0u) length = 4;
    return std::min(text.size(), offset + length);
}

std::uint32_t CodepointCount(std::string_view text) noexcept
{
    std::uint32_t count = 0;
    for (std::size_t offset = 0; offset < text.size(); offset = NextByte(text, offset))
    {
        ++count;
    }
    return count;
}

std::size_t ByteOffsetForCodepoint(std::string_view text, std::uint32_t codepoint) noexcept
{
    std::size_t offset = 0;
    for (std::uint32_t index = 0; index < codepoint && offset < text.size(); ++index)
    {
        offset = NextByte(text, offset);
    }
    return offset;
}

std::uint32_t CodepointForByteOffset(std::string_view text, std::size_t byteOffset) noexcept
{
    std::uint32_t codepoint = 0;
    for (std::size_t offset = 0; offset < byteOffset && offset < text.size(); offset = NextByte(text, offset))
    {
        ++codepoint;
    }
    return codepoint;
}
} // namespace

struct UiTextEdit::Impl
{
    struct Edit
    {
        std::string before;
        std::string after;
        std::uint32_t caretBefore = 0;
        std::uint32_t caretAfter = 0;
    };

    UiDocument* document = nullptr;
    UiElementHandle element;
    UiTextService* textService = nullptr;
    UiPlatformServices* platform = nullptr;
    std::string text;
    std::string composition;
    std::string clipboard;
    std::uint32_t caret = 0;
    std::uint32_t anchor = 0;
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    float scroll = 0.0f;
    std::vector<Edit> undo;
    std::vector<Edit> redo;

    void Sync()
    {
        if (document && document->Element(element))
        {
            document->SetText(element, text);
        }
    }

    [[nodiscard]] std::uint32_t Count() const noexcept
    {
        return CodepointCount(text);
    }

    void ClampCaret()
    {
        const std::uint32_t count = Count();
        caret = std::min(caret, count);
        anchor = std::min(anchor, count);
    }

    [[nodiscard]] std::uint32_t SelectionStart() const noexcept
    {
        return std::min(caret, anchor);
    }

    [[nodiscard]] std::uint32_t SelectionEnd() const noexcept
    {
        return std::max(caret, anchor);
    }

    [[nodiscard]] bool HasSelection() const noexcept
    {
        return caret != anchor;
    }

    [[nodiscard]] std::string Selected() const
    {
        const std::uint32_t start = SelectionStart();
        const std::uint32_t end = SelectionEnd();
        if (start == end)
        {
            return {};
        }
        const std::size_t first = ByteOffsetForCodepoint(text, start);
        const std::size_t last = ByteOffsetForCodepoint(text, end);
        return text.substr(first, last - first);
    }

    [[nodiscard]] std::string TextWithoutSelection() const
    {
        const std::uint32_t start = SelectionStart();
        const std::uint32_t end = SelectionEnd();
        const std::size_t first = ByteOffsetForCodepoint(text, start);
        const std::size_t last = ByteOffsetForCodepoint(text, end);
        return text.substr(0, first) + text.substr(last);
    }

    bool Apply(std::string next, std::uint32_t nextCaret)
    {
        if (next == text)
        {
            return false;
        }
        undo.push_back(Edit{text, next, caret, nextCaret});
        if (undo.size() > 100)
        {
            undo.erase(undo.begin());
        }
        redo.clear();
        text = std::move(next);
        caret = nextCaret;
        anchor = caret;
        ClampCaret();
        Sync();
        EnsureVisible();
        return true;
    }

    void RemoveSelectionOrRange(std::uint32_t first, std::uint32_t last)
    {
        const std::size_t start = ByteOffsetForCodepoint(text, first);
        const std::size_t end = ByteOffsetForCodepoint(text, last);
        const std::string next = text.substr(0, start) + text.substr(end);
        Apply(next, first);
    }

    void EnsureVisible()
    {
        if (viewportWidth <= 0.0f || viewportHeight <= 0.0f)
        {
            return;
        }
        std::uint32_t line = 0;
        float lineHeight = 16.0f;
        if (textService)
        {
            const std::string prefix = text.substr(0, ByteOffsetForCodepoint(text, caret));
            const UiTextMeasure measure = textService->Measure(prefix, 14.0f, viewportWidth, true);
            line = measure.lineCount == 0 ? 0 : measure.lineCount - 1;
            lineHeight = textService->LineHeight(14.0f);
            const UiTextMeasure all = textService->Measure(text, 14.0f, viewportWidth, true);
            const float maximum = std::max(0.0f, all.height - viewportHeight);
            const float wanted = std::max(0.0f, static_cast<float>(line + 1) * lineHeight - viewportHeight);
            scroll = std::clamp(wanted, 0.0f, maximum);
            return;
        }
        for (std::size_t offset = 0; offset < ByteOffsetForCodepoint(text, caret); ++offset)
        {
            if (text[offset] == '\n')
            {
                ++line;
            }
        }
        const float totalLines = static_cast<float>(line + 1);
        scroll = std::max(0.0f, totalLines * lineHeight - viewportHeight);
    }

    void ReadClipboard(std::string& output)
    {
        if (platform && platform->ReadClipboard(output))
        {
            return;
        }
        output = clipboard;
    }

    void WriteClipboard(std::string_view value)
    {
        clipboard.assign(value);
        if (platform)
        {
            (void)platform->WriteClipboard(value);
        }
    }
};

UiTextEdit::UiTextEdit(UiDocument& document, UiElementHandle element, UiTextService* textService,
                       UiPlatformServices* platform)
    : impl_(std::make_unique<Impl>())
{
    impl_->document = &document;
    impl_->element = element;
    impl_->textService = textService;
    impl_->platform = platform;
    if (document.Element(element))
    {
        impl_->text.assign(document.Text(element));
    }
}

UiTextEdit::~UiTextEdit() = default;

UiTextEdit::UiTextEdit(UiTextEdit&&) noexcept = default;

UiTextEdit& UiTextEdit::operator=(UiTextEdit&&) noexcept = default;

void UiTextEdit::SetText(std::string_view text)
{
    impl_->text.assign(text);
    impl_->caret = 0;
    impl_->anchor = 0;
    impl_->composition.clear();
    impl_->undo.clear();
    impl_->redo.clear();
    impl_->scroll = 0.0f;
    impl_->Sync();
    impl_->EnsureVisible();
}

std::string_view UiTextEdit::Text() const noexcept
{
    return impl_->text;
}

std::string UiTextEdit::DisplayText() const
{
    if (impl_->composition.empty())
    {
        return impl_->text;
    }
    const std::size_t first = ByteOffsetForCodepoint(impl_->text, impl_->SelectionStart());
    const std::size_t last = ByteOffsetForCodepoint(impl_->text, impl_->SelectionEnd());
    return impl_->text.substr(0, first) + impl_->composition + impl_->text.substr(last);
}

std::uint32_t UiTextEdit::Caret() const noexcept
{
    return impl_->caret;
}

std::uint32_t UiTextEdit::Anchor() const noexcept
{
    return impl_->anchor;
}

void UiTextEdit::SetCaret(std::uint32_t codepoint)
{
    impl_->caret = codepoint;
    impl_->anchor = codepoint;
    impl_->ClampCaret();
    impl_->EnsureVisible();
}

void UiTextEdit::Select(std::uint32_t anchor, std::uint32_t caret)
{
    impl_->anchor = anchor;
    impl_->caret = caret;
    impl_->ClampCaret();
    impl_->EnsureVisible();
}

std::string UiTextEdit::SelectedText() const
{
    return impl_->Selected();
}

bool UiTextEdit::HandleKey(UiKeyCode key, bool shift, bool control)
{
    if (control)
    {
        if (key == 'A')
        {
            Select(0, impl_->Count());
            return true;
        }
        if (key == 'C')
        {
            return Copy();
        }
        if (key == 'X')
        {
            return Cut();
        }
        if (key == 'V')
        {
            return Paste();
        }
        if (key == 'Z')
        {
            return Undo();
        }
        if (key == 'Y')
        {
            return Redo();
        }
    }
    const std::uint32_t count = impl_->Count();
    if (key == UiKeyLeft || key == UiKeyRight || key == UiKeyHome || key == UiKeyEnd)
    {
        if (!shift && impl_->HasSelection() && (key == UiKeyLeft || key == UiKeyRight))
        {
            impl_->caret = key == UiKeyLeft ? impl_->SelectionStart() : impl_->SelectionEnd();
            impl_->anchor = impl_->caret;
        }
        else
        {
            if (key == UiKeyLeft && impl_->caret > 0) --impl_->caret;
            else if (key == UiKeyRight && impl_->caret < count) ++impl_->caret;
            else if (key == UiKeyHome) impl_->caret = 0;
            else if (key == UiKeyEnd) impl_->caret = count;
            if (!shift) impl_->anchor = impl_->caret;
        }
        impl_->EnsureVisible();
        return true;
    }
    if (key == UiKeyBackspace)
    {
        if (impl_->HasSelection())
        {
            impl_->RemoveSelectionOrRange(impl_->SelectionStart(), impl_->SelectionEnd());
        }
        else if (impl_->caret > 0)
        {
            impl_->RemoveSelectionOrRange(impl_->caret - 1, impl_->caret);
        }
        return true;
    }
    if (key == UiKeyDelete)
    {
        if (impl_->HasSelection())
        {
            impl_->RemoveSelectionOrRange(impl_->SelectionStart(), impl_->SelectionEnd());
        }
        else if (impl_->caret < count)
        {
            impl_->RemoveSelectionOrRange(impl_->caret, impl_->caret + 1);
        }
        return true;
    }
    if (key == UiKeyEnter)
    {
        return InsertText("\n");
    }
    if (key == UiKeySpace)
    {
        return InsertText(" ");
    }
    return false;
}

bool UiTextEdit::InsertText(std::string_view text)
{
    if (text.empty())
    {
        return false;
    }
    const std::string prefix = impl_->TextWithoutSelection().substr(
        0, ByteOffsetForCodepoint(impl_->text, impl_->SelectionStart()));
    const std::string suffix = impl_->text.substr(ByteOffsetForCodepoint(impl_->text, impl_->SelectionEnd()));
    const std::uint32_t inserted = CodepointCount(text);
    const std::uint32_t caret = impl_->SelectionStart() + inserted;
    return impl_->Apply(prefix + std::string(text) + suffix, caret);
}

bool UiTextEdit::Copy()
{
    const std::string selected = impl_->Selected();
    if (selected.empty())
    {
        return false;
    }
    impl_->WriteClipboard(selected);
    return true;
}

bool UiTextEdit::Cut()
{
    const std::string selected = impl_->Selected();
    if (selected.empty())
    {
        return false;
    }
    impl_->WriteClipboard(selected);
    impl_->RemoveSelectionOrRange(impl_->SelectionStart(), impl_->SelectionEnd());
    return true;
}

bool UiTextEdit::Paste()
{
    std::string value;
    impl_->ReadClipboard(value);
    if (value.empty())
    {
        return false;
    }
    return InsertText(value);
}

bool UiTextEdit::Undo()
{
    if (impl_->undo.empty())
    {
        return false;
    }
    Impl::Edit edit = std::move(impl_->undo.back());
    impl_->undo.pop_back();
    impl_->redo.push_back(edit);
    impl_->text = edit.before;
    impl_->caret = edit.caretBefore;
    impl_->anchor = impl_->caret;
    impl_->ClampCaret();
    impl_->Sync();
    impl_->EnsureVisible();
    return true;
}

bool UiTextEdit::Redo()
{
    if (impl_->redo.empty())
    {
        return false;
    }
    Impl::Edit edit = std::move(impl_->redo.back());
    impl_->redo.pop_back();
    impl_->undo.push_back(edit);
    impl_->text = edit.after;
    impl_->caret = edit.caretAfter;
    impl_->anchor = impl_->caret;
    impl_->ClampCaret();
    impl_->Sync();
    impl_->EnsureVisible();
    return true;
}

bool UiTextEdit::CanUndo() const noexcept
{
    return !impl_->undo.empty();
}

bool UiTextEdit::CanRedo() const noexcept
{
    return !impl_->redo.empty();
}

bool UiTextEdit::SetComposition(std::string_view text)
{
    impl_->composition.assign(text);
    return true;
}

bool UiTextEdit::CommitComposition()
{
    if (impl_->composition.empty())
    {
        return false;
    }
    const std::string value = std::move(impl_->composition);
    impl_->composition.clear();
    return InsertText(value);
}

void UiTextEdit::CancelComposition()
{
    impl_->composition.clear();
}

std::string_view UiTextEdit::Composition() const noexcept
{
    return impl_->composition;
}

bool UiTextEdit::Composing() const noexcept
{
    return !impl_->composition.empty();
}

void UiTextEdit::SetViewport(float width, float height)
{
    impl_->viewportWidth = std::max(0.0f, width);
    impl_->viewportHeight = std::max(0.0f, height);
    impl_->EnsureVisible();
}

void UiTextEdit::ScrollBy(float delta)
{
    impl_->scroll = std::max(0.0f, impl_->scroll + delta);
    if (impl_->viewportHeight > 0.0f && impl_->textService)
    {
        const UiTextMeasure all = impl_->textService->Measure(impl_->text, 14.0f, impl_->viewportWidth, true);
        impl_->scroll = std::min(impl_->scroll, std::max(0.0f, all.height - impl_->viewportHeight));
    }
}

float UiTextEdit::ScrollOffset() const noexcept
{
    return impl_->scroll;
}

float UiTextEdit::ViewportWidth() const noexcept
{
    return impl_->viewportWidth;
}

float UiTextEdit::ViewportHeight() const noexcept
{
    return impl_->viewportHeight;
}

void UiTextEdit::EnsureCaretVisible()
{
    impl_->EnsureVisible();
}

} // namespace se::ui
