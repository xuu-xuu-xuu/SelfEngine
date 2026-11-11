#include "FunctionLayer/UI/Core/UiDocument.h"

#include <algorithm>
#include <deque>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace se::ui
{

struct UiDocument::Slot
{
    UiElement element;
    UiStyle style;
    UiStyleMask inlineStyleMask = 0;
    std::string text;
    bool visible = true;
    bool enabled = true;
    std::uint32_t generation = 1;
    bool alive = false;
};

struct UiDocument::Impl
{
    std::deque<Slot> slots;
    std::vector<std::uint32_t> freeSlots;
    std::unordered_map<std::uint32_t, std::uint32_t> idToSlot;
    std::unordered_map<std::string, std::uint32_t> nameToId;
    std::unordered_map<std::string, UiNameId> classIds;
    std::vector<std::string> classNames;
    std::uint32_t nextId = 1;
    std::uint32_t rootSlot = 0;
    std::size_t aliveCount = 0;
    std::uint64_t layoutRevision = 0;
    std::uint64_t styleRevision = 0;
};

namespace
{
void SetError(UiDocumentError* error, UiDocumentError value) noexcept
{
    if (error)
    {
        *error = value;
    }
}
} // namespace

UiDocument::UiDocument()
    : impl_(std::make_unique<Impl>())
{
    const UiElementHandle root = CreateElement(UiElementKind::Panel);
    impl_->rootSlot = root.index - 1;
}

UiDocument::~UiDocument() = default;

UiDocument::UiDocument(UiDocument&&) noexcept = default;

UiDocument& UiDocument::operator=(UiDocument&&) noexcept = default;

bool UiDocument::IsValid() const noexcept
{
    return impl_ != nullptr;
}

UiDocument::Slot* UiDocument::SlotFor(UiElementHandle handle) noexcept
{
    if (!impl_ || !handle.IsValid())
    {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(handle.index - 1);
    if (index >= impl_->slots.size())
    {
        return nullptr;
    }
    Slot& slot = impl_->slots[index];
    if (!slot.alive || slot.generation != handle.generation)
    {
        return nullptr;
    }
    return &slot;
}

const UiDocument::Slot* UiDocument::SlotFor(UiElementHandle handle) const noexcept
{
    if (!impl_ || !handle.IsValid())
    {
        return nullptr;
    }
    const std::size_t index = static_cast<std::size_t>(handle.index - 1);
    if (index >= impl_->slots.size())
    {
        return nullptr;
    }
    const Slot& slot = impl_->slots[index];
    if (!slot.alive || slot.generation != handle.generation)
    {
        return nullptr;
    }
    return &slot;
}

std::uint32_t UiDocument::AllocateSlot() noexcept
{
    std::uint32_t index = 0;
    if (!impl_->freeSlots.empty())
    {
        index = impl_->freeSlots.back();
        impl_->freeSlots.pop_back();
    }
    else
    {
        impl_->slots.emplace_back();
        index = static_cast<std::uint32_t>(impl_->slots.size() - 1);
    }

    Slot& slot = impl_->slots[index];
    slot.element = UiElement{};
    slot.style = UiStyle{};
    slot.inlineStyleMask = 0;
    slot.text.clear();
    slot.visible = true;
    slot.enabled = true;
    slot.alive = true;
    slot.element.handle_ = UiElementHandle{index + 1, slot.generation};
    slot.element.id_ = UiId{impl_->nextId++};
    slot.element.dirty_ = UiDirtyFlags::All;
    ++impl_->aliveCount;
    impl_->idToSlot[slot.element.id_.value] = index;
    return index;
}

UiElementHandle UiDocument::Root() const noexcept
{
    if (!impl_ || impl_->slots.empty())
    {
        return {};
    }
    return impl_->slots[impl_->rootSlot].element.handle_;
}

const UiElement* UiDocument::RootElement() const noexcept
{
    return Element(Root());
}

const UiElement* UiDocument::Element(UiElementHandle handle) const noexcept
{
    const Slot* slot = SlotFor(handle);
    return slot ? &slot->element : nullptr;
}

const UiElement* UiDocument::Find(UiId id) const noexcept
{
    if (!impl_ || !id.IsValid())
    {
        return nullptr;
    }
    const auto found = impl_->idToSlot.find(id.value);
    if (found == impl_->idToSlot.end())
    {
        return nullptr;
    }
    const Slot& slot = impl_->slots[found->second];
    return slot.alive ? &slot.element : nullptr;
}

const UiElement* UiDocument::FindByName(std::string_view name) const noexcept
{
    if (!impl_ || name.empty())
    {
        return nullptr;
    }
    const auto found = impl_->nameToId.find(std::string(name));
    if (found == impl_->nameToId.end())
    {
        return nullptr;
    }
    return Find(UiId{found->second});
}

std::size_t UiDocument::ElementCount() const noexcept
{
    return impl_ ? impl_->aliveCount : 0;
}

std::size_t UiDocument::Capacity() const noexcept
{
    return impl_ ? impl_->slots.size() : 0;
}

UiElementHandle UiDocument::CreateElement(UiElementKind kind)
{
    if (!impl_)
    {
        return {};
    }
    const std::uint32_t index = AllocateSlot();
    Slot& slot = impl_->slots[index];
    slot.element.kind_ = kind;
    return slot.element.handle_;
}

bool UiDocument::DestroyElement(UiElementHandle element, UiDocumentError* error)
{
    SetError(error, UiDocumentError::None);
    if (!SlotFor(element))
    {
        SetError(error, UiDocumentError::InvalidHandle);
        return false;
    }
    if (element == Root())
    {
        SetError(error, UiDocumentError::RootOperation);
        return false;
    }
    DestroyElementInternal(element.index - 1);
    return true;
}

void UiDocument::DestroyElementInternal(std::uint32_t slotIndex) noexcept
{
    if (!impl_ || slotIndex >= impl_->slots.size())
    {
        return;
    }
    Slot& slot = impl_->slots[slotIndex];
    if (!slot.alive)
    {
        return;
    }

    const UiElementHandle parentHandle = slot.element.parent_;
    DetachFromParent(slot);

    while (slot.element.firstChild_.IsValid())
    {
        DestroyElementInternal(slot.element.firstChild_.index - 1);
    }

    impl_->idToSlot.erase(slot.element.id_.value);
    if (!slot.element.name_.empty())
    {
        impl_->nameToId.erase(slot.element.name_);
    }

    slot.alive = false;
    --impl_->aliveCount;
    slot.element = UiElement{};
    slot.style = UiStyle{};
    slot.inlineStyleMask = 0;
    slot.text.clear();
    slot.visible = true;
    slot.enabled = true;

    if (slot.generation != 0xFFFFFFFFu)
    {
        ++slot.generation;
        impl_->freeSlots.push_back(slotIndex);
    }

    if (parentHandle.IsValid())
    {
        MarkDirtyInternal(parentHandle.index - 1, UiDirtyFlags::Structure | UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    }
}

bool UiDocument::SetParent(UiElementHandle childHandle, UiElementHandle parentHandle, UiDocumentError* error)
{
    SetError(error, UiDocumentError::None);
    Slot* child = SlotFor(childHandle);
    if (!child)
    {
        SetError(error, UiDocumentError::InvalidHandle);
        return false;
    }
    if (childHandle == Root())
    {
        SetError(error, UiDocumentError::RootOperation);
        return false;
    }

    Slot* parent = nullptr;
    if (parentHandle.IsValid())
    {
        parent = SlotFor(parentHandle);
        if (!parent)
        {
            SetError(error, UiDocumentError::InvalidParent);
            return false;
        }
        if (parentHandle == childHandle || IsAncestor(childHandle, parentHandle))
        {
            SetError(error, UiDocumentError::Cycle);
            return false;
        }
    }

    if (child->element.parent_ == parentHandle)
    {
        return true;
    }

    const UiElementHandle oldParentHandle = child->element.parent_;
    DetachFromParent(*child);
    if (parent)
    {
        AppendChild(*parent, *child);
    }

    MarkDirtyInternal(childHandle.index - 1, UiDirtyFlags::Structure | UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    if (oldParentHandle.IsValid())
    {
        MarkDirtyInternal(oldParentHandle.index - 1, UiDirtyFlags::Structure | UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    }
    if (parentHandle.IsValid())
    {
        MarkDirtyInternal(parentHandle.index - 1, UiDirtyFlags::Structure | UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    }
    return true;
}

bool UiDocument::IsAncestor(UiElementHandle ancestor, UiElementHandle descendant) const noexcept
{
    const Slot* ancestorSlot = SlotFor(ancestor);
    const Slot* descendantSlot = SlotFor(descendant);
    if (!ancestorSlot || !descendantSlot)
    {
        return false;
    }

    UiElementHandle parent = descendantSlot->element.parent_;
    while (parent.IsValid())
    {
        if (parent == ancestor)
        {
            return true;
        }
        const Slot* parentSlot = SlotFor(parent);
        if (!parentSlot)
        {
            return false;
        }
        parent = parentSlot->element.parent_;
    }
    return false;
}

void UiDocument::DetachFromParent(Slot& slot) noexcept
{
    const UiElementHandle self = slot.element.handle_;
    const UiElementHandle parentHandle = slot.element.parent_;
    if (parentHandle.IsValid())
    {
        Slot* parent = SlotFor(parentHandle);
        if (parent)
        {
            if (parent->element.firstChild_ == self)
            {
                parent->element.firstChild_ = slot.element.nextSibling_;
            }
            if (parent->element.lastChild_ == self)
            {
                parent->element.lastChild_ = slot.element.previousSibling_;
            }
        }
    }
    if (slot.element.previousSibling_.IsValid())
    {
        Slot* previous = SlotFor(slot.element.previousSibling_);
        if (previous)
        {
            previous->element.nextSibling_ = slot.element.nextSibling_;
        }
    }
    if (slot.element.nextSibling_.IsValid())
    {
        Slot* next = SlotFor(slot.element.nextSibling_);
        if (next)
        {
            next->element.previousSibling_ = slot.element.previousSibling_;
        }
    }
    slot.element.parent_ = {};
    slot.element.previousSibling_ = {};
    slot.element.nextSibling_ = {};
}

void UiDocument::AppendChild(Slot& parent, Slot& child) noexcept
{
    child.element.parent_ = parent.element.handle_;
    child.element.previousSibling_ = parent.element.lastChild_;
    child.element.nextSibling_ = {};
    if (parent.element.lastChild_.IsValid())
    {
        Slot* last = SlotFor(parent.element.lastChild_);
        if (last)
        {
            last->element.nextSibling_ = child.element.handle_;
        }
    }
    else
    {
        parent.element.firstChild_ = child.element.handle_;
    }
    parent.element.lastChild_ = child.element.handle_;
}

UiElementHandle UiDocument::FirstChild(UiElementHandle parent) const noexcept
{
    const Slot* slot = SlotFor(parent);
    return slot ? slot->element.firstChild_ : UiElementHandle{};
}

UiElementHandle UiDocument::NextSibling(UiElementHandle element) const noexcept
{
    const Slot* slot = SlotFor(element);
    return slot ? slot->element.nextSibling_ : UiElementHandle{};
}

std::size_t UiDocument::ChildCount(UiElementHandle parent) const noexcept
{
    const Slot* slot = SlotFor(parent);
    if (!slot)
    {
        return 0;
    }
    std::size_t count = 0;
    UiElementHandle child = slot->element.firstChild_;
    while (child.IsValid())
    {
        ++count;
        const Slot* childSlot = SlotFor(child);
        if (!childSlot)
        {
            break;
        }
        child = childSlot->element.nextSibling_;
    }
    return count;
}

bool UiDocument::SetKind(UiElementHandle element, UiElementKind kind)
{
    Slot* slot = SlotFor(element);
    if (!slot)
    {
        return false;
    }
    if (slot->element.kind_ == kind)
    {
        return true;
    }
    slot->element.kind_ = kind;
    MarkDirtyInternal(element.index - 1, UiDirtyFlags::Structure | UiDirtyFlags::Style | UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    return true;
}

bool UiDocument::SetStyle(UiElementHandle element, const UiStyle& style)
{
    Slot* slot = SlotFor(element);
    if (!slot)
    {
        return false;
    }
    if (slot->style == style)
    {
        return true;
    }
    slot->style = style;
    slot->inlineStyleMask = DiffStyleMask(style);
    MarkDirtyInternal(element.index - 1, UiDirtyFlags::Style | UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    return true;
}

const UiStyle& UiDocument::Style(UiElementHandle element) const noexcept
{
    static const UiStyle defaultStyle{};
    const Slot* slot = SlotFor(element);
    return slot ? slot->style : defaultStyle;
}

UiStyleMask UiDocument::InlineStyleMask(UiElementHandle element) const noexcept
{
    const Slot* slot = SlotFor(element);
    return slot ? slot->inlineStyleMask : 0;
}

std::uint64_t UiDocument::LayoutRevision() const noexcept
{
    return impl_ ? impl_->layoutRevision : 0;
}

std::uint64_t UiDocument::StyleRevision() const noexcept
{
    return impl_ ? impl_->styleRevision : 0;
}

bool UiDocument::SetName(UiElementHandle element, std::string_view name, UiDocumentError* error)
{
    SetError(error, UiDocumentError::None);
    Slot* slot = SlotFor(element);
    if (!slot)
    {
        SetError(error, UiDocumentError::InvalidHandle);
        return false;
    }
    if (std::string_view(slot->element.name_) == name)
    {
        return true;
    }
    if (!name.empty())
    {
        const auto existing = impl_->nameToId.find(std::string(name));
        if (existing != impl_->nameToId.end() && existing->second != slot->element.id_.value)
        {
            SetError(error, UiDocumentError::DuplicateName);
            return false;
        }
    }
    if (!slot->element.name_.empty())
    {
        impl_->nameToId.erase(slot->element.name_);
    }
    slot->element.name_.assign(name);
    if (!slot->element.name_.empty())
    {
        impl_->nameToId[slot->element.name_] = slot->element.id_.value;
    }
    MarkDirtyInternal(element.index - 1, UiDirtyFlags::Style | UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    return true;
}

std::string_view UiDocument::Name(UiElementHandle element) const noexcept
{
    const Slot* slot = SlotFor(element);
    return slot ? std::string_view(slot->element.name_) : std::string_view{};
}

bool UiDocument::AddClass(UiElementHandle element, std::string_view className)
{
    Slot* slot = SlotFor(element);
    if (!slot || className.empty())
    {
        return false;
    }
    const std::string key(className);
    UiNameId id = kInvalidUiNameId;
    const auto found = impl_->classIds.find(key);
    if (found != impl_->classIds.end())
    {
        id = found->second;
    }
    else
    {
        id = static_cast<UiNameId>(impl_->classNames.size() + 1);
        impl_->classIds.emplace(key, id);
        impl_->classNames.push_back(key);
    }
    if (std::find(slot->element.classes_.begin(), slot->element.classes_.end(), id) != slot->element.classes_.end())
    {
        return true;
    }
    slot->element.classes_.push_back(id);
    MarkDirtyInternal(element.index - 1, UiDirtyFlags::Style | UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    return true;
}

bool UiDocument::RemoveClass(UiElementHandle element, std::string_view className)
{
    Slot* slot = SlotFor(element);
    if (!slot || className.empty())
    {
        return false;
    }
    const auto found = impl_->classIds.find(std::string(className));
    if (found == impl_->classIds.end())
    {
        return true;
    }
    const auto removed = std::remove(slot->element.classes_.begin(), slot->element.classes_.end(), found->second);
    if (removed == slot->element.classes_.end())
    {
        return true;
    }
    slot->element.classes_.erase(removed, slot->element.classes_.end());
    MarkDirtyInternal(element.index - 1, UiDirtyFlags::Style | UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    return true;
}

bool UiDocument::HasClass(UiElementHandle element, std::string_view className) const noexcept
{
    const Slot* slot = SlotFor(element);
    if (!slot || className.empty())
    {
        return false;
    }
    const auto found = impl_->classIds.find(std::string(className));
    if (found == impl_->classIds.end())
    {
        return false;
    }
    return std::find(slot->element.classes_.begin(), slot->element.classes_.end(), found->second) !=
        slot->element.classes_.end();
}

std::string_view UiDocument::ClassName(UiNameId id) const noexcept
{
    if (!impl_ || id == kInvalidUiNameId || id > impl_->classNames.size())
    {
        return {};
    }
    return impl_->classNames[id - 1];
}

bool UiDocument::SetText(UiElementHandle element, std::string_view text)
{
    Slot* slot = SlotFor(element);
    if (!slot || std::string_view(slot->text) == text)
    {
        return false;
    }
    slot->text.assign(text);
    MarkDirtyInternal(element.index - 1, UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    return true;
}

std::string_view UiDocument::Text(UiElementHandle element) const noexcept
{
    const Slot* slot = SlotFor(element);
    return slot ? std::string_view(slot->text) : std::string_view{};
}

bool UiDocument::SetVisible(UiElementHandle element, bool visible)
{
    Slot* slot = SlotFor(element);
    if (!slot || slot->visible == visible)
    {
        return false;
    }
    slot->visible = visible;
    MarkDirtyInternal(element.index - 1, UiDirtyFlags::Layout | UiDirtyFlags::Paint);
    return true;
}

bool UiDocument::Visible(UiElementHandle element) const noexcept
{
    const Slot* slot = SlotFor(element);
    return slot ? slot->visible : false;
}

bool UiDocument::SetEnabled(UiElementHandle element, bool enabled)
{
    Slot* slot = SlotFor(element);
    if (!slot || slot->enabled == enabled)
    {
        return false;
    }
    slot->enabled = enabled;
    MarkDirtyInternal(element.index - 1, UiDirtyFlags::Paint);
    return true;
}

bool UiDocument::Enabled(UiElementHandle element) const noexcept
{
    const Slot* slot = SlotFor(element);
    return slot ? slot->enabled : false;
}

void UiDocument::MarkDirty(UiElementHandle element, UiDirtyFlags flags)
{
    if (SlotFor(element))
    {
        MarkDirtyInternal(element.index - 1, flags);
    }
}

void UiDocument::MarkDirtyInternal(std::uint32_t slotIndex, UiDirtyFlags flags) noexcept
{
    if (!impl_ || slotIndex >= impl_->slots.size() || flags == UiDirtyFlags::None)
    {
        return;
    }
    Slot& slot = impl_->slots[slotIndex];
    if (!slot.alive)
    {
        return;
    }

    UiDirtyFlags effective = flags;
    if (HasFlag(effective, UiDirtyFlags::Structure) || HasFlag(effective, UiDirtyFlags::Style))
    {
        effective |= UiDirtyFlags::Layout | UiDirtyFlags::Paint;
    }
    if (HasFlag(effective, UiDirtyFlags::Layout))
    {
        ++impl_->layoutRevision;
    }
    if (HasFlag(effective, UiDirtyFlags::Style) || HasFlag(effective, UiDirtyFlags::Structure))
    {
        ++impl_->styleRevision;
    }
    slot.element.dirty_ |= effective;

    UiElementHandle parent = slot.element.parent_;
    while (parent.IsValid())
    {
        Slot* parentSlot = SlotFor(parent);
        if (!parentSlot)
        {
            break;
        }
        UiDirtyFlags inherited = UiDirtyFlags::None;
        if (HasFlag(effective, UiDirtyFlags::Structure))
        {
            inherited = UiDirtyFlags::Structure | UiDirtyFlags::Layout | UiDirtyFlags::Paint;
        }
        else if (HasFlag(effective, UiDirtyFlags::Layout))
        {
            inherited = UiDirtyFlags::Layout | UiDirtyFlags::Paint;
        }
        else if (HasFlag(effective, UiDirtyFlags::Paint))
        {
            inherited = UiDirtyFlags::Paint;
        }
        parentSlot->element.dirty_ |= inherited;
        parent = parentSlot->element.parent_;
    }
}

void UiDocument::ClearDirty(UiElementHandle element, UiDirtyFlags flags)
{
    Slot* slot = SlotFor(element);
    if (slot)
    {
        slot->element.dirty_ &= ~flags;
    }
}

UiDirtyFlags UiDocument::Dirty(UiElementHandle element) const noexcept
{
    const Slot* slot = SlotFor(element);
    return slot ? slot->element.dirty_ : UiDirtyFlags::None;
}

std::size_t UiDocument::DirtyCount() const noexcept
{
    if (!impl_)
    {
        return 0;
    }
    std::size_t count = 0;
    for (const Slot& slot : impl_->slots)
    {
        if (slot.alive && slot.element.dirty_ != UiDirtyFlags::None)
        {
            ++count;
        }
    }
    return count;
}

void UiDocument::ClearAllDirty() noexcept
{
    if (!impl_)
    {
        return;
    }
    for (Slot& slot : impl_->slots)
    {
        if (slot.alive)
        {
            slot.element.dirty_ = UiDirtyFlags::None;
        }
    }
}

} // namespace se::ui
