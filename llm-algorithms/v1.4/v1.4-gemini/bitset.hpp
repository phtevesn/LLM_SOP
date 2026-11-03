#ifndef SOP_BITSET_HPP
#define SOP_BITSET_HPP

#include <cstdint>
#include <cstring>

namespace sop {

static constexpr int MAX_NODES = 1024;
static constexpr int MAX_WORDS = MAX_NODES / 64; // 16 words of 64 bits
static constexpr int64_t INF = 1000000000000LL;  // 10^12 (safe against overflow when doubled)

/**
 * @brief 64-byte cache-line-aligned 1024-bit fixed-capacity bitset for fast
 *        precedence closure, readiness subset checks, and DP/B&B state dominance.
 */
struct alignas(64) Bitset1024 {
    uint64_t w[MAX_WORDS];

    Bitset1024() noexcept { std::memset(w, 0, sizeof(w)); }

    inline void clear() noexcept { std::memset(w, 0, sizeof(w)); }
    inline void set(int i) noexcept { w[i >> 6] |= (1ULL << (i & 63)); }
    inline void reset(int i) noexcept { w[i >> 6] &= ~(1ULL << (i & 63)); }
    inline bool test(int i) const noexcept { return (w[i >> 6] >> (i & 63)) & 1ULL; }

    inline bool empty(int nwords) const noexcept {
        for (int i = 0; i < nwords; ++i) {
            if (w[i] != 0ULL) return false;
        }
        return true;
    }

    inline bool is_subset_of(const Bitset1024& o, int nwords) const noexcept {
        for (int i = 0; i < nwords; ++i) {
            if (w[i] & ~o.w[i]) return false;
        }
        return true;
    }

    inline bool intersects(const Bitset1024& o, int nwords) const noexcept {
        for (int i = 0; i < nwords; ++i) {
            if (w[i] & o.w[i]) return true;
        }
        return false;
    }

    inline void bit_or(const Bitset1024& o, int nwords) noexcept {
        for (int i = 0; i < nwords; ++i) {
            w[i] |= o.w[i];
        }
    }

    inline int count(int nwords) const noexcept {
        int c = 0;
        for (int i = 0; i < nwords; ++i) {
            c += __builtin_popcountll(w[i]);
        }
        return c;
    }

    inline bool equals(const Bitset1024& o, int nwords) const noexcept {
        for (int i = 0; i < nwords; ++i) {
            if (w[i] != o.w[i]) return false;
        }
        return true;
    }
};

struct BitsetHasher {
    inline size_t operator()(const Bitset1024& b) const noexcept {
        uint64_t h = 0x9e3779b97f4a7c15ULL;
        for (int i = 0; i < MAX_WORDS; ++i) {
            if (b.w[i]) {
                uint64_t x = b.w[i];
                x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
                x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
                x = x ^ (x >> 31);
                h ^= x + 0x9e3779b97f4a7c15ULL * (i + 1) + (h << 6) + (h >> 2);
            }
        }
        return static_cast<size_t>(h);
    }
};

struct BitsetEqual {
    inline bool operator()(const Bitset1024& a, const Bitset1024& b) const noexcept {
        return std::memcmp(a.w, b.w, sizeof(a.w)) == 0;
    }
};

} // namespace sop

#endif // SOP_BITSET_HPP