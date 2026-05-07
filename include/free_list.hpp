#pragma once
#include <atomic>
#include <cstdint>

namespace pp
{
    struct Node
    {
        uint16_t arena_id;  // Which arena owns this block
        Node *next;
    };

    class FreeList
    {
        static_assert(std::atomic<uint64_t>::is_always_lock_free);

        static constexpr uint64_t PTR_MASK = (1ULL << 48) - 1;
        static constexpr int TAG_SHIFT = 48;

        std::atomic<uint64_t> head_{0};

        static uint64_t pack(Node *ptr, uint64_t tag) noexcept
        {
            return (tag << TAG_SHIFT) | (reinterpret_cast<uint64_t>(ptr) & PTR_MASK);
        }
        static Node *unpack_ptr(uint64_t v) noexcept
        {
            return reinterpret_cast<Node *>(v & PTR_MASK);
        }
        static uint16_t unpack_tag(uint64_t v) noexcept
        {
            return static_cast<uint16_t>(v >> TAG_SHIFT);
        }

    public:
        FreeList() = default;
        FreeList(const FreeList &) = delete;
        FreeList &operator=(const FreeList &) = delete;

        void push(void *block) noexcept
        {
            Node *node = static_cast<Node *>(block);
            uint64_t cur = head_.load(std::memory_order_relaxed);
            uint64_t next;
            do
            {
                node->next = unpack_ptr(cur);
                next = pack(node, unpack_tag(cur) + 1u);
            } while (!head_.compare_exchange_weak(cur, next,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed));
        }

        void *pop() noexcept
        {
            uint64_t cur = head_.load(std::memory_order_acquire);
            uint64_t next;
            do
            {
                Node *ptr = unpack_ptr(cur);
                if (!ptr)
                    return nullptr;
                next = pack(ptr->next, unpack_tag(cur) + 1u);
            } while (!head_.compare_exchange_weak(cur, next,
                                                  std::memory_order_release,
                                                  std::memory_order_relaxed));
            return unpack_ptr(cur);
        }
    };

} // namespace pp
