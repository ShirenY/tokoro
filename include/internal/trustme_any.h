#pragma once

#include <type_traits>
#include <utility>
#include <new>
#include <cassert>
#include <cstddef> // for std::max_align_t
#include <new> // std::launder

// ============================================================
// TrustMeAny<Size, Align>
//  - Size:  SSO buffer size
//  - Align: SSO buffer alignment (default: max_align_t)
// ============================================================

namespace tokoro::internal
{

template <std::size_t Size, std::size_t Align = alignof(std::max_align_t)>
class TrustMeAny {
    static_assert(Size > 0);
    static_assert((Align& (Align - 1)) == 0, "Align must be a power of 2");
    static_assert(Align <= alignof(std::max_align_t),
                  "Extended alignment not supported. Align must be <= alignof(std::max_align_t)");

    struct alignas(Align) Buffer {
        unsigned char data[Size];
    };

    struct VTable {
        void (*destroy)(TrustMeAny*);
        void (*copy)(const TrustMeAny*, TrustMeAny*);
        void (*move)(TrustMeAny*, TrustMeAny*);
    };

    template <typename T>
    static constexpr bool kIsStack = sizeof(T) <= Size && alignof(T) <= Align;

public:
    TrustMeAny() = default;

    ~TrustMeAny() {
        Reset();
    }

    TrustMeAny(const TrustMeAny& other) {
        if (other.vptr_) {
            if (!other.vptr_->copy) {
                assert(false && "Attempting to copy a Move-only TrustMeAny");
                vptr_ = nullptr;
            }
            else {
                other.vptr_->copy(&other, this);
                vptr_ = other.vptr_;
            }
        }
    }

    TrustMeAny(TrustMeAny&& other) {
        if (other.vptr_) {
            other.vptr_->move(&other, this);
            vptr_ = other.vptr_;
            other.vptr_ = nullptr;
        }
    }

    template <typename T, typename Decayed = std::decay_t<T>>
    TrustMeAny(T&& value) requires (!std::is_same_v<Decayed, TrustMeAny>)
    {
        this->emplace<Decayed>(std::forward<T>(value));
    }

    // Support in_place construction
    template <class T, class... Args>
    explicit TrustMeAny(std::in_place_type_t<T>, Args&&... args) {
        emplace<T>(std::forward<Args>(args)...);
    }

    TrustMeAny& operator=(const TrustMeAny& other)
    {
        if (this != &other) 
        {
            // Strong Exception Guarantee via Swap-like logic
            TrustMeAny tmp(other); // May throw, but `this` is untouched
            *this = std::move(tmp); // Move assign (usually safe)
        }
        return *this;
    }

    TrustMeAny& operator=(TrustMeAny&& other) noexcept {
        if (this == &other) return *this;
        Reset();
        if (other.vptr_) {
            other.vptr_->move(&other, this);
            vptr_ = other.vptr_;
            other.vptr_ = nullptr;
        }
        return *this;
    }

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
    T* Cast() noexcept {
        if (!vptr_) return nullptr;
        // TRUST ME: logic assumes T is correct.
        if constexpr (kIsStack<T>) {
            return std::launder(reinterpret_cast<T*>(&buffer_));
        }
        else {
            return static_cast<T*>(heap_ptr_);
        }
    }

    template <class T>
    const T* Cast() const noexcept {
        if (!vptr_) return nullptr;
        if constexpr (kIsStack<T>) {
            return std::launder(reinterpret_cast<const T*>(&buffer_));
        }
        else {
            return static_cast<const T*>(heap_ptr_);
        }
    }

private:
    union {
        Buffer buffer_;
        void* heap_ptr_;
    };

    const VTable* vptr_ = nullptr;

    template <class U>
    static void destroy_impl(TrustMeAny* self) {
        if constexpr (kIsStack<U>) {
            reinterpret_cast<U*>(&self->buffer_)->~U();
        }
        else {
            delete static_cast<U*>(self->heap_ptr_);
        }
    }

    template <class U>
    static void copy_impl(const TrustMeAny* src, TrustMeAny* dst) {
        if constexpr (std::is_copy_constructible_v<U>) {
            if constexpr (kIsStack<U>) {
                new (&dst->buffer_) U(
                    *reinterpret_cast<const U*>(&src->buffer_));
            }
            else {
                dst->heap_ptr_ = new U(*static_cast<U*>(src->heap_ptr_));
            }
        }
    }

    template <class U>
    static void move_impl(TrustMeAny* src, TrustMeAny* dst) {
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
const typename TrustMeAny<Size, Align>::VTable
TrustMeAny<Size, Align>::vtable_for = {
    &TrustMeAny<Size, Align>::template destroy_impl<U>,
    std::is_copy_constructible_v<U> ? &TrustMeAny::copy_impl<U> : nullptr,
    &TrustMeAny<Size, Align>::template move_impl<U>
};

}
