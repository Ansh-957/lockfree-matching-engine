#pragma once

#include <cstddef>
#include <new>
#include <optional>

namespace engine {

template <typename T, size_t PoolSize>

class MemoryPool {
    public:

    //constructor to connect all slots into free list
    MemoryPool() {
        for (size_t i = PoolSize; i > 0; --i) {
            auto* node = reinterpret_cast<FreeNode*>(slot_ptr(i - 1));
            node->next = free_head_;
            free_head_ = node;
        }
    }

    //allocate a new object, pops a free node off the free list then returns a pointer to the popped slot
    [[nodiscard]] T* allocate() noexcept {
        if (free_head_ == nullptr) {
            return nullptr;
        }

        FreeNode* node = free_head_;
        free_head_ = node->next;
        T* result = reinterpret_cast<T*>(node);
        return result;
    }

    //deallocate an object, pushes the slot back onto the free list
    void deallocate(T* ptr) noexcept {
        FreeNode* node = reinterpret_cast<FreeNode*>(ptr);
        node->next = free_head_;    
        free_head_ = node;
    }



    private:
    //converting array index into an address
    //avoiding construction of T objects on compile time and only allocating raw bytes
    [[nodiscard]] std::byte* slot_ptr(size_t index) noexcept {
        return &storage_[index * sizeof(T)];
    }

    struct FreeNode {
        FreeNode* next;
    };

    alignas(alignof(T)) std::byte storage_[sizeof(T) * PoolSize];
    FreeNode* free_head_ = nullptr;
    
};



} //end namespace engine