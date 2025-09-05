#include "m61.hh"
#include <cstdio>
#include <cassert>
#include <cstring>
// Check diabolical m61_calloc.

int main() {
    size_t very_large = (size_t) -1 / 8 + 2;
    void* p = m61_calloc(very_large, 16);
    assert(p == nullptr);

    void* p2 = m61_calloc(16, very_large);
    assert(p2 == nullptr);

    m61_print_statistics();
}

//! alloc count: active          0   total          0   fail          2
//! alloc size:  active          0   total          0   fail        ???
