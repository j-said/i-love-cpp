# PacketPool — Thread-Safe Memory Allocator & Multi-threaded Server

High-performance lock-free memory allocator with dynamic arena expansion and comprehensive fragmentation analysis.

## Build

```bash
cmake -B build
cmake --build build
```

**Requirements:** C++20, Linux, pthreads

## Executables

| Binary | Description |
|--------|-------------|
| `benchmark` | Single-arena pool vs malloc throughput (tight/batch loops) |
| `benchmark_multi_arena` | Multi-arena expansion, fragmentation, arena lifecycle tests |
| `server_pool` | Thread-pool HTTP/telnet server (8 workers, epoll) |
| `stress` | Load testing for server_pool (configurable clients/requests) |

## Quick Start

### 1. Run allocator benchmarks
```bash
./build/benchmark            # Single-arena baseline
./build/benchmark_multi_arena # Multi-arena tests + stats
```

### 2. Run server
```bash
./build/server_pool
# In another terminal:
./build/stress 127.0.0.1 8080
```

## Architecture

### PacketPool (Single-Arena)
- Fixed-size pre-allocated heap
- Lock-free LIFO free list with ABA tagging
- O(1) alloc/dealloc without mutex
- Used by server_pool for buffer management

### MultiArenaPool (Dynamic)
- Grows exponentially: 2→4→8→... blocks per arena
- Per-arena lock-free FreeList + shared_mutex on arena vector
- Arena removal on complete deallocation
- Fragmentation metrics: ratio, per-arena stats

### STL Integration
- `PoolAllocator<T>` for PacketPool
- `MultiArenaAllocator<T>` for MultiArenaPool
- Works with `std::vector`, `std::list`, etc.

## Key Features

✓ **Lock-Free Allocation** — No mutex in hot path (except arena expansion)  
✓ **Dynamic Expansion** — Creates arenas on-demand  
✓ **Fragmentation Analysis** — Tracks free/occupied ratio, avg sizes  
✓ **Thread-Safe** — Atomic operations + shared_mutex coordination  
✓ **STL Compatible** — Custom allocator adapters included  

## Testing

All tests included in `benchmark_multi_arena`:
- Correctness (smoke test with arena expansion)
- Fragmentation under random deallocation
- Arena lifecycle (creation/removal)
- Multi-threaded stress (8 threads, 8000 allocs)
- STL vector integration
- Performance comparison for the multi-arena allocator

Run all tests: `./build/benchmark_multi_arena`

## Performance (Sample Results)

**Multi-threaded (8 threads, tight alloc/dealloc):**
- PacketPool: ~5.6M allocs/sec (after arena expansion)
- malloc: Varies (thread contention dependent)

**Fragmentation (100 blocks allocated):**
- Initial: 20% fragmentation
- After 50% deallocation: 140% fragmentation
- After compact: 0% (all arenas reclaimed)

## Files

- `include/packet_pool.hpp` — Single-arena allocator
- `include/multi_arena_pool.hpp` — Dynamic multi-arena pool
- `include/pool_allocator.hpp` — STL adapter for PacketPool
- `include/multi_arena_allocator.hpp` — STL adapter for MultiArenaPool
- `include/free_list.hpp` — Lock-free LIFO list with ABA tagging
- `src/server_pool.cpp` — Thread-pool server (8 workers + epoll)
- `src/benchmark.cpp` — Single-arena vs malloc comparison
- `src/benchmark_multi_arena.cpp` — Multi-arena test suite + performance comparison
- `src/stress.cpp` — Server load generator

## Notes

- Block sizes are uniform and rounded up to max_align_t alignment
- Arena IDs encoded in Node headers for O(1) deallocation lookup
- Server uses PacketPool for per-worker buffer slots (never exhausts)
- Fragmentation ratio = free_blocks / occupied_blocks
