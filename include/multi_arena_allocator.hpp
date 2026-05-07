#pragma once
#include "multi_arena_pool.hpp"

namespace pp
{
    template <typename T>
    class MultiArenaAllocator
    {
        MultiArenaPool *pool_;

        template <typename U>
        friend class MultiArenaAllocator;

    public:
        using value_type = T;

        template <typename U>
        struct rebind
        {
            using other = MultiArenaAllocator<U>;
        };

        explicit MultiArenaAllocator(MultiArenaPool &pool) noexcept
            : pool_(&pool)
        {
        }

        template <typename U>
        MultiArenaAllocator(const MultiArenaAllocator<U> &o) noexcept
            : pool_(o.pool_)
        {
        }

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

        bool operator==(const MultiArenaAllocator &o) const noexcept
        {
            return pool_ == o.pool_;
        }
        bool operator!=(const MultiArenaAllocator &o) const noexcept
        {
            return pool_ != o.pool_;
        }
    };
} // namespace pp
