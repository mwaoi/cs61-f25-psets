#include "m61.hh"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cinttypes>
#include <cassert>
#include <unordered_map>
#include <unordered_set>
#include <cstddef>      // defines size_t
#include <limits>

extern "C" void* base_malloc(size_t);
extern "C" void  base_free(void*);

struct alloc_info {
    size_t size;        // user-requested size
    const char* file;   // where allocated
    long line;
};

static std::unordered_map<void*, alloc_info> active;   // replaces active_bytes


static constexpr size_t M61_CANARY_SIZE = 16;    
static constexpr unsigned char M61_CANARY_BYTE = 0xA5;

static std::unordered_set<void*> freed_bases;

static m61_statistics gstats = {
    0, // nactive
    0, // active_size
    0, // ntotal
    0, // total_size
    0, // nfail
    0, // fail_size
    0, // heap_min
    0  // heap_max
};

static inline void update_heap_bounds(void* p, size_t sz) {
    if (!p || sz == 0) return;
    uintptr_t lo = reinterpret_cast<uintptr_t>(p);
    uintptr_t hi = lo + (sz - 1);              // last byte in this block
    if (gstats.heap_min == 0 || lo < gstats.heap_min) gstats.heap_min = lo;
    if (hi > gstats.heap_max) gstats.heap_max = hi;
}

/// m61_malloc(sz, file, line)
///    Return a pointer to `sz` bytes of newly-allocated dynamic memory.
///    The memory is not initialized. If `sz == 0`, then m61_malloc must
///    return a unique, newly-allocated pointer value. The allocation
///    request was at location `file`:`line`.

void* m61_malloc(size_t sz, const char* file, long line) {
    (void) file; (void) line;

    size_t user_sz  = sz;
    size_t alloc_sz = (user_sz == 0 ? 1 : user_sz);

    // NEW: guard against overflow when adding the canary
    if (alloc_sz > std::numeric_limits<size_t>::max() - M61_CANARY_SIZE) {
        gstats.nfail += 1;
        gstats.fail_size += user_sz;
        return nullptr;
    }

    size_t real_sz  = alloc_sz + M61_CANARY_SIZE;

    void* p = base_malloc(real_sz);
    if (p) {
        unsigned char* fence = (unsigned char*) p + user_sz;
        std::memset(fence, M61_CANARY_BYTE, M61_CANARY_SIZE);

        active[p] = alloc_info{ user_sz, file, line };
        gstats.active_size += user_sz;
        gstats.ntotal      += 1;
        gstats.nactive     += 1;
        gstats.total_size  += user_sz;

        freed_bases.erase(p);
        update_heap_bounds(p, user_sz);
    } else {
        gstats.nfail += 1;
        gstats.fail_size += user_sz;
    }
    return p;
}





/// m61_free(ptr, file, line)
///    Free the memory space pointed to by `ptr`, which must have been
///    returned by a previous call to m61_malloc. If `ptr == NULL`,
///    does nothing. The free was called at location `file`:`line`.

