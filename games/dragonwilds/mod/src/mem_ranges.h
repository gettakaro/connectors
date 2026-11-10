#pragma once
// Address-range checks over a parsed /proc/self/maps, kept free of engine code so the unit tests
// can drive them with synthetic layouts.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct MapRange {
    uintptr_t lo, hi;
    bool r, w;
    std::string path;
};

// True when [a, a + len) is readable (and writable, if asked) across `ranges`, sorted by `lo`.
// The span may cross several mappings as long as they are back to back: swapping one vtable slot
// re-protects its page, which splits a .rodata mapping in two, and a vtable that straddles that
// page boundary must still count as readable.
inline bool RangesCover(const std::vector<MapRange>& ranges, uintptr_t a, size_t len, bool needWrite) {
    if (!len || a + len < a) return false;
    auto it = std::upper_bound(ranges.begin(), ranges.end(), a,
                               [](uintptr_t x, const MapRange& m) { return x < m.lo; });
    if (it == ranges.begin()) return false;
    --it;
    const uintptr_t end = a + len;
    uintptr_t at = a;
    for (; it != ranges.end(); ++it) {
        if (at < it->lo || at >= it->hi || !it->r || (needWrite && !it->w)) return false;
        if (end <= it->hi) return true;
        at = it->hi;
    }
    return false;
}
