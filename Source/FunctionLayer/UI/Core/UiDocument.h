#pragma once

#include "FunctionLayer/UI/Core/UiElement.h"
#include "FunctionLayer/UI/Core/UiStyle.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace se::ui
{

enum class UiDocumentError : std::uint8_t
{
    None = 0,
    InvalidHandle,
    InvalidParent,
    RootOperation,
    Cycle,
    DuplicateName,
};

// 文档拥有元素存储和树关系。它不负责文件 IO、绑定求值、样式级联或布局；
// 加载、绑定、布局通过独立类型协作。文档不是线程安全的。
class UiDocument
{
public:
    UiDocument();
    ~UiDocument();

    UiDocument(UiDocument&&) noexcept;
    UiDocument& operator=(UiDocument&&) noexcept;

    UiDocument(const UiDocument&) = delete;
    UiDocument& operator=(const UiDocument&) = delete;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] UiElementHandle Root() const noexcept;
    [[nodiscard]] const UiElement* RootElement() const noexcept;

    [[nodiscard]] const UiElement* Element(UiElementHandle handle) const noexcept;
    [[nodiscard]] const UiElement* Find(UiId id) const noexcept;
    [[nodiscard]] const UiElement* FindByName(std::string_view name) const noexcept;

    [[nodiscard]] std::size_t ElementCount() const noexcept;
    [[nodiscard]] std::size_t Capacity() const noexcept;

    UiElementHandle CreateElement(UiElementKind kind);
    bool DestroyElement(UiElementHandle element, UiDocumentError* error = nullptr);

    bool SetParent(UiElementHandle child, UiElementHandle parent, UiDocumentError* error = nullptr);
    [[nodiscard]] bool IsAncestor(UiElementHandle ancestor, UiElementHandle descendant) const noexcept;

    [[nodiscard]] UiElementHandle FirstChild(UiElementHandle parent) const noexcept;
    [[nodiscard]] UiElementHandle NextSibling(UiElementHandle element) const noexcept;
    [[nodiscard]] std::size_t ChildCount(UiElementHandle parent) const noexcept;

    bool SetKind(UiElementHandle element, UiElementKind kind);
    bool SetStyle(UiElementHandle element, const UiStyle& style);
    [[nodiscard]] const UiStyle& Style(UiElementHandle element) const noexcept;
    [[nodiscard]] UiStyleMask InlineStyleMask(UiElementHandle element) const noexcept;
    [[nodiscard]] std::uint64_t LayoutRevision() const noexcept;
    [[nodiscard]] std::uint64_t StyleRevision() const noexcept;
    bool SetName(UiElementHandle element, std::string_view name, UiDocumentError* error = nullptr);
    [[nodiscard]] std::string_view Name(UiElementHandle element) const noexcept;

    bool AddClass(UiElementHandle element, std::string_view className);
    bool RemoveClass(UiElementHandle element, std::string_view className);
    [[nodiscard]] bool HasClass(UiElementHandle element, std::string_view className) const noexcept;
    [[nodiscard]] std::string_view ClassName(UiNameId id) const noexcept;

    bool SetText(UiElementHandle element, std::string_view text);
    [[nodiscard]] std::string_view Text(UiElementHandle element) const noexcept;
    bool SetVisible(UiElementHandle element, bool visible);
    [[nodiscard]] bool Visible(UiElementHandle element) const noexcept;
    bool SetEnabled(UiElementHandle element, bool enabled);
    [[nodiscard]] bool Enabled(UiElementHandle element) const noexcept;

    void MarkDirty(UiElementHandle element, UiDirtyFlags flags);
    void ClearDirty(UiElementHandle element, UiDirtyFlags flags);
    [[nodiscard]] UiDirtyFlags Dirty(UiElementHandle element) const noexcept;
    [[nodiscard]] std::size_t DirtyCount() const noexcept;
    void ClearAllDirty() noexcept;

private:
    struct Slot;
    struct Impl;
    std::unique_ptr<Impl> impl_;

    [[nodiscard]] Slot* SlotFor(UiElementHandle handle) noexcept;
    [[nodiscard]] const Slot* SlotFor(UiElementHandle handle) const noexcept;
    [[nodiscard]] std::uint32_t AllocateSlot() noexcept;
    void DetachFromParent(Slot& slot) noexcept;
    void AppendChild(Slot& parent, Slot& child) noexcept;
    void MarkDirtyInternal(std::uint32_t slotIndex, UiDirtyFlags flags) noexcept;
    void DestroyElementInternal(std::uint32_t slotIndex) noexcept;
};

} // namespace se::ui