void m61_free(void* ptr, const char* file, long line) {
    (void) file; (void) line;

    // 1) free(NULL) is a no-op
    if (!ptr) return;

    // 2) Reject pointers that are outside our heap range
    uintptr_t a = reinterpret_cast<uintptr_t>(ptr);
    if (gstats.heap_min == 0 || a < gstats.heap_min || a > gstats.heap_max) {
        fprintf(stderr,
            "MEMORY BUG: %s:%ld: invalid free of pointer %p, not in heap\n",
            file, line, ptr);
        return;                    // do not touch counters or base_free
    }

    // 3) If we already freed this exact base, it's a double free
    if (freed_bases.find(ptr) != freed_bases.end()) {
        fprintf(stderr,
            "MEMORY BUG: %s:%ld: invalid free of pointer %p, double free\n",
            file, line, ptr);
        return;
    }

// 4) If this is the base of a live allocation, do a normal free (with fence check)
auto it = active.find(ptr);
if (it != active.end()) {
    size_t user_sz = it->second.size;

    // verify the fence (M61_CANARY_SIZE bytes after the user region)
    const unsigned char* fence = (const unsigned char*) ptr + user_sz;
    bool fence_ok = true;
    for (size_t i = 0; i < M61_CANARY_SIZE; ++i) {
        if (fence[i] != M61_CANARY_BYTE) { fence_ok = false; break; }
    }
    if (!fence_ok) {
        // T36 expects no file:line here (just memory bug wild write message)
        fprintf(stderr,
                "MEMORY BUG???: detected wild write during free of pointer %p\n",
                ptr);
    }

    // normal free bookkeeping
    gstats.active_size -= user_sz;
    active.erase(it);
    if (gstats.nactive > 0) --gstats.nactive;
    freed_bases.insert(ptr);
    base_free(ptr);
    return;
}

// 5) Otherwise, if ptr is inside any live block (but not the base) → not allocated
for (const auto& kv : active) {
    uintptr_t base = reinterpret_cast<uintptr_t>(kv.first);
    const alloc_info& ai = kv.second;
    size_t sz = ai.size;
    if (sz > 0) {
        uintptr_t last = base + (sz - 1);
        if (a >= base && a <= last) {
            // Line 1: standard not-allocated error at the free site
            fprintf(stderr,
                "MEMORY BUG: %s:%ld: invalid free of pointer %p, not allocated\n",
                file, line, ptr);

            // Line 2: explanatory line about the interior location
            size_t offset = static_cast<size_t>(a - base);
            // NOTE: two leading spaces, as the tests expect
            fprintf(stderr,
                "  %s:%ld: %p is %zu bytes inside a %zu byte region allocated here\n",
                ai.file, ai.line, ptr, offset, sz);
            return;
        }
    } else {
        // sz == 0: only the exact base would be valid (already handled above)
    }
}

    // 6) Inside heap but not a known base and not interior to any live block
    fprintf(stderr,
            "MEMORY BUG???: invalid free of pointer %p, not allocated\n",
            ptr);
    return;
}





/// m61_calloc(nmemb, sz, file, line)
///    Return a pointer to newly-allocated dynamic memory big enough to
///    hold an array of `nmemb` elements of `sz` bytes each. If `sz == 0`,
///    then must return a unique, newly-allocated pointer value. Returned
///    memory should be initialized to zero. The allocation request was at
///    location `file`:`line`.

void* m61_calloc(size_t nmemb, size_t sz, const char* file, long line) {
    // Overflow check: if nmemb * sz would wrap, fail
    if (sz != 0 && nmemb > std::numeric_limits<size_t>::max() / sz) {
        gstats.nfail += 1;
        gstats.fail_size += (unsigned long long) nmemb * (unsigned long long) sz;
        return nullptr;
    }

    size_t total = nmemb * sz;

    void* p = m61_malloc(total, file, line);
    if (p && total != 0) {
        std::memset(p, 0, total);
    }
    return p;
}


/// m61_get_statistics(stats)
///    Store the current memory statistics in `*stats`.

void m61_get_statistics(m61_statistics* stats) {
    *stats = gstats;   // copy global counters into caller’s struct
}



/// m61_print_statistics()
///    Print the current memory statistics.

void m61_print_statistics() {
    m61_statistics stats;
    m61_get_statistics(&stats);

    printf("alloc count: active %10llu   total %10llu   fail %10llu\n",
           stats.nactive, stats.ntotal, stats.nfail);
    printf("alloc size:  active %10llu   total %10llu   fail %10llu\n",
           stats.active_size, stats.total_size, stats.fail_size);
}


/// m61_print_leak_report()
///    Print a report of all currently-active allocated blocks of dynamic
///    memory.

void m61_print_leak_report() {
    // Print one line per still-active allocation.
    for (const auto& kv : active) {
        void* p = kv.first;
        const alloc_info& ai = kv.second;
        // Exact format the tests expect:
        // "LEAK CHECK: file:line: allocated object %p with size %zu\n"
            printf("LEAK CHECK: %s:%ld: allocated object %p with size %zu\n",
                ai.file, ai.line, p, ai.size);
    }
}


/// m61_print_heavy_hitter_report()
///    Print a report of heavily-used allocation locations.

void m61_print_heavy_hitter_report() {
    // Your heavy-hitters code here
}
