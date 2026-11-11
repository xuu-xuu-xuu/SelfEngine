#include "FunctionLayer/UI/Core/UiBinding.h"

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace se::ui
{
namespace
{
struct TextBinding
{
    UiElementHandle target;
    std::function<std::string()> getter;
    std::string last;
};

struct BoolBinding
{
    UiElementHandle target;
    std::function<bool()> getter;
    bool last = false;
};
} // namespace

struct UiBindingTable::Impl
{
    UiDocument* document = nullptr;
    std::vector<TextBinding> text;
    std::vector<BoolBinding> visible;
    std::vector<BoolBinding> enabled;
};

UiBindingTable::UiBindingTable(UiDocument& document)
    : impl_(std::make_unique<Impl>())
{
    impl_->document = &document;
}

UiBindingTable::~UiBindingTable() = default;

UiBindingTable::UiBindingTable(UiBindingTable&&) noexcept = default;

UiBindingTable& UiBindingTable::operator=(UiBindingTable&&) noexcept = default;

bool UiBindingTable::BindText(UiElementHandle element, std::function<std::string()> getter)
{
    if (!impl_ || !impl_->document || !impl_->document->Element(element) || !getter)
    {
        return false;
    }
    std::string value = getter();
    impl_->document->SetText(element, value);
    impl_->text.push_back(TextBinding{element, std::move(getter), std::move(value)});
    return true;
}

bool UiBindingTable::BindVisible(UiElementHandle element, std::function<bool()> getter)
{
    if (!impl_ || !impl_->document || !impl_->document->Element(element) || !getter)
    {
        return false;
    }
    const bool value = getter();
    impl_->document->SetVisible(element, value);
    impl_->visible.push_back(BoolBinding{element, std::move(getter), value});
    return true;
}

bool UiBindingTable::BindEnabled(UiElementHandle element, std::function<bool()> getter)
{
    if (!impl_ || !impl_->document || !impl_->document->Element(element) || !getter)
    {
        return false;
    }
    const bool value = getter();
    impl_->document->SetEnabled(element, value);
    impl_->enabled.push_back(BoolBinding{element, std::move(getter), value});
    return true;
}

bool UiBindingTable::Unbind(UiElementHandle element, UiBindingProperty property)
{
    if (!impl_)
    {
        return false;
    }
    const auto matches = [element](const auto& binding)
    {
        return binding.target == element;
    };
    switch (property)
    {
    case UiBindingProperty::Text:
    {
        const auto first = std::remove_if(impl_->text.begin(), impl_->text.end(), matches);
        const bool removed = first != impl_->text.end();
        impl_->text.erase(first, impl_->text.end());
        return removed;
    }
    case UiBindingProperty::Visible:
    {
        const auto first = std::remove_if(impl_->visible.begin(), impl_->visible.end(), matches);
        const bool removed = first != impl_->visible.end();
        impl_->visible.erase(first, impl_->visible.end());
        return removed;
    }
    case UiBindingProperty::Enabled:
    {
        const auto first = std::remove_if(impl_->enabled.begin(), impl_->enabled.end(), matches);
        const bool removed = first != impl_->enabled.end();
        impl_->enabled.erase(first, impl_->enabled.end());
        return removed;
    }
    }
    return false;
}

std::size_t UiBindingTable::Update()
{
    if (!impl_ || !impl_->document)
    {
        return 0;
    }

    std::size_t changed = 0;
    for (TextBinding& binding : impl_->text)
    {
        if (!impl_->document->Element(binding.target))
        {
            continue;
        }
        std::string value = binding.getter();
        if (value != binding.last)
        {
            binding.last = std::move(value);
            if (impl_->document->SetText(binding.target, binding.last))
            {
                ++changed;
            }
        }
    }
    for (BoolBinding& binding : impl_->visible)
    {
        if (!impl_->document->Element(binding.target))
        {
            continue;
        }
        const bool value = binding.getter();
        if (value != binding.last)
        {
            binding.last = value;
            if (impl_->document->SetVisible(binding.target, value))
            {
                ++changed;
            }
        }
    }
    for (BoolBinding& binding : impl_->enabled)
    {
        if (!impl_->document->Element(binding.target))
        {
            continue;
        }
        const bool value = binding.getter();
        if (value != binding.last)
        {
            binding.last = value;
            if (impl_->document->SetEnabled(binding.target, value))
            {
                ++changed;
            }
        }
    }
    return changed;
}

std::size_t UiBindingTable::BindingCount() const noexcept
{
    if (!impl_)
    {
        return 0;
    }
    return impl_->text.size() + impl_->visible.size() + impl_->enabled.size();
}

} // namespace se::ui
