#pragma once

#include "internal/defines.h"
#include "internal/promise.h"
#include "internal/singleawaiter.h"
#include "internal/timequeue.h"

#include <array>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <functional>
#include <memory>
#include <optional>

namespace tokoro
{

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
class SchedulerBP;

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
class WaitBP
{
public:
    WaitBP(double sec, UpdateEnum updateType = internal::GetEnumDefault<UpdateEnum>(), TimeEnum timeType = internal::GetEnumDefault<TimeEnum>());
    WaitBP(UpdateEnum updateType = internal::GetEnumDefault<UpdateEnum>(), TimeEnum timeType = internal::GetEnumDefault<TimeEnum>());
    ~WaitBP();

    // Functions for C++ coroutine callbacks
    //
    bool await_ready() const noexcept;
    template <typename T>
    void await_suspend(std::coroutine_handle<internal::Promise<T>> handle) noexcept;
    void await_resume() const noexcept;

    void Resume();

private:
    friend class SchedulerBP<UpdateEnum, TimeEnum>;

    std::optional<typename internal::TimeQueue<WaitBP*>::Iterator> mExeIter;
    double                                                         mDelay;
    std::coroutine_handle<internal::PromiseBase>                   mHandle = nullptr;
    UpdateEnum                                                     mUpdateType;
    TimeEnum                                                       mTimeType;
};

namespace internal
{
class CoroManager;
}

enum class AsyncState
{
    Running,
    Succeed,
    Failed,  // When coroutine threw exception.
    Stopped, // When coroutine stopped by Handle.
};

template <typename T>
class Handle
{
    // The tool to control coroutines from outside.
    // When a Handle go out of scope, the coroutine associate with it will
    // Keep running to the end. When it succeeds/fails, the coroutine will
    // be removed.

public:
    // Default constructor will create an invalid Handle
    Handle() noexcept = default;
    Handle(Handle&& other) noexcept;
    Handle& operator=(Handle&& other) noexcept;
    Handle(const Handle& other) = delete; // Handle is not copiable.

    // Destructor will stop the associated coroutine.
    ~Handle() noexcept;

    // True if handle is not create by Scheduler.
    bool IsValid() const noexcept;

    // Manually stop a coroutine when they are running. Or the method will do nothing.
    void Stop() const noexcept;

    // Normally Handles with destroy associated coroutines when they go out of scope.
    // Forget makes them won't. However, everything else in Handle still works after forget.
    // Forget is good for Fire and Forget situation.
    void Forget() noexcept;

    // Return nothing if the Scheduler is no longer exist.
    std::optional<AsyncState> GetState() const noexcept;

    // A quick utility for user to detect whether a coroutine is still running.
    // Returns false when: !IsValid()/!GetState().have_value()/*GetState() != AsyncState::Running
    bool IsRunning() const noexcept;

    // Take result out of succeed coroutine.
    // When a coroutine AsyncState::Failed, it will throw out the exception.
    // Can only work for once.
    std::optional<T> TakeResult() const
        requires(!std::is_void_v<T>);

    // When a coroutine AsyncState::Failed, this method will throw out the exception.
    // Can only work for once.
    void TakeResult() const
        requires(std::is_void_v<T>);

private:
    friend class internal::CoroManager;

    Handle(uint64_t id, internal::CoroManager* coroMgr, const std::weak_ptr<std::monostate>& liveSignal)
        : mId(id), mCoroMgr(coroMgr), mCoroMgrLiveSignal(liveSignal)
    {
    }

    void Reset();

    uint64_t                      mId            = 0;
    bool                          mBoundLifetime = true;
    internal::CoroManager*        mCoroMgr       = nullptr;
    std::weak_ptr<std::monostate> mCoroMgrLiveSignal;
};

template <typename... Ts>
class Any;

template <typename... Ts>
class All;

template<typename RetType>
std::coroutine_handle<internal::Promise<RetType>> toTypedHandle(std::coroutine_handle<> handle)
{
    return std::coroutine_handle<internal::Promise<RetType>>::from_address(handle.address());
}

// Async<T> is the return value of every tokoro coroutine.
// It's meanly used for RAII management of sub-coroutines, passing as awaiter.
// You won't need to manipulate it directly as an end user.
template <typename T>
class Async
{
public:
    using promise_type = internal::Promise<T>;
    using value_type   = T;
    using handle_type  = std::coroutine_handle<promise_type>;

    Async(handle_type h) noexcept
        : mHandle(h)
    {
    }

    Async(Async&& o) noexcept
        : mHandle(o.mHandle)
    {
        o.mHandle = nullptr;
    }

    ~Async()
    {
        if (mHandle)
            mHandle.destroy();
    }

    auto operator co_await() noexcept
    {
        return internal::SingleCoroAwaiter(toTypedHandle<T>(mHandle));
    }

private:
    template <typename... Ts>
    friend class All;
    template <typename... Ts>
    friend class Any;
    friend class internal::CoroManager;

    // APIs for friends
    //

    // Takeover the lifetime of mHandle. This Async will become invalid.
    std::coroutine_handle<> takeHandle()
    {
        auto handle = mHandle;
        mHandle = nullptr;
        return handle;
    }

    std::coroutine_handle<promise_type> getTypedHandle()
    {
        return std::coroutine_handle<promise_type>::from_address(mHandle.address());
    }

    // Even friends should not R\W to this member directly.
    std::coroutine_handle<> mHandle;
};

namespace internal
{

// Helper template for Scheduler
//
template <typename Func, typename... Args>
using AsyncReturnT = std::invoke_result_t<Func, Args...>;

template <typename Func, typename... Args>
using AsyncValueT = typename AsyncReturnT<Func, Args...>::value_type;

template <typename Func, typename... Args>
concept ReturnsAsync = std::invocable<Func, Args...> &&
                       std::same_as<AsyncReturnT<Func, Args...>, Async<AsyncValueT<Func, Args...>>>;

class CoroManager
{
public:
    CoroManager()
    {
        mLiveSignal = std::make_shared<std::monostate>();
    }

    /// Start: start a coroutine and return its handle.
    /// func: Callable object that returns Async<T>. Could be a lambda or function.
    /// funcArgs: parameters of AsyncFunc, Start will forward them to construct the coroutine.
    /// Return value: The Handle of new started coroutine. When handle destroyed they will automatically
    ///               Stop() the coroutine, so you should never discard the return. You can use Forget()
    ///               to do fire and forget style:
    ///               sched.Start(Something).Forget();
    template <typename AsyncFunc, typename... Args>
        requires internal::ReturnsAsync<AsyncFunc, Args...> // Constrain that need function to return Async<T>
    [[nodiscard]] Handle<AsyncValueT<AsyncFunc, Args...>> Start(AsyncFunc&& func, Args&&... funcArgs)
    {
        using RetType = AsyncValueT<AsyncFunc, Args...>;

        uint64_t id          = mNextId++;
        auto [iter, succeed] = mCoroutines.emplace(id, Entry());

        Entry& newEntry = iter->second;

        // Cache the input function and parameters into a lambda to avoid the famous C++ coroutine pitfall.
        // https://devblogs.microsoft.com/oldnewthing/20211103-00/?p=105870
        // <A capturing lambda can be a coroutine, but you have to save your captures while you still can>
        newEntry.lambda = [task = std::forward<AsyncFunc>(func), tup = std::make_tuple(std::forward<Args>(funcArgs)...)]() mutable {
            return std::apply(task, tup).takeHandle(); // For top coroutines, CoroManager will take over handle's lifetime management.
        };

        // Create the Coro<T>
        newEntry.handle = newEntry.lambda();

        auto& promise = toTypedHandle<RetType>(newEntry.handle).promise();
        promise.SetId(id);
        promise.SetCoroManager(this);

        // Kick off the coroutine.
        newEntry.handle.resume();

        // Check if the new coroutine already stopped running.
        StopNewFinishedCoro();

        return Handle<RetType>{id, this, mLiveSignal};
    }

protected:
    void ClearCoros()
    {
        mCoroutines.clear();
    }

    void StopNewFinishedCoro()
    {
        if (mNewFinishedCoro == 0)
            return;

        const auto it    = mCoroutines.find(mNewFinishedCoro);
        mNewFinishedCoro = 0;

        Entry& e = it->second;
        assert(it != mCoroutines.end() && e.state == AsyncState::Running);

        e.state  = mNewFinishedSucceed ? AsyncState::Succeed : AsyncState::Failed;
        e.lambda = {}; // Remove start lambda

        if (e.released)
        {
            // When coro is stopped running and released by handle, we can delete it.
            mCoroutines.erase(it);
        }
    }

private:
    template <typename T>
    friend class tokoro::Async;
    template <typename T>
    friend class tokoro::Handle;
    friend class PromiseBase;

    void Release(uint64_t id)
    {
        auto it = mCoroutines.find(id);
        assert(it != mCoroutines.end() && !it->second.released);

        it->second.released = true;
        if (it->second.state != AsyncState::Running)
        {
            // When coro is stopped running and released by handle, we can delete it.
            mCoroutines.erase(it);
        }
    }

    void Stop(uint64_t id)
    {
        const auto it = mCoroutines.find(id);
        assert(it != mCoroutines.end());

        auto& entry = it->second;
        assert(!entry.released && "Coroutines should not be released, if their handle is trying to stop (Handle still alive).");

        if (entry.state == AsyncState::Running)
        {
            entry.state = AsyncState::Stopped;
            entry.handle.destroy(); // Destroy the handle
            entry.handle = nullptr;
            entry.lambda = {};  // Remove start lambda
        }
        else
        {
            // When coro already stopped running, do nothing.
        }
    }

    AsyncState GetState(uint64_t id)
    {
        const auto it = mCoroutines.find(id);
        assert(it != mCoroutines.end());

        return it->second.state;
    }

    template <typename T>
        requires(!std::is_void_v<T>)
    std::optional<T> TakeResult(uint64_t id)
    {
        auto& entry = mCoroutines[id];
        if (entry.handle == nullptr)
            return std::nullopt;

        auto handle = entry.handle;
        entry.handle = nullptr;
        return std::move(toTypedHandle<T>(handle).promise().TakeResult());
    }

    template <typename T>
        requires(std::is_void_v<T>)
    void TakeResult(uint64_t id)
    {
        auto& entry = mCoroutines[id];
        if (entry.handle == nullptr)
            return;

        toTypedHandle<T>(entry.handle).promise().TakeResult();
    }

    void OnCoroutineFinished(uint64_t id, bool isSucceed)
    {
        // Because delete root coroutine inside FinalAwaiter::await_suspend() will delete
        // the return value receiver of await_suspend() too. Which will lead to use after free
        // issue for 'return std::noop_coroutine();'. So add a delay release mechanic for scheduler
        // managed coroutines.

        assert(id != 0 && "id parameter should never be invalid in this method.");
        assert(mNewFinishedCoro == 0 && "There's already a coro need to be finished. Only one coro at max should be finished in one awaiter resume.");

        mNewFinishedCoro    = id;
        mNewFinishedSucceed = isSucceed;
    }

    struct Entry
    {
        std::coroutine_handle<>                  handle;
        std::function<std::coroutine_handle<>()> lambda;
        AsyncState                               state    = AsyncState::Running;
        bool                                     released = false;
    };

    uint64_t                            mNextId = 1;
    std::unordered_map<uint64_t, Entry> mCoroutines;
    uint64_t                            mNewFinishedCoro    = 0;
    bool                                mNewFinishedSucceed = true;
    std::shared_ptr<std::monostate>     mLiveSignal;
};

} // namespace internal

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
class SchedulerBP : public internal::CoroManager
{
public:
    // Scheduler is neither copyable or movable.
    SchedulerBP()                              = default;
    SchedulerBP(const SchedulerBP&)            = delete;
    SchedulerBP& operator=(const SchedulerBP&) = delete;
    SchedulerBP(SchedulerBP&&)                 = delete;
    SchedulerBP& operator=(SchedulerBP&&)      = delete;

    ~SchedulerBP()
    {
        // Clear coroutines first, so that the Wait objects can be safely removed from mExecuteQueues.
        // If we do the other way around
        CoroManager::ClearCoros();

        for (auto& queue : mExecuteQueues)
        {
            queue.Clear();
        }
    }

    // SetCustomTimer: Set custom timer for specific time type to replace default realtime timer.
    void SetCustomTimer(TimeEnum timeType, std::function<double()> getTimeFunc)
    {
        mCustomTimers[static_cast<int>(timeType)] = std::move(getTimeFunc);
    }

    void Update(UpdateEnum updateType = UpdateEnum::Update,
                TimeEnum   timeType   = TimeEnum::Realtime)
    {
        auto& timeQueue = GetUpdateQueue(updateType, timeType);
        timeQueue.SetupUpdate(GetCurrentTime(timeType));

        while (timeQueue.CheckUpdate())
        {
            timeQueue.Pop()->Resume();

            CoroManager::StopNewFinishedCoro();
        }
    }

private:
    using MyWait = WaitBP<UpdateEnum, TimeEnum>;
    friend MyWait;

