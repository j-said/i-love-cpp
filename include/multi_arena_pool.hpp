#pragma once
#include "free_list.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <stdexcept>
#include <vector>

namespace pp
{
    struct FragmentationStats
    {
        std::size_t total_blocks;
        std::size_t free_blocks;
        std::size_t occupied_blocks;
        std::size_t num_arenas;
        double fragmentation_ratio;
        double avg_free_block_sz;
        double avg_occupied_block_sz;

        struct PerArenaStats
        {
            uint16_t arena_id;
            std::size_t free_count;
            std::size_t occupied_count;
        };
        std::vector<PerArenaStats> per_arena_stats;
    };

    class MultiArenaPool
    {
        struct FreeDeleter
        {
            void operator()(void *p) const noexcept { std::free(p); }
        };

        struct Arena
        {
            std::unique_ptr<std::byte[], FreeDeleter> storage;
            FreeList free_list;
            std::atomic<std::size_t> free_count{0};
            std::size_t block_size;
            std::size_t block_count;
            uint16_t arena_id;
        };

        std::vector<std::unique_ptr<Arena>> arenas_;
        mutable std::shared_mutex arenas_mu_;
        std::size_t block_size_;
        std::size_t initial_block_count_;
        std::size_t max_arenas_;

        static std::size_t round_up(std::size_t n, std::size_t align) noexcept
        {
            return (n + align - 1) & ~(align - 1);
        }

        std::unique_ptr<Arena> create_arena(uint16_t arena_id, std::size_t block_count)
        {
            auto arena = std::make_unique<Arena>();
            arena->block_size = block_size_;
            arena->block_count = block_count;
            arena->arena_id = arena_id;

            void *raw = std::aligned_alloc(alignof(std::max_align_t),
                                           block_size_ * block_count);
            if (!raw)
                throw std::bad_alloc{};
            arena->storage.reset(static_cast<std::byte *>(raw));

            for (std::size_t i = 0; i < block_count; ++i)
            {
                std::byte *block = arena->storage.get() + i * block_size_;
                Node *node = reinterpret_cast<Node *>(block);
                node->arena_id = arena_id;
                arena->free_list.push(block);
            }

            arena->free_count.store(block_count, std::memory_order_relaxed);
            return arena;
        }

        Arena *allocate_from_arena(Arena *arena)
        {
            void *p = arena->free_list.pop();
            if (p)
            {
                arena->free_count.fetch_sub(1, std::memory_order_relaxed);
                return arena;
            }
            return nullptr;
        }

        std::size_t compute_next_block_count(std::size_t last_count) const noexcept
        {
            return last_count == 0 ? initial_block_count_ : last_count * 2;
        }

    public:
        MultiArenaPool(std::size_t block_size, std::size_t initial_block_count,
                       std::size_t max_arenas = 256)
            : block_size_(round_up(
                  std::max(block_size, sizeof(Node)),
                  alignof(std::max_align_t))),
              initial_block_count_(
                  initial_block_count == 0 ? 1 : initial_block_count),
              max_arenas_(max_arenas)
        {
            if (max_arenas_ == 0)
                throw std::invalid_argument("max_arenas must be > 0");

            std::lock_guard lk(arenas_mu_);
            arenas_.push_back(create_arena(0, initial_block_count_));
        }

        MultiArenaPool(const MultiArenaPool &) = delete;
        MultiArenaPool &operator=(const MultiArenaPool &) = delete;
        MultiArenaPool(MultiArenaPool &&) = delete;

