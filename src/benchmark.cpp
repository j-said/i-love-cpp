#include "packet_pool.hpp"
#include "pool_allocator.hpp"
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <latch>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

struct BenchResult
{
    std::size_t threads;
    std::size_t ops_per_thread;
    double pool_mops; // million ops/sec
    double malloc_mops;
};

// ── pool benchmark ──────────────────────────────────────────────────────────

static double bench_pool(std::size_t n_threads, std::size_t ops,
                         std::size_t block_size)
{
    // Pool sized to allow all threads to hold one block simultaneously
    pp::PacketPool pool(block_size, n_threads * 2);

    std::latch ready(static_cast<std::ptrdiff_t>(n_threads) + 1);
    std::vector<std::thread> threads;
    threads.reserve(n_threads);

    for (std::size_t t = 0; t < n_threads; ++t)
    {
        threads.emplace_back([&]
                             {
            ready.arrive_and_wait();
            for (std::size_t i = 0; i < ops; ++i) {
                void* p = pool.allocate();
                pool.deallocate(p);
            } });
    }

    ready.arrive_and_wait();
    auto t0 = Clock::now();

    for (auto &th : threads)
        th.join();

    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(n_threads * ops) / secs / 1e6;
}

// ── malloc benchmark ─────────────────────────────────────────────────────────

static double bench_malloc(std::size_t n_threads, std::size_t ops,
                           std::size_t block_size)
{
    std::latch ready(static_cast<std::ptrdiff_t>(n_threads) + 1);
    std::vector<std::thread> threads;
    threads.reserve(n_threads);

    for (std::size_t t = 0; t < n_threads; ++t)
    {
        threads.emplace_back([&]
                             {
            ready.arrive_and_wait();
            for (std::size_t i = 0; i < ops; ++i) {
                void* p = std::malloc(block_size);
                // prevent the compiler from eliding the allocation
                asm volatile("" : : "r"(p) : "memory");
                std::free(p);
            } });
    }

    ready.arrive_and_wait();
    auto t0 = Clock::now();

    for (auto &th : threads)
        th.join();

    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(n_threads * ops) / secs / 1e6;
}

// ── STL allocator smoke-test ─────────────────────────────────────────────────

static void stl_smoke_test()
{
    // Block large enough for a std::vector node (or just raw bytes)
    constexpr std::size_t BLOCK = 64;
    constexpr std::size_t COUNT = 256;

    pp::PacketPool pool(BLOCK, COUNT);
    pp::PoolAllocator<std::byte> alloc(pool);

    // Allocate / deallocate round-trip
    auto *p = alloc.allocate(1);
    alloc.deallocate(p, 1);

    // bad_alloc on n*sizeof(T) > block_size
    bool threw = false;
    try
    {
        alloc.allocate(COUNT + 1);
    }
    catch (const std::bad_alloc &)
    {
        threw = true;
    }

    // bad_alloc on pool exhaustion
    std::vector<std::byte *> held;
    held.reserve(COUNT);
    try
    {
        for (std::size_t i = 0; i <= COUNT; ++i)
            held.push_back(alloc.allocate(1));
    }
    catch (const std::bad_alloc &)
    {
        threw = true;
    }
    for (auto *q : held)
        alloc.deallocate(q, 1);

    std::cout << "STL smoke-test: " << (threw ? "PASS" : "FAIL") << "\n\n";
}

// ── batch benchmark (alloc N blocks, use them, free all) ─────────────────────
//
// Closer to real workloads: contention is spread over time rather than
// hammering the CAS head_ in a tight single-block loop.

static double bench_pool_batch(std::size_t n_threads, std::size_t rounds,
                               std::size_t batch, std::size_t block_size)
{
    pp::PacketPool pool(block_size, n_threads * batch);
    std::latch ready(static_cast<std::ptrdiff_t>(n_threads) + 1);
    std::vector<std::thread> threads;
    threads.reserve(n_threads);

    for (std::size_t t = 0; t < n_threads; ++t)
    {
        threads.emplace_back([&]
                             {
            std::vector<void*> ptrs(batch);
            ready.arrive_and_wait();
            for (std::size_t r = 0; r < rounds; ++r) {
                for (auto& p : ptrs) p = pool.allocate();
                for (auto  p : ptrs) pool.deallocate(p);
            } });
    }

    ready.arrive_and_wait();
    auto t0 = Clock::now();
    for (auto &th : threads)
        th.join();
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(n_threads * rounds * batch * 2) / secs / 1e6;
}

static double bench_malloc_batch(std::size_t n_threads, std::size_t rounds,
                                 std::size_t batch, std::size_t block_size)
{
    std::latch ready(static_cast<std::ptrdiff_t>(n_threads) + 1);
    std::vector<std::thread> threads;
    threads.reserve(n_threads);

    for (std::size_t t = 0; t < n_threads; ++t)
    {
        threads.emplace_back([&]
                             {
            std::vector<void*> ptrs(batch);
            ready.arrive_and_wait();
            for (std::size_t r = 0; r < rounds; ++r) {
                for (auto& p : ptrs) { p = std::malloc(block_size); asm volatile(""::"r"(p):"memory"); }
                for (auto  p : ptrs) std::free(p);
            } });
    }

    ready.arrive_and_wait();
    auto t0 = Clock::now();
    for (auto &th : threads)
        th.join();
    double secs = std::chrono::duration<double>(Clock::now() - t0).count();
    return static_cast<double>(n_threads * rounds * batch * 2) / secs / 1e6;
}

// ── main ─────────────────────────────────────────────────────────────────────

int main()
{
    stl_smoke_test();

    constexpr std::size_t BLOCK_SIZE = 64;
    constexpr std::size_t OPS = 1'000'000;
    const std::size_t THREADS[] = {1, 2, 4, 8};

    auto header = [](const char *title)
    {
        std::cout << "\n── " << title << " ──\n"
                  << std::left
                  << std::setw(10) << "Threads"
                  << std::setw(18) << "Pool (Mops/s)"
                  << std::setw(18) << "malloc (Mops/s)"
                  << "Speedup\n"
                  << std::string(54, '-') << "\n";
    };

    header("Tight loop: alloc + immediate free (worst-case CAS contention)");
    for (std::size_t nt : THREADS)
    {
        double pm = bench_pool(nt, OPS, BLOCK_SIZE);
        double mm = bench_malloc(nt, OPS, BLOCK_SIZE);
        std::cout << std::left << std::setw(10) << nt
                  << std::setw(18) << std::fixed << std::setprecision(2) << pm
                  << std::setw(18) << mm
                  << std::setprecision(2) << (pm / mm) << "x\n";
    }

    header("Batch: alloc 32 blocks, free 32 blocks (realistic workload)");
    for (std::size_t nt : THREADS)
    {
        double pm = bench_pool_batch(nt, OPS / 32, 32, BLOCK_SIZE);
        double mm = bench_malloc_batch(nt, OPS / 32, 32, BLOCK_SIZE);
        std::cout << std::left << std::setw(10) << nt
                  << std::setw(18) << std::fixed << std::setprecision(2) << pm
                  << std::setw(18) << mm
                  << std::setprecision(2) << (pm / mm) << "x\n";
    }

    return 0;
}
