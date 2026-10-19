#pragma once
#include <cstddef>
#include <mutex>
#include <vector>

namespace AnimationInjector {
// Worker threads post items; the main thread takes them all at once in arrival order.
// Posting never blocks on game work, and a full mailbox drops the new item instead of growing.
template<class T>
class BoundedMailbox {
public:
    explicit BoundedMailbox(std::size_t capacity) : _capacity(capacity) {}
    bool Post(T item) {
        std::lock_guard lock(_mutex);
        if (_items.size() >= _capacity) {
            ++_dropped;
            return false;
        }
        _items.push_back(std::move(item));
        return true;
    }
    std::vector<T> TakeAll() {
        std::vector<T> out;
        std::lock_guard lock(_mutex);
        out.swap(_items);
        return out;
    }
    void Clear() {
        std::lock_guard lock(_mutex);
        _items.clear();
    }
    // Number of items dropped since the last call; resets the count.
    std::size_t TakeDropped() {
        std::lock_guard lock(_mutex);
        const auto n = _dropped;
        _dropped = 0;
        return n;
    }

private:
    std::mutex _mutex;
    std::vector<T> _items;
    std::size_t _capacity;
    std::size_t _dropped = 0;
};
}
