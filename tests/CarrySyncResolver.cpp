#include "core/CarrySyncResolver.h"
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

namespace {
std::vector<unsigned char> memory;
std::map<std::uintptr_t, std::uintptr_t> ranges;
void Check(bool ok, const char* message) {
    if (!ok) {
        std::cerr << "Carry sync resolver failed: " << message << '\n';
        std::abort();
    }
}
bool Range(std::uintptr_t at, std::uintptr_t& end) {
    const auto found = ranges.find(at);
    if (found == ranges.end()) return false;
    end = found->second;
    return true;
}
std::uint32_t Read(std::ifstream& stream) {
    std::uint32_t value = 0;
    stream.read(reinterpret_cast<char*>(&value), sizeof(value));
    Check(bool(stream), "fixture is complete");
    return value;
}
}
void TestCarrySyncResolver(const char* path) {
    std::ifstream stream(path, std::ios::binary);
    Check(bool(stream), "open inspected native function fixture");
    Check(Read(stream) == 0x4353594E, "fixture format");
    const auto updateRva = Read(stream), tableRva = Read(stream), expectedRva = Read(stream), count = Read(stream);
    Check(count == 5, "five inspected native bodies");
    memory.resize(0x800000);
    const auto base = reinterpret_cast<std::uintptr_t>(memory.data());
    std::vector<std::uint32_t> starts;
    for (unsigned i = 0; i < count; ++i) {
        const auto start = Read(stream), size = Read(stream);
        Check(start + size < memory.size() && size <= 0x1000, "bounded native fixture");
        stream.read(reinterpret_cast<char*>(memory.data() + start), size);
        Check(bool(stream), "body exists");
        ranges[base + start] = base + start + size;
        starts.push_back(start);
    }
    auto resolve = [&] {
        return AnimationInjector::CarrySyncResolver::Resolve(base + updateRva, base + tableRva, Range);
    };
    Check(resolve() == base + expectedRva, "find actual registration via mount, prepare and constructor");
    Check(AnimationInjector::CarrySyncResolver::Resolve(base + updateRva, base + tableRva + 8, Range) == 0,
        "wrong native vtable rejected");
    const auto prepare = base + starts[3], savedEnd = ranges[prepare];
    ranges.erase(prepare);
    Check(resolve() == 0, "unreadable prepare rejected");
    ranges[prepare] = savedEnd;
    ranges[prepare] = prepare + 0x1001;
    Check(resolve() == 0, "oversized function rejected");
    ranges[prepare] = savedEnd;
    auto* attach = memory.data() + starts[1];
    const auto attachSize = ranges[base + starts[1]] - (base + starts[1]);
    const unsigned char prefix[]{0xC6, 0x44, 0x24, 0x20, 0x00, 0x4C, 0x8D, 0x0D};
    std::size_t arguments = 0;
    for (std::size_t i = 0; i + sizeof(prefix) < attachSize; ++i)
        if (std::memcmp(attach + i, prefix, sizeof(prefix)) == 0) arguments = i;
    Check(arguments != 0, "mount call arguments in fixture");
    attach[arguments + 4] = 1;
    Check(resolve() == 0, "different physics ABI rejected");
    attach[arguments + 4] = 0;
    const auto originalEnd = ranges[base + starts[1]];
    // An ambiguous extra registration call must not silently choose one.
    std::memcpy(reinterpret_cast<void*>(originalEnd), attach + arguments, 26);
    const auto displacement = static_cast<std::int32_t>(base + expectedRva - (originalEnd + 26));
    std::memcpy(reinterpret_cast<void*>(originalEnd + 22), &displacement, 4);
    ranges[base + starts[1]] = originalEnd + 26;
    Check(resolve() == 0, "ambiguous registration rejected");
    ranges[base + starts[1]] = originalEnd;
    const auto constructor = base + starts[4], constructorEnd = ranges[constructor];
    ranges[constructor] = constructor + 1;
    Check(resolve() == 0, "truncated instruction rejected");
    ranges[constructor] = constructorEnd;
    Check(resolve() == base + expectedRva, "unmodified path resolves again");
    std::cout
        << "Production mount sync resolver: native AE 1.6.1170 path and 6 unsafe/ambiguous path rejections passed\n";
}
