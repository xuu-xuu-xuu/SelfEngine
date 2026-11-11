#include "FunctionLayer/UI/Core/UiElement.h"

namespace se::ui
{

static_assert(sizeof(UiElementHandle) == sizeof(std::uint32_t) * 2,
              "UiElementHandle must remain two 32-bit fields");
static_assert(alignof(UiElementHandle) == alignof(std::uint32_t),
              "UiElementHandle must remain tightly aligned");

} // namespace se::ui
