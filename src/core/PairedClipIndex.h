#pragma once
#include <cstdint>
#include <optional>

namespace AnimationInjector::PairedClipIndex {
inline std::optional<std::uint16_t> Resolve(
    int index, int bindingCount, bool slot, bool ours, bool active, bool replacement) {
    if (!ours || active || replacement || !slot || index < 0 || index >= bindingCount || index >= 0xFFFF)
        return std::nullopt;
    return static_cast<std::uint16_t>(index);
}
class Borrow {
public:
    Borrow() = default;
    Borrow(const Borrow&) = delete;
    Borrow& operator=(const Borrow&) = delete;
    void Set(std::uint16_t& value, std::uint16_t desired) {
        if (field || value == desired) return;
        field = &value;
        original = value;
        value = desired;
    }
    ~Borrow() {
        if (field) *field = original;
    }

private:
    std::uint16_t* field = nullptr;
    std::uint16_t original = 0;
};
}
