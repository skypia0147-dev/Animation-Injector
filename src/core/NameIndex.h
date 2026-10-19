#pragma once
#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

namespace AnimationInjector {
// Match CRT byte-wise case folding used by the existing _stricmp lookups.
// Own keys: a Havok array may relocate when names are appended.
struct NameHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view value) const noexcept {
        std::size_t hash = 14695981039346656037ull;
        for (unsigned char c : value) {
            hash ^= static_cast<unsigned char>(std::tolower(c));
            hash *= 1099511628211ull;
        }
        return hash;
    }
};
struct NameEqual {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
                return false;
        return true;
    }
};
class NameIndex {
public:
    template<class Read>
    void Reset(int size, Read read) {
        entries.clear();
        entries.reserve(static_cast<std::size_t>(size > 0 ? size : 0));
        for (int i = 0; i < size; ++i) Add(read(i), i);
    }
    void Add(const char* name, int index) {
        // emplace preserves the FIRST matching ID, including duplicate names.
        if (name) entries.try_emplace(name, index);
    }
    int Find(const char* name) const {
        if (!name) return -1;
        const auto it = entries.find(std::string_view(name));
        return it == entries.end() ? -1 : it->second;
    }

private:
    std::unordered_map<std::string, int, NameHash, NameEqual> entries;
};

class NameTableLookup {
public:
    template<class Read>
    int Find(const void* data, int size, Read read, const char* name) {
        if (!name) return -1;
        if (!data) size = 0;
        // Small actions should not pay to hash an entire native string table.
        if (!indexed && linearQueries++ < 16) {
            for (int i = 0; i < size; ++i) {
                const char* value = read(i);
                if (value && _stricmp(value, name) == 0) return i;
            }
            return -1;
        }
        if (!indexed || indexedData != data || indexedSize != size) {
            index.Reset(size, read);
            indexed = true;
            indexedData = data;
            indexedSize = size;
            ++builds;
        }
        return index.Find(name);
    }
    void Appended(const void* data, int size, const char* name) {
        if (!indexed) return;
        if (indexedSize != size - 1) {
            indexed = false;
            return;
        }
        index.Add(name, size - 1);
        indexedData = data;
        indexedSize = size;
    }
    std::size_t builds = 0;

private:
    NameIndex index;
    const void* indexedData = nullptr;
    int indexedSize = -1, linearQueries = 0;
    bool indexed = false;
};
}
