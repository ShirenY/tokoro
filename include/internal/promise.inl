#pragma once

// All function definitions of Promise related classes
// Put them in the .inl file to avoid compiling order issue.(Depend on CoroAwaiterBase and Scheduler)

#include <cassert>

namespace tokoro::internal
{

// PromiseBase::FinalAwaiter functions
//
inline bool PromiseBase::FinalAwaiter::await_ready() const noexcept
{
    return false;
}

// LCOV_EXCL_START This function never used, but compiler need it.
inline void PromiseBase::FinalAwaiter::await_resume() const noexcept
{
}
// LCOV_EXCL_STOP

inline std::coroutine_handle<> PromiseBase::FinalAwaiter::await_suspend(std::coroutine_handle<> h) const noexcept
{
    auto             handle        = std::coroutine_handle<PromiseBase>::from_address(h.address());
    auto&            promise       = handle.promise();
    const uint64_t   coroId        = promise.mId;
    CoroAwaiterBase* parentAwaiter = promise.mParentAwaiter;

    assert(parentAwaiter != nullptr || coroId != 0 && "A coro should have parent awaiter or coroId.");
    assert(parentAwaiter == nullptr || coroId == 0 && "Coro can't have bother parent and coroId.");

    if (parentAwaiter != nullptr)
    {
        return parentAwaiter->OnWaitComplete(h);
    }
    else
    {
        promise.GetCoroManager()->OnCoroutineFinished(coroId, promise.mException == nullptr);
        return std::noop_coroutine();
    }
}

// PromiseBase functions
//
inline std::suspend_always PromiseBase::initial_suspend() noexcept
{
    return {};
}

inline PromiseBase::FinalAwaiter PromiseBase::final_suspend() noexcept
{
    return FinalAwaiter{};
}

inline void PromiseBase::unhandled_exception()
{
    mException = std::current_exception();
}

inline void PromiseBase::SetId(uint64_t id)
{
    mId = id;
}

class CoroManager;

inline void PromiseBase::SetCoroManager(CoroManager* coroManager)
{
    mCoroManager = static_cast<void*>(coroManager);
}

inline CoroManager* PromiseBase::GetCoroManager() const
{
    return static_cast<CoroManager*>(mCoroManager);
}

inline void PromiseBase::SetParentAwaiter(CoroAwaiterBase* awaiter)
{
    mParentAwaiter = awaiter;
}

inline void PromiseBase::RethrowIfAny()
{
    if (this->mException)
    {
        auto localException = this->mException;
        this->mException    = nullptr;

        std::rethrow_exception(localException);
    }
}

// Promise<T> functions
//
template <typename T>
auto Promise<T>::get_return_object() noexcept
{
    return Handle::from_promise(*this);
}

template <typename T>
void Promise<T>::return_value(T&& val)
{
    this->mReturnValue = std::forward<T>(val);
}

template <typename T>
void Promise<T>::return_value(const T& val)
{
    this->mReturnValue = val;
}

template <typename T>
T Promise<T>::TakeResult()
{
    RethrowIfAny();
    return std::move(mReturnValue.value());
}

// Promise<void> functions
//
inline auto Promise<void>::get_return_object() noexcept
{
    return Handle::from_promise(*this);
}

inline void Promise<void>::return_void()
{
}

inline void Promise<void>::TakeResult()
{
    RethrowIfAny();
}

} // namespace tokoro::internal
