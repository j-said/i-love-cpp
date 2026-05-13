#pragma once

#include "free_list.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <thread>
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
            std::uint64_t arena_uid;
            std::thread::id owner_thread;
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

        struct BlockHeader
        {
            std::uint64_t arena_uid;
        };

        struct Arena
        {
            std::unique_ptr<std::byte[], FreeDeleter> storage;
            FreeList free_list;
            mutable std::shared_mutex mu;
            std::atomic<std::size_t> occupied_count{0};
            std::size_t block_count{0};
            std::size_t block_stride{0};
            std::uint64_t arena_uid{0};
            std::thread::id owner_thread{};
        };

        static constexpr std::size_t MAX_ARENAS = 16;
        inline static thread_local std::uint64_t tls_arena_uid_ = 0;

        std::vector<std::unique_ptr<Arena>> arenas_;
        mutable std::shared_mutex arenas_mu_;
        std::size_t block_size_;
        std::size_t block_header_size_;
        std::size_t block_stride_;
        std::size_t initial_block_count_;
        std::size_t max_arenas_;
        std::atomic<std::uint64_t> next_arena_uid_{1};

        static std::size_t round_up(std::size_t n, std::size_t align) noexcept
        {
            return (n + align - 1) & ~(align - 1);
        }

        std::size_t next_block_count(std::size_t last_count) const noexcept
        {
            return last_count == 0 ? initial_block_count_ : last_count * 2;
        }

        Arena *find_arena_by_uid_unlocked(std::uint64_t uid) const noexcept
        {
            for (const auto &arena : arenas_)
            {
                if (arena && arena->arena_uid == uid)
                    return arena.get();
            }
            return nullptr;
        }

        std::vector<std::unique_ptr<Arena>>::iterator find_arena_it_unlocked(
            std::uint64_t uid)
        {
            return std::find_if(
                arenas_.begin(), arenas_.end(),
                [uid](const std::unique_ptr<Arena> &arena)
                {
                    return arena && arena->arena_uid == uid;
                });
        }

        Arena *create_arena_unlocked(std::size_t block_count,
                                     std::thread::id owner_thread)
        {
            if (arenas_.size() >= max_arenas_)
                throw std::bad_alloc{};

            auto arena = std::make_unique<Arena>();
            arena->block_count = block_count;
            arena->block_stride = block_stride_;
            arena->arena_uid = next_arena_uid_.fetch_add(1, std::memory_order_relaxed);
            arena->owner_thread = owner_thread;

            void *raw = std::aligned_alloc(alignof(std::max_align_t),
                                           arena->block_stride * block_count);
            if (!raw)
                throw std::bad_alloc{};
            arena->storage.reset(static_cast<std::byte *>(raw));

            for (std::size_t i = 0; i < block_count; ++i)
            {
                std::byte *base = arena->storage.get() + i * arena->block_stride;
                auto *header = reinterpret_cast<BlockHeader *>(base);
                header->arena_uid = arena->arena_uid;
                arena->free_list.push(base);
            }

            arena->occupied_count.store(0, std::memory_order_relaxed);

            Arena *result = arena.get();
            arenas_.push_back(std::move(arena));
            return result;
        }

        void *try_allocate_from_arena(Arena &arena)
        {
            std::shared_lock arena_lock(arena.mu);
            void *base = arena.free_list.pop();
            if (!base)
                return nullptr;

            arena.occupied_count.fetch_add(1, std::memory_order_relaxed);
            auto *header = static_cast<BlockHeader *>(base);
            header->arena_uid = arena.arena_uid;
            return reinterpret_cast<std::byte *>(base) + block_header_size_;
        }

        void remove_arena_if_empty(std::uint64_t arena_uid)
        {
            std::unique_lock global_lock(arenas_mu_);
            auto it = find_arena_it_unlocked(arena_uid);
            if (it == arenas_.end())
                return;

            Arena &arena = *(*it);
            std::unique_lock arena_lock(arena.mu);
            if (arena.occupied_count.load(std::memory_order_relaxed) != 0)
                return;

            if (tls_arena_uid_ == arena_uid)
                tls_arena_uid_ = 0;

            arenas_.erase(it);
        }

    public:
        MultiArenaPool(std::size_t block_size, std::size_t initial_block_count,
                       std::size_t max_arenas = MAX_ARENAS)
            : block_size_(round_up(std::max(block_size, sizeof(Node)),
                                   alignof(std::max_align_t))),
              block_header_size_(round_up(std::max(sizeof(BlockHeader), sizeof(Node)),
                                          alignof(std::max_align_t))),
              block_stride_(0),
              initial_block_count_(initial_block_count == 0 ? 1 : initial_block_count),
              max_arenas_(std::min(max_arenas, MAX_ARENAS))
        {
            if (max_arenas_ == 0)
                throw std::invalid_argument("max_arenas must be > 0");

            block_stride_ = round_up(block_header_size_ + block_size_,
                                     alignof(std::max_align_t));

            std::unique_lock lock(arenas_mu_);
            Arena *primary = create_arena_unlocked(initial_block_count_,
                                                   std::this_thread::get_id());
            tls_arena_uid_ = primary->arena_uid;
        }

        MultiArenaPool(const MultiArenaPool &) = delete;
        MultiArenaPool &operator=(const MultiArenaPool &) = delete;
        MultiArenaPool(MultiArenaPool &&) = delete;

        [[nodiscard]] void *allocate()
        {
            const std::thread::id this_thread = std::this_thread::get_id();

            if (tls_arena_uid_ != 0)
            {
                std::shared_lock lock(arenas_mu_);
                Arena *arena = find_arena_by_uid_unlocked(tls_arena_uid_);
                if (arena && arena->owner_thread == this_thread)
                {
                    if (void *p = try_allocate_from_arena(*arena))
                        return p;
                }
            }

            {
                std::shared_lock lock(arenas_mu_);
                for (const auto &arena_ptr : arenas_)
                {
                    Arena &arena = *arena_ptr;
                    if (arena.owner_thread != this_thread)
                        continue;
                    if (void *p = try_allocate_from_arena(arena))
                    {
                        tls_arena_uid_ = arena.arena_uid;
                        return p;
                    }
                }
            }

            std::unique_lock lock(arenas_mu_);

            if (tls_arena_uid_ != 0)
            {
                Arena *current = find_arena_by_uid_unlocked(tls_arena_uid_);
                if (current && current->owner_thread == this_thread)
                {
                    if (void *p = try_allocate_from_arena(*current))
                        return p;

                    std::size_t next_count = next_block_count(current->block_count);
                    Arena *created = create_arena_unlocked(next_count, this_thread);
                    tls_arena_uid_ = created->arena_uid;
                    if (void *p = try_allocate_from_arena(*created))
                        return p;
                }
            }

            Arena *created = create_arena_unlocked(initial_block_count_, this_thread);
            tls_arena_uid_ = created->arena_uid;
            if (void *p = try_allocate_from_arena(*created))
                return p;

            throw std::bad_alloc{};
        }

        void deallocate(void *p) noexcept
        {
            if (!p)
                return;

            std::byte *payload = static_cast<std::byte *>(p);
            auto *header = reinterpret_cast<BlockHeader *>(payload - block_header_size_);
            std::uint64_t arena_uid = header->arena_uid;

            {
                std::shared_lock lock(arenas_mu_);
                Arena *arena = find_arena_by_uid_unlocked(arena_uid);
                if (!arena)
                    return;

                std::shared_lock arena_lock(arena->mu);
                arena->free_list.push(payload - block_header_size_);
                std::size_t previous = arena->occupied_count.fetch_sub(
                    1, std::memory_order_relaxed);
                if (previous != 1)
                    return;
            }

            remove_arena_if_empty(arena_uid);
        }

        FragmentationStats get_fragmentation_stats() const
        {
            FragmentationStats stats{};

            std::shared_lock lock(arenas_mu_);
            stats.num_arenas = arenas_.size();
            stats.per_arena_stats.reserve(arenas_.size());

            for (const auto &arena_ptr : arenas_)
            {
                const Arena &arena = *arena_ptr;
                std::size_t occupied = arena.occupied_count.load(std::memory_order_relaxed);
                std::size_t free = arena.block_count - occupied;

                stats.total_blocks += arena.block_count;
                stats.free_blocks += free;
                stats.occupied_blocks += occupied;

                FragmentationStats::PerArenaStats per_arena{};
                per_arena.arena_uid = arena.arena_uid;
                per_arena.owner_thread = arena.owner_thread;
                per_arena.free_count = free;
                per_arena.occupied_count = occupied;
                stats.per_arena_stats.push_back(per_arena);
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

            void *new_ptr = allocate();
            std::memcpy(new_ptr, old_ptr, block_size_);
            deallocate(old_ptr);
            return new_ptr;
        }

        void compact()
        {
            std::unique_lock lock(arenas_mu_);
            auto it = arenas_.begin();
            while (it != arenas_.end())
            {
                Arena &arena = *(*it);
                std::unique_lock arena_lock(arena.mu);
                if (arena.occupied_count.load(std::memory_order_relaxed) == 0)
                {
                    if (tls_arena_uid_ == arena.arena_uid)
                        tls_arena_uid_ = 0;
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
            std::shared_lock lock(arenas_mu_);
            return arenas_.size();
        }

        std::size_t block_size() const noexcept { return block_size_; }

        std::size_t total_block_count() const noexcept
        {
            std::shared_lock lock(arenas_mu_);
            std::size_t total = 0;
            for (const auto &arena_ptr : arenas_)
                total += arena_ptr->block_count;
            return total;
        }
    };
} // namespace pp
