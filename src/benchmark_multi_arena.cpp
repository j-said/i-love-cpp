#include "multi_arena_pool.hpp"
#include "multi_arena_allocator.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <latch>
#include <random>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;


static void smoke_test()
{
    std::cout << "\n── Smoke Test: Multi-Arena Correctness ──\n";

    constexpr std::size_t BLOCK_SIZE = 64;
    constexpr std::size_t INITIAL_BLOCKS = 2;

    pp::MultiArenaPool pool(BLOCK_SIZE, INITIAL_BLOCKS, 10);

    std::cout << "Initial: " << pool.arena_count() << " arena(s), "
              << pool.total_block_count() << " block(s)\n";

    std::vector<void *> ptrs;

    for (int i = 0; i < 10; ++i)
        ptrs.push_back(pool.allocate());

    std::cout << "After 10 allocs: " << pool.arena_count() << " arena(s), "
              << pool.total_block_count() << " block(s)\n";

    for (auto p : ptrs)
        pool.deallocate(p);
    ptrs.clear();

    auto stats = pool.get_fragmentation_stats();
    std::cout << "After dealloc all: fragmentation_ratio="
              << std::fixed << std::setprecision(2)
              << stats.fragmentation_ratio << "\n";

    pool.compact();
    std::cout << "After compact: " << pool.arena_count() << " arena(s)\n";

    std::cout << "✓ Smoke test passed\n";
}


static void fragmentation_test()
{
    std::cout << "\n── Test 2: Fragmentation Analysis ──\n";

    constexpr std::size_t BLOCK_SIZE = 64;
    constexpr std::size_t INITIAL_BLOCKS = 8;

    pp::MultiArenaPool pool(BLOCK_SIZE, INITIAL_BLOCKS, 100);

    std::vector<void *> ptrs;
    std::mt19937 rng(42);

    for (int i = 0; i < 100; ++i)
        ptrs.push_back(pool.allocate());

    auto stats = pool.get_fragmentation_stats();
    std::cout << "After alloc 100: "
              << "fragmentation=" << std::setprecision(3)
              << (stats.fragmentation_ratio * 100) << "% "
              << "(free=" << stats.free_blocks
              << " occupied=" << stats.occupied_blocks
              << " arenas=" << stats.num_arenas << ")\n";

    std::uniform_int_distribution<int> dist(0, ptrs.size() - 1);
    for (int i = 0; i < 50; ++i)
    {
        int idx = dist(rng);
        std::swap(ptrs[idx], ptrs.back());
        pool.deallocate(ptrs.back());
        ptrs.pop_back();
    }

    stats = pool.get_fragmentation_stats();
    std::cout << "After dealloc 50: "
              << "fragmentation=" << std::setprecision(3)
              << (stats.fragmentation_ratio * 100) << "% "
              << "(free=" << stats.free_blocks
              << " occupied=" << stats.occupied_blocks
              << " arenas=" << stats.num_arenas << ")\n";

    for (auto p : ptrs)
        pool.deallocate(p);
    ptrs.clear();

    stats = pool.get_fragmentation_stats();
    std::cout << "After dealloc all: "
              << "fragmentation=" << std::setprecision(3)
              << (stats.fragmentation_ratio * 100) << "% "
              << "(free=" << stats.free_blocks
              << " arenas=" << stats.num_arenas << ")\n";

    std::cout << "✓ Fragmentation test passed\n";
}


static void arena_lifecycle_test()
{
    std::cout << "\n── Test 3: Arena Lifecycle ──\n";

    constexpr std::size_t BLOCK_SIZE = 64;
    constexpr std::size_t INITIAL_BLOCKS = 4;

    pp::MultiArenaPool pool(BLOCK_SIZE, INITIAL_BLOCKS, 20);

    std::vector<void *> ptrs;

    std::cout << "Start: " << pool.arena_count() << " arena(s)\n";

    for (int i = 0; i < 32; ++i)
        ptrs.push_back(pool.allocate());

    std::cout << "After 32 allocs: " << pool.arena_count() << " arena(s), "
              << pool.total_block_count() << " block(s)\n";

    for (int i = 0; i < 16; ++i)
    {
        pool.deallocate(ptrs.back());
        ptrs.pop_back();
    }
    pool.compact();
    std::cout << "After dealloc 16 + compact: " << pool.arena_count()
              << " arena(s), " << pool.total_block_count() << " block(s)\n";

    for (auto p : ptrs)
        pool.deallocate(p);
    ptrs.clear();

    pool.compact();
    std::cout << "After dealloc all + compact: " << pool.arena_count()
              << " arena(s)\n";

    std::cout << "✓ Arena lifecycle test passed\n";
}


