#pragma once
#include "free_list.hpp"
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>

namespace pp
{
    class PacketPool
    {
        struct FreeDeleter
        {
            void operator()(void *p) const noexcept { std::free(p); }
        };
        std::unique_ptr<std::byte[], FreeDeleter> storage_;
        FreeList free_list_;
        std::size_t block_size_;
        std::size_t block_count_;
        std::atomic<std::size_t> free_count_{0};

        static std::size_t round_up(std::size_t n, std::size_t align) noexcept
        {
            return (n + align - 1) & ~(align - 1);
        }

    public:
        PacketPool(std::size_t block_size, std::size_t block_count)
            : block_size_(round_up(
                  std::max(block_size, sizeof(Node)),
                  alignof(std::max_align_t))),
              block_count_(block_count)
        {
            if (block_count_ == 0)
                throw std::invalid_argument("block_count must be > 0");

            void *raw = std::aligned_alloc(alignof(std::max_align_t),
                                           block_size_ * block_count_);
            if (!raw)
                throw std::bad_alloc{};
            storage_.reset(static_cast<std::byte *>(raw));

            for (std::size_t i = 0; i < block_count_; ++i)
                free_list_.push(storage_.get() + i * block_size_);

            free_count_.store(block_count_, std::memory_order_relaxed);
        }

        PacketPool(const PacketPool &) = delete;
        PacketPool &operator=(const PacketPool &) = delete;
        PacketPool(PacketPool &&) = delete;

        [[nodiscard]] void *allocate()
        {
            void *p = free_list_.pop();
            if (!p)
                throw std::bad_alloc{};
            free_count_.fetch_sub(1, std::memory_order_relaxed);
            return p;
        }

        void deallocate(void *p) noexcept
        {
            free_list_.push(p);
            free_count_.fetch_add(1, std::memory_order_relaxed);
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

        std::size_t free_count() const noexcept { return free_count_.load(std::memory_order_relaxed); }
        std::size_t block_size() const noexcept { return block_size_; }
        std::size_t block_count() const noexcept { return block_count_; }
    };
} // namespace pp
