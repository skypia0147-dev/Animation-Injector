#pragma once
#include <array>
#include <cstddef>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

namespace AnimationInjector {
// One instance belongs to one synchronous graph operation. Never retain it
// across engine loading calls. Child order and the original DFS limits remain.
template<class Node, class Child, class ReadName, class ReadChildren>
class BoundedNodeSearch {
public:
    Node* Find(Node* root, const char* name, int depth = 0) {
        seen.clear();
        return name ? Visit(root, name, depth) : nullptr;
    }
    // Only native templates in an append-only injection phase may use this.
    // Misses are not cached: a later action may add the requested node.
    Node* FindNative(Node* root, const char* name, int depth = 0) {
        if (!root || !name) return nullptr;
        for (const auto& item : nativeHits)
            if (item.root == root && item.depth == depth && item.name == name) {
                ++cacheHits;
                return item.result;
            }
        auto* result = Find(root, name, depth);
        if (result) nativeHits.push_back({root, name, depth, result});
        return result;
    }
    std::size_t LastVisited() const { return seen.size(); }
    std::size_t visits = 0, cacheHits = 0, bufferGrowths = 0;

private:
    struct Hit {
        Node* root;
        std::string name;
        int depth;
        Node* result;
    };
    Node* Visit(Node* node, const char* name, int depth) {
        if (!node || depth < 0 || depth > 24 || seen.size() >= 8192 || !seen.insert(node).second) return nullptr;
        ++visits;
        const char* actual = ReadName{}(node);
        if (actual && _stricmp(actual, name) == 0) return node;
        auto& buffer = buffers[depth];
        const auto oldCapacity = buffer.capacity();
        const int count = ReadChildren{}(node, buffer);
        if (buffer.capacity() > oldCapacity) ++bufferGrowths;
        if (count < 0 || count > 4096 || static_cast<std::size_t>(count) > buffer.size()) return nullptr;
        for (int i = 0; i < count; ++i)
            if (auto* result = Visit(buffer[i].node, name, depth + 1)) return result;
        return nullptr;
    }
    std::array<std::vector<Child>, 25> buffers;
    std::unordered_set<Node*> seen;
    std::vector<Hit> nativeHits;
};
}
