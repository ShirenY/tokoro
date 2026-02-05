#pragma once

#include <type_traits>
#include <utility>
#include <new>
#include <cassert>
#include <cstddef> // for std::max_align_t
#include <new> // std::launder
#include <string_view>
#include <cstdint>

// ============================================================
// SizedAny<Size, Align>
// A std::any alternative:
// - Does not depend on RTTI,
// - Customizable SSO size.
// - Does not support copy. (Because tokoro need support move only types.)
//
// Params:
//  - Size:  SSO buffer size
//  - Align: SSO buffer alignment (default: max_align_t)
// ============================================================

namespace tokoro::internal
{

template <typename T>
constexpr std::string_view TypeName() {
#if defined(__clang__) || defined(__GNUC__)
    return __PRETTY_FUNCTION__;
#elif defined(_MSVC_LANG)
    return __FUNCSIG__;
#endif
}

constexpr uint64_t fnv1a_hash(std::string_view str) {
    uint64_t hash = 14695981039346656037ULL; // Offset Basis
    const uint64_t prime = 1099511628211ULL; // Prime

    for (char c : str) {
        hash ^= static_cast<uint8_t>(c);
        hash *= prime;
    }
    return hash;
}

// TypeId is based on hash of __PRETTY_FUNCTION__
// So it's not reliable cross-compilers.
struct TypeId {
    uint64_t hash;
#ifndef NDEBUG
    std::string_view name; // Only used in dev for hash collision detection
#endif
    bool operator==(const TypeId& other) const {
        if (hash != other.hash) return false;
#ifndef NDEBUG
        assert(name == other.name && "Hash collision detected! Consider change the type name.");
#endif
        return true;
    }
};

template <typename T>
constexpr TypeId GetTypeId() {
    auto name = TypeName<T>();
    return TypeId{
        fnv1a_hash(name),
#ifndef NDEBUG
        name
#endif
    };
}

template <std::size_t Size, std::size_t Align = alignof(std::max_align_t)>
class SizedAny {
    static_assert(Size > 0);
    static_assert((Align& (Align - 1)) == 0, "Align must be a power of 2");
    static_assert(Align <= alignof(std::max_align_t),
                  "Extended alignment not supported. Align must be <= alignof(std::max_align_t)");

    struct alignas(Align) Buffer {
        unsigned char data[Size];
    };

    struct VTable {
        void (*destroy)(SizedAny*);
        void (*move)(SizedAny*, SizedAny*);
        TypeId id;
    };

    template <typename T>
    static constexpr bool kIsStack = sizeof(T) <= Size && 
                                     alignof(T) <= Align &&
                                     std::is_nothrow_move_constructible_v<T>; // So that move constructor can be noexcept

public:
    // SizedAny is not copyable
    SizedAny(const SizedAny&) = delete;
    SizedAny& operator=(const SizedAny&) = delete;

    SizedAny() = default;

    ~SizedAny() {
        Reset();
    }

    SizedAny(SizedAny&& other) noexcept
    {
        if (other.vptr_) {
            other.vptr_->move(&other, this);
            vptr_ = other.vptr_;
            other.vptr_ = nullptr;
        }
    }

    template <typename T, typename Decayed = std::decay_t<T>>
    SizedAny(T&& value)
        requires (!std::is_same_v<Decayed, SizedAny> && !std::is_same_v<Decayed, std::in_place_type_t<Decayed>>)
    {
        this->template emplace<Decayed>(std::forward<T>(value));
    }

    // Support in_place construction
    template <class T, class... Args>
    explicit SizedAny(std::in_place_type_t<T>, Args&&... args) {
        emplace<T>(std::forward<Args>(args)...);
    }

    SizedAny& operator=(SizedAny&& other) noexcept {
        if (this == &other) return *this;
        Reset();
        if (other.vptr_) {
            other.vptr_->move(&other, this);
            vptr_ = other.vptr_;
            other.vptr_ = nullptr;
        }
        return *this;
    }

    bool HasValue() const noexcept {
        return vptr_ != nullptr;
    }

    void Reset() noexcept {
        if (vptr_) {
            vptr_->destroy(this);
            vptr_ = nullptr;
        }
    }

    template <class T>
    bool IsType() const
    {
        if (vptr_ == nullptr)
            return false;

        return GetTypeId<T>() == vptr_->id;
    }

    template <class T>
    T* Get() noexcept {
        if (!IsType<T>())
            return nullptr;

        if constexpr (kIsStack<T>) {
            return std::launder(reinterpret_cast<T*>(&buffer_));
        }
        else {
            return static_cast<T*>(heap_ptr_);
        }
    }

    template <class T>
    const T* Get() const noexcept {
        if (!IsType<T>())
            return nullptr;

        if constexpr (kIsStack<T>) {
            return std::launder(reinterpret_cast<const T*>(&buffer_));
        }
        else {
            return static_cast<const T*>(heap_ptr_);
        }
    }

private:
    const VTable* vptr_ = nullptr;
    union {
        Buffer buffer_;
        void* heap_ptr_;
    };

    template <class T, class... Args>
    void emplace(Args&&... args) {
        Reset();

        using U = std::decay_t<T>;

        if constexpr (kIsStack<U>) {
            new (&buffer_) U(std::forward<Args>(args)...);
        }
        else {
            heap_ptr_ = new U(std::forward<Args>(args)...);
        }

        vptr_ = &vtable_for<U>;
    }

    template <class U>
    static void destroy_impl(SizedAny* self) {
        if constexpr (kIsStack<U>) {
            reinterpret_cast<U*>(&self->buffer_)->~U();
        }
        else {
            delete static_cast<U*>(self->heap_ptr_);
        }
    }

    template <class U>
    static void move_impl(SizedAny* src, SizedAny* dst) {
        if constexpr (kIsStack<U>) {
            new (&dst->buffer_) U(
                std::move(*reinterpret_cast<U*>(&src->buffer_)));
            reinterpret_cast<U*>(&src->buffer_)->~U();
        }
        else {
            dst->heap_ptr_ = src->heap_ptr_;
            src->heap_ptr_ = nullptr;
        }
    }

    template <class U>
    static const VTable vtable_for;
};

// ===================== vtable definitions =====================

template <std::size_t Size, std::size_t Align>
template <class U>
const typename SizedAny<Size, Align>::VTable
SizedAny<Size, Align>::vtable_for = {
    &SizedAny<Size, Align>::template destroy_impl<U>,
    &SizedAny<Size, Align>::template move_impl<U>,
    GetTypeId<U>()
};

}
