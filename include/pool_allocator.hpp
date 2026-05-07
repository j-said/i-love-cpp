#pragma once
#include "packet_pool.hpp"

namespace pp
{
    template <typename T>
    class PoolAllocator
    {
        PacketPool *pool_;

        template <typename U>
        friend class PoolAllocator;

    public:
        using value_type = T;

        template <typename U>
        struct rebind
        {
            using other = PoolAllocator<U>;
        };

        explicit PoolAllocator(PacketPool &pool) noexcept : pool_(&pool) {}

        template <typename U>
        PoolAllocator(const PoolAllocator<U> &o) noexcept : pool_(o.pool_) {}

        T *allocate(std::size_t n)
        {
            if (n == 0)
                return nullptr;
            if (n * sizeof(T) > pool_->block_size())
                throw std::bad_alloc{};
            return static_cast<T *>(pool_->allocate());
        }

        void deallocate(T *p, std::size_t) noexcept
        {
            if (p)
                pool_->deallocate(p);
        }

        bool operator==(const PoolAllocator &o) const noexcept { return pool_ == o.pool_; }
        bool operator!=(const PoolAllocator &o) const noexcept { return pool_ != o.pool_; }
    };
} // namespace pp