    int TypesToIndex(UpdateEnum updateType, TimeEnum timeType)
    {
        const int updateIndex = static_cast<int>(updateType);
        const int timeIndex   = static_cast<int>(timeType);
        return updateIndex * static_cast<int>(TimeEnum::Count) + timeIndex;
    }

    internal::TimeQueue<MyWait*>& GetUpdateQueue(UpdateEnum updateType, TimeEnum timeType)
    {
        int queueIndex = TypesToIndex(updateType, timeType);
        return mExecuteQueues[queueIndex];
    }

    std::function<double()>& GetCustomTimer(TimeEnum timeType)
    {
        return mCustomTimers[static_cast<int>(timeType)];
    }

    static double defaultTimer()
    {
        using Clock     = std::chrono::steady_clock;
        using TimePoint = Clock::time_point;

        static TimePoint                    startTime = Clock::now();
        const std::chrono::duration<double> diff      = Clock::now() - startTime;
        return diff.count();
    }

    double GetCurrentTime(TimeEnum timeType)
    {
        auto& customTimer = GetCustomTimer(timeType);
        if (customTimer)
        {
            return customTimer();
        }
        else
        {
            return defaultTimer();
        }
    }

    using WaitIter = typename internal::TimeQueue<MyWait*>::Iterator;
    WaitIter AddWait(MyWait* wait, UpdateEnum updateType, TimeEnum timeType)
    {
        auto& timeQueue = GetUpdateQueue(updateType, timeType);

        double executeTime = 0;
        if (wait->mDelay != 0)
            executeTime = GetCurrentTime(timeType) + wait->mDelay;
        return timeQueue.AddTimed(executeTime, wait);
    }

    void RemoveWait(WaitIter waitHandle, UpdateEnum updateType, TimeEnum timeType)
    {
        auto& timeQueue = GetUpdateQueue(updateType, timeType);
        timeQueue.Remove(waitHandle);
    }

    static constexpr int UpdateQueueCount = static_cast<int>(UpdateEnum::Count) * static_cast<int>(TimeEnum::Count);

