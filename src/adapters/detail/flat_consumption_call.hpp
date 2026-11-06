#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace synth::adapters::detail {

// Flat 1.11.240 instant-cast arguments, reconstructed by task 0x7E.
// The engine applies effects and then calls finished(item, source, stack_index).
// This is a stack-owned synchronous call, never a queued task.
template <class Item, class Reference>
struct FlatConsumptionCall final {
    using Finished = void (*)(Item*, Reference*, std::uint32_t);
    Item* item{};
    Reference* source{};
    void* unused{};
    Finished finished{};
    std::uint32_t stack_index{};
    bool flag{};
};

using FlatConsumptionLayout = FlatConsumptionCall<void, void>;
static_assert(std::is_standard_layout_v<FlatConsumptionLayout>);
static_assert(sizeof(FlatConsumptionLayout) == 0x28);
static_assert(offsetof(FlatConsumptionLayout, item) == 0x00);
static_assert(offsetof(FlatConsumptionLayout, source) == 0x08);
static_assert(offsetof(FlatConsumptionLayout, unused) == 0x10);
static_assert(offsetof(FlatConsumptionLayout, finished) == 0x18);
static_assert(offsetof(FlatConsumptionLayout, stack_index) == 0x20);
static_assert(offsetof(FlatConsumptionLayout, flag) == 0x24);

} // namespace synth::adapters::detail