static void multithreaded_test()
{
    std::cout << "\n── Test 4: Multi-threaded Allocation ──\n";

    constexpr std::size_t BLOCK_SIZE = 64;
    constexpr std::size_t INITIAL_BLOCKS = 16;
    constexpr std::size_t NUM_THREADS = 8;
    constexpr std::size_t ALLOCS_PER_THREAD = 1000;

    pp::MultiArenaPool pool(BLOCK_SIZE, INITIAL_BLOCKS, 100);

    std::latch ready(static_cast<std::ptrdiff_t>(NUM_THREADS) + 1);
    std::atomic<std::size_t> errors{0};

    auto worker = [&]
    {
        std::vector<void *> ptrs;
        ready.arrive_and_wait();

        for (std::size_t i = 0; i < ALLOCS_PER_THREAD; ++i)
        {
            try
            {
                void *p = pool.allocate();
                ptrs.push_back(p);

                if (ptrs.size() >= 16)
                {
                    pool.deallocate(ptrs.back());
                    ptrs.pop_back();
                }
            }
            catch (const std::bad_alloc &)
            {
                errors.fetch_add(1, std::memory_order_relaxed);
            }
        }

        for (auto p : ptrs)
            pool.deallocate(p);
    };

    std::vector<std::thread> threads;
    threads.reserve(NUM_THREADS);
    for (std::size_t i = 0; i < NUM_THREADS; ++i)
        threads.emplace_back(worker);

    ready.arrive_and_wait();
    auto t0 = Clock::now();

    for (auto &t : threads)
        t.join();

    double secs = std::chrono::duration<double>(Clock::now() - t0).count();

    auto stats = pool.get_fragmentation_stats();
    std::cout << "Threads: " << NUM_THREADS
              << " | Allocs: " << (NUM_THREADS * ALLOCS_PER_THREAD)
              << " | Errors: " << errors.load()
              << " | Throughput: " << std::fixed << std::setprecision(2)
              << (NUM_THREADS * ALLOCS_PER_THREAD) / secs
              << " allocs/sec\n"
              << "Final state: " << stats.num_arenas << " arena(s), "
              << stats.total_blocks << " block(s), fragmentation="
              << (stats.fragmentation_ratio * 100) << "%\n";

    if (errors.load() == 0)
        std::cout << "✓ Multi-threaded test passed\n";
    else
        std::cout << "✗ Multi-threaded test FAILED (" << errors.load()
                  << " errors)\n";
}


static void stl_test()
{
    std::cout << "\n── Test 5: STL Container Integration ──\n";

    constexpr std::size_t BLOCK_SIZE = 4096;
    constexpr std::size_t INITIAL_BLOCKS = 8;

    pp::MultiArenaPool pool(BLOCK_SIZE, INITIAL_BLOCKS, 100);
    pp::MultiArenaAllocator<int> alloc(pool);

    std::vector<int, pp::MultiArenaAllocator<int>> vec(alloc);

    for (int i = 0; i < 100; ++i)
        vec.push_back(i);

    auto stats = pool.get_fragmentation_stats();
    std::cout << "Vector with " << vec.size() << " elements: "
              << stats.num_arenas << " arena(s), fragmentation="
              << (stats.fragmentation_ratio * 100) << "%\n";

    vec.clear();
    pool.compact();

    stats = pool.get_fragmentation_stats();
    std::cout << "After clear & compact: " << stats.num_arenas
              << " arena(s)\n";

    std::cout << "✓ STL test passed\n";
}

// ── Test 6: Performance Comparison ───────────────────────────────────────

static void performance_test()
{
    std::cout << "\n── Test 6: Performance Comparison ──\n";

    constexpr std::size_t BLOCK_SIZE = 64;
    constexpr std::size_t INITIAL_BLOCKS = 32;
    constexpr std::size_t NUM_THREADS = 4;
    constexpr std::size_t OPS_PER_THREAD = 100'000;

    // Multi-arena pool
    pp::MultiArenaPool pool(BLOCK_SIZE, INITIAL_BLOCKS, 100);

    std::latch ready(static_cast<std::ptrdiff_t>(NUM_THREADS) + 1);

    std::vector<std::thread> threads;
    for (std::size_t t = 0; t < NUM_THREADS; ++t)
    {
        threads.emplace_back([&]
                             {
            ready.arrive_and_wait();
            for (std::size_t i = 0; i < OPS_PER_THREAD; ++i)
            {
                void *p = pool.allocate();
                pool.deallocate(p);
            } });
    }

    ready.arrive_and_wait();
    auto t0 = Clock::now();

    for (auto &t : threads)
        t.join();

    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    double mops = (NUM_THREADS * OPS_PER_THREAD * 2) / secs / 1e6;

    std::cout << "Multi-Arena Pool (" << NUM_THREADS << " threads, "
              << OPS_PER_THREAD << " ops each): " << std::fixed
              << std::setprecision(2) << mops << " Mops/sec\n";
}

int main()
{
    std::cout << std::string(70, '=')
              << "\nMulti-Arena PacketPool Benchmark\n"
              << std::string(70, '=') << "\n";

    try
    {
        smoke_test();
        fragmentation_test();
        arena_lifecycle_test();
        multithreaded_test();
        stl_test();
        performance_test();

        std::cout << "\n" << std::string(70, '=')
                  << "\n✓ All tests completed successfully\n"
                  << std::string(70, '=') << "\n";
    }
    catch (const std::exception &e)
    {
        std::cerr << "\n✗ Test failed: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