    std::array<internal::TimeQueue<MyWait*>, UpdateQueueCount>             mExecuteQueues;
    std::array<std::function<double()>, static_cast<int>(TimeEnum::Count)> mCustomTimers;
};

// Handle functions
//
template <typename T>
Handle<T>::Handle(Handle&& other) noexcept
    : mId(other.mId),
      mBoundLifetime(other.mBoundLifetime),
      mCoroMgr(other.mCoroMgr),
      mCoroMgrLiveSignal(std::move(other.mCoroMgrLiveSignal))
{
    other.mId      = 0;
    other.mCoroMgr = nullptr;
}

template <typename T>
Handle<T>& Handle<T>::operator=(Handle&& other) noexcept
{
    if (this != &other)
    {
        Reset();

        mId                = other.mId;
        mBoundLifetime     = other.mBoundLifetime;
        mCoroMgr           = other.mCoroMgr;
        mCoroMgrLiveSignal = std::move(other.mCoroMgrLiveSignal);

        other.mId      = 0;
        other.mCoroMgr = nullptr;
    }
    return *this;
}

template <typename T>
Handle<T>::~Handle() noexcept
{
    Reset();
}

template <typename T>
bool Handle<T>::IsValid() const noexcept
{
    return mId != 0;
}

template <typename T>
void Handle<T>::Stop() const noexcept
{
    if (!IsValid())
        return;

    if (!mCoroMgrLiveSignal.expired())
        mCoroMgr->Stop(mId);
}

template <typename T>
void Handle<T>::Forget() noexcept
{
    mBoundLifetime = false;
}

template <typename T>
std::optional<AsyncState> Handle<T>::GetState() const noexcept
{
    if (!IsValid())
        return std::nullopt;

    if (!mCoroMgrLiveSignal.expired())
        return mCoroMgr->GetState(mId);
    else
        return std::nullopt;
}

template <typename T>
bool Handle<T>::IsRunning() const noexcept
{
    const auto stateHolder = GetState();
    if (!stateHolder.has_value())
        return false;

    const auto state = stateHolder.value();
    return state == AsyncState::Running;
}

template <typename T>
std::optional<T> Handle<T>::TakeResult() const
    requires(!std::is_void_v<T>)
{
    if (!IsValid())
        return std::nullopt;

    if (mCoroMgrLiveSignal.expired())
        return std::nullopt;

    if (GetState().value() == AsyncState::Running)
        return std::nullopt;

    return mCoroMgr->TakeResult<T>(mId);
}

template <typename T>
void Handle<T>::TakeResult() const
    requires(std::is_void_v<T>)
{
    if (!IsValid())
        return;

    if (mCoroMgrLiveSignal.expired())
        return;

    if (GetState().value() == AsyncState::Running)
        return;

    mCoroMgr->TakeResult<T>(mId);
}

template <typename T>
void Handle<T>::Reset()
{
    if (mId != 0 && !mCoroMgrLiveSignal.expired())
    {
        if (mBoundLifetime)
            mCoroMgr->Stop(mId);
        mCoroMgr->Release(mId);

        mId      = 0;
        mCoroMgr = nullptr;
        mCoroMgrLiveSignal.reset();
        mBoundLifetime = true;
    }
}

// TimeAwaiter functions
//
template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
WaitBP<UpdateEnum, TimeEnum>::WaitBP(double sec, UpdateEnum updateType, TimeEnum timeType)
    : mDelay(sec),
      mUpdateType(updateType), mTimeType(timeType)
{
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
WaitBP<UpdateEnum, TimeEnum>::WaitBP(UpdateEnum updateType, TimeEnum timeType)
    : mDelay(0), mUpdateType(updateType), mTimeType(timeType)
{
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
WaitBP<UpdateEnum, TimeEnum>::~WaitBP()
{
    if (mExeIter.has_value())
    {
        auto coroMgrPtr   = mHandle.promise().GetCoroManager();
        auto schedulerPtr = static_cast<SchedulerBP<UpdateEnum, TimeEnum>*>(coroMgrPtr);
        schedulerPtr->RemoveWait(*mExeIter, mUpdateType, mTimeType);
    }
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
bool WaitBP<UpdateEnum, TimeEnum>::await_ready() const noexcept
{
    return false;
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
template <typename T>
void WaitBP<UpdateEnum, TimeEnum>::await_suspend(std::coroutine_handle<internal::Promise<T>> handle) noexcept
{
    mHandle           = std::coroutine_handle<internal::PromiseBase>::from_address(handle.address());
    auto coroMgrPtr   = mHandle.promise().GetCoroManager();
    auto schedulerPtr = static_cast<SchedulerBP<UpdateEnum, TimeEnum>*>(coroMgrPtr);
    mExeIter          = schedulerPtr->AddWait(this, mUpdateType, mTimeType);
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
void WaitBP<UpdateEnum, TimeEnum>::await_resume() const noexcept
{
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
void WaitBP<UpdateEnum, TimeEnum>::Resume()
{
    assert(mHandle && !mHandle.done() && mExeIter.has_value());
    // mExeIter has been removed from mExecuteQueue before enter Resume().
    mExeIter.reset();
    mHandle.resume();
}

//  Awaiter for All: waits all, returns tuple<T1, T2, T3 ...>
//
template <typename... Ts>
class All : public internal::CoroAwaiterBase
{
private:
    std::tuple<Async<Ts>...>                     mWaitedCoros;
    std::size_t                                  mRemainingCount;
    std::coroutine_handle<internal::PromiseBase> mParentHandle;

public:
    All(Async<Ts>&&... cs)
        : mWaitedCoros(std::move(cs)...), mRemainingCount(sizeof...(Ts))
    {
    }

    bool await_ready() const noexcept
    {
        return mRemainingCount == 0;
    }

    template <typename T>
    void await_suspend(std::coroutine_handle<internal::Promise<T>> h) noexcept
    {
        mParentHandle = std::coroutine_handle<internal::PromiseBase>::from_address(h.address());

        auto resumeWithIndexes = [this]<std::size_t... Is>(std::index_sequence<Is...>) {
            (
                [this] {
                    auto& coro = std::get<Is>(mWaitedCoros);
                    auto  handle = coro.getTypedHandle();
                    auto& promise = handle.promise();
                    promise.SetCoroManager(mParentHandle.promise().GetCoroManager());
                    promise.SetParentAwaiter(this);
                    handle.resume();
                }(),
                ...);
        };

        resumeWithIndexes(std::index_sequence_for<Ts...>{});
    }

    auto await_resume()
    {
        std::tuple<internal::RetConvert<Ts>...> results;

        auto storeResults = [this, &results]<std::size_t... Is>(std::index_sequence<Is...>) {
            ([this, &results] {
                auto& coro = std::get<Is>(mWaitedCoros);
                auto typedHandle = coro.getTypedHandle();
                using T    = std::tuple_element_t<Is, std::tuple<Ts...>>;
                if constexpr (std::is_void_v<T>)
                {
                    typedHandle.promise().TakeResult();
                    std::get<Is>(results) = std::monostate{};
                }
                else
                {
                    std::get<Is>(results) = std::move(typedHandle.promise().TakeResult());
                }
            }(),
             ...);
        };

        storeResults(std::index_sequence_for<Ts...>{});
        return std::move(results);
    }

    std::coroutine_handle<> OnWaitComplete(std::coroutine_handle<> h) noexcept override
    {
        if (--mRemainingCount == 0)
            return mParentHandle;
        else
            return std::noop_coroutine();
    }
};

//  Awaiter for Any: waits first, returns tuple<optional<T1>, optional<T2>, optional<T2>...>
//
template <typename... Ts>
class Any : public internal::CoroAwaiterBase
{
private:
    std::tuple<Async<Ts>...>                                    mWaitedCoros;
    std::coroutine_handle<>                                     mFirstFinish;
    std::tuple<std::optional<internal::RetConvert<Ts>>...>      mResults;
    std::coroutine_handle<internal::PromiseBase>                mParentHandle;
    bool                                                        mSuspended = false;

public:
    Any(Async<Ts>&&... cs)
        : mWaitedCoros(std::move(cs)...), mResults()
    {
    }

    bool await_ready() const noexcept
    {
        return false;
    }

    template <typename T>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<internal::Promise<T>> h) noexcept
    {
        mParentHandle = std::coroutine_handle<internal::PromiseBase>::from_address(h.address());

        auto resumeWithIndexes = [this]<std::size_t... Is>(std::index_sequence<Is...>) {
            ([this] {
                if (mFirstFinish) 
                    return; // A child already completed, skip remaining
                auto& coro    = std::get<Is>(mWaitedCoros);
                auto handle = coro.getTypedHandle();
                auto& promise = handle.promise();
                promise.SetCoroManager(mParentHandle.promise().GetCoroManager());
                promise.SetParentAwaiter(this);
                handle.resume();
            }(),
             ...);
        };
        resumeWithIndexes(std::index_sequence_for<Ts...>{});

        if (mFirstFinish)
            return mParentHandle;

        mSuspended = true;
        return std::noop_coroutine();
    }

    auto await_resume()
    {
        auto checkStoreWithIndexes = [this]<std::size_t... Is>(std::index_sequence<Is...>) {
            ([this] {
                auto& coro = std::get<Is>(mWaitedCoros);
                auto handle = coro.getTypedHandle();
                if (handle.address() != mFirstFinish.address())
                    return;

                using T = std::tuple_element_t<Is, std::tuple<Ts...>>;
                if constexpr (std::is_void_v<T>)
                {
                    // To trigger the exception if any
                    handle.promise().TakeResult();
                    std::get<Is>(mResults) = std::monostate{};
                }
                else
                {
                    std::get<Is>(mResults) = std::move(handle.promise().TakeResult());
                }
            }(),
             ...);
        };
        checkStoreWithIndexes(std::index_sequence_for<Ts...>{});

        return mResults;
    }

    std::coroutine_handle<> OnWaitComplete(std::coroutine_handle<> h) noexcept override
    {
        mFirstFinish = h;
        if (mSuspended)
            return mParentHandle;
        else
            return std::noop_coroutine();
    }
};

} // namespace tokoro

#include "internal/promise.inl"

namespace tokoro
{

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
Async<void> WaitUntilBP(std::function<bool()>&& checkFunc)
{
    while (!checkFunc())
    {
        co_await WaitBP<UpdateEnum, TimeEnum>(internal::GetEnumDefault<UpdateEnum>());
    }
}

template <internal::CountEnum UpdateEnum, internal::CountEnum TimeEnum>
Async<void> WaitWhileBP(std::function<bool()>&& checkFunc)
{
    while (checkFunc())
    {
        co_await WaitBP<UpdateEnum, TimeEnum>(internal::GetEnumDefault<UpdateEnum>());
    }
}

// Define preset types for quick setup.
//
using Scheduler       = SchedulerBP<internal::PresetUpdateType, internal::PresetTimeType>;
using Wait            = WaitBP<internal::PresetUpdateType, internal::PresetTimeType>;
inline auto WaitUntil = WaitUntilBP<internal::PresetUpdateType, internal::PresetTimeType>;
inline auto WaitWhile = WaitWhileBP<internal::PresetUpdateType, internal::PresetTimeType>;

} // namespace tokoro