        [[nodiscard]] void *allocate()
        {
            // First try: shared lock to read existing arenas
            {
                std::shared_lock lk(arenas_mu_);
                for (auto &arena : arenas_)
                {
                    void *p = arena->free_list.pop();
                    if (p)
                    {
                        arena->free_count.fetch_sub(1,
                                                    std::memory_order_relaxed);
                        return p;
                    }
                }
            }
            // If we get here, all existing arenas are exhausted
            // Release the shared lock and try to expand

            // Second try: unique lock to expand
            {
                std::unique_lock lk(arenas_mu_);

                // Check again in case another thread expanded while we
                // released the lock
                for (auto &arena : arenas_)
                {
                    void *p = arena->free_list.pop();
                    if (p)
                    {
                        arena->free_count.fetch_sub(1,
                                                    std::memory_order_relaxed);
                        return p;
                    }
                }

                // All still exhausted, create new arena
                if (arenas_.size() >= max_arenas_)
                    throw std::bad_alloc{};

                std::size_t next_size = compute_next_block_count(
                    arenas_.back()->block_count);
                uint16_t new_id = static_cast<uint16_t>(arenas_.size());
                arenas_.push_back(create_arena(new_id, next_size));

                void *p = arenas_.back()->free_list.pop();
                if (p)
                {
                    arenas_.back()->free_count.fetch_sub(
                        1, std::memory_order_relaxed);
                    return p;
                }
            }

            throw std::bad_alloc{};
        }

        void deallocate(void *p) noexcept
        {
            if (!p)
                return;

            Node *node = static_cast<Node *>(p);
            uint16_t arena_id = node->arena_id;

            {
                std::shared_lock lk(arenas_mu_);
                if (arena_id < arenas_.size())
                {
                    arenas_[arena_id]->free_list.push(p);
                    arenas_[arena_id]->free_count.fetch_add(
                        1, std::memory_order_relaxed);
                }
            }
        }

        FragmentationStats get_fragmentation_stats() const
        {
            FragmentationStats stats{};
            stats.total_blocks = 0;
            stats.free_blocks = 0;
            stats.occupied_blocks = 0;

            {
                std::shared_lock lk(arenas_mu_);
                stats.num_arenas = arenas_.size();

                for (const auto &arena : arenas_)
                {
                    std::size_t free = arena->free_count.load(
                        std::memory_order_relaxed);
                    std::size_t occupied = arena->block_count - free;

                    stats.total_blocks += arena->block_count;
                    stats.free_blocks += free;
                    stats.occupied_blocks += occupied;

                    FragmentationStats::PerArenaStats per_arena;
                    per_arena.arena_id = arena->arena_id;
                    per_arena.free_count = free;
                    per_arena.occupied_count = occupied;
                    stats.per_arena_stats.push_back(per_arena);
                }
            }

            stats.fragmentation_ratio =
                stats.occupied_blocks > 0
                    ? static_cast<double>(stats.free_blocks) /
                          static_cast<double>(stats.occupied_blocks)
                    : 0.0;

            stats.avg_free_block_sz =
                stats.free_blocks > 0 ? static_cast<double>(block_size_) : 0.0;
            stats.avg_occupied_block_sz = static_cast<double>(block_size_);

            return stats;
        }

        [[nodiscard]] void *reallocate(void *old_ptr, std::size_t new_size)
        {
            if (!old_ptr)
                return allocate();
            if (new_size == 0)
            {
                deallocate(old_ptr);
                return nullptr;
            }
            if (new_size <= block_size_)
                return old_ptr;
            // new_size > block_size_: allocate new block, copy, free old
            void *new_ptr = allocate();
            std::memcpy(new_ptr, old_ptr, block_size_);
            deallocate(old_ptr);
            return new_ptr;
        }

        void compact()
        {
            std::unique_lock lk(arenas_mu_);
            auto it = arenas_.begin();
            while (it != arenas_.end())
            {
                auto &arena = *it;
                if (arena->free_count.load(std::memory_order_relaxed) ==
                    arena->block_count)
                {
                    it = arenas_.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        }

        std::size_t arena_count() const noexcept
        {
            std::shared_lock lk(arenas_mu_);
            return arenas_.size();
        }

        std::size_t block_size() const noexcept { return block_size_; }
        std::size_t total_block_count() const noexcept
        {
            std::shared_lock lk(arenas_mu_);
            std::size_t total = 0;
            for (const auto &arena : arenas_)
                total += arena->block_count;
            return total;
        }
    };
} // namespace pp
