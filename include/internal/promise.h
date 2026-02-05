#pragma once

#include "defines.h"
#include "sizedany.h"

#include <coroutine>
#include <exception>

namespace tokoro
{

namespace internal
{

class CoroManager;
class CoroAwaiterBase;

class PromiseBase
{
public:
    struct FinalAwaiter
    {
        bool                    await_ready() const noexcept;
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) const noexcept;
        void                    await_resume() const noexcept;
    };

    std::suspend_always initial_suspend() noexcept;
    FinalAwaiter        final_suspend() noexcept;
    void                unhandled_exception();

    void SetId(uint64_t id);

    void         SetCoroManager(CoroManager* scheduler);
    CoroManager* GetCoroManager() const;

    void SetParentAwaiter(CoroAwaiterBase* awaiter);

protected:
    void RethrowIfAny();

    std::exception_ptr mException;
    SizedAny<32>     mReturnValue;
    uint64_t           mId            = 0;
    CoroAwaiterBase*   mParentAwaiter = nullptr;
    void*              mCoroManager   = nullptr;
};

template <typename T>
class Promise : public PromiseBase
{
public:
    using Handle = std::coroutine_handle<Promise<T>>;

    auto get_return_object() noexcept;
    void return_value(T&& val);
    void return_value(const T& val);
    T    TakeResult();
};

template <>
class Promise<void> : public PromiseBase
{
public:
    using Handle = std::coroutine_handle<Promise<void>>;

    auto get_return_object() noexcept;
    void return_void();
    void TakeResult();
};

} // namespace internal

} // namespace tokoro