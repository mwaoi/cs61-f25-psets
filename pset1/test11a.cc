#include "m61.hh"
#include <cstdio>
// The distance from heap_min to heap_max should be big enough to hold
// any block that was allocated.

int main() {
    void *p = m61_malloc(17);
    assert(p);
    m61_free(p);

    p = m61_malloc(19);
    assert(p);
    m61_free(p);

    m61_statistics stats = m61_get_statistics();
    assert(stats.heap_max - stats.heap_min >= 18);
}
