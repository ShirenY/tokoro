#pragma once

#include <atomic>
#include <memory>
#include <coroutine>

namespace tokoro::internal
{

class PromiseBase;

// Phase-based state machine for coordinating Send() and await_suspend across threads.
//
// Transitions:
//   Init ──(await_suspend)──> Suspended
//   Init ──(Send)──────────> Signaled
//   Init ──(~CallbackBP)──> Cancelled
//
// The CAS loser (second to arrive) is responsible for pushing the SignaledNode.
// This ensures exactly one push, even under concurrent access.
enum class CallbackPhase : int
{
    Init      = 0,
    Suspended = 1, // await_suspend won the CAS
    Signaled  = 2, // Send() won the CAS
    Cancelled = 3, // Destructor won the CAS (coroutine stopped before Send)
};

struct CallbackStateBase
{
    std::atomic<CallbackPhase> phase{CallbackPhase::Init};
    std::atomic<bool>          cancelled{false}; // Set when destructor loses the CAS race

    virtual ~CallbackStateBase() = default;
};

struct SignaledNode
{
    std::shared_ptr<CallbackStateBase>         state;
    std::coroutine_handle<PromiseBase>         handle;
    int                                        queueIndex;
    SignaledNode*                               next;
};

class SignaledStack
{
public:
    SignaledStack() = default;

    SignaledStack(const SignaledStack&)            = delete;
    SignaledStack& operator=(const SignaledStack&) = delete;

    ~SignaledStack()
    {
        auto* node = mHead.load(std::memory_order_relaxed);
        while (node)
        {
            auto* next = node->next;
            delete node;
            node = next;
        }
    }

    void Push(SignaledNode* node) noexcept
    {
        node->next = mHead.load(std::memory_order_relaxed);
        while (!mHead.compare_exchange_weak(
            node->next, node,
            std::memory_order_release,
            std::memory_order_relaxed))
        {
        }
    }

    SignaledNode* DrainAll() noexcept
    {
        return mHead.exchange(nullptr, std::memory_order_acquire);
    }

private:
    std::atomic<SignaledNode*> mHead{nullptr};
};

template <typename T>
struct CallbackSharedState : CallbackStateBase
{
    T                                          value{};
    std::coroutine_handle<PromiseBase>         handle;
    int                                        queueIndex = -1;
    std::shared_ptr<SignaledStack>              stack;
};

template <>
struct CallbackSharedState<void> : CallbackStateBase
{
    std::coroutine_handle<PromiseBase>         handle;
    int                                        queueIndex = -1;
    std::shared_ptr<SignaledStack>              stack;
};

template <typename T>
class Sender;

template <typename T>
class Sender
{
public:
    Sender() = default;

    explicit Sender(std::shared_ptr<CallbackSharedState<T>> state)
        : mState(std::move(state))
    {
    }

    void Send(T value) const
    {
        if (!mState)
            return;

        mState->value = std::move(value);

        // Try to transition Init → Signaled.
        auto expected = CallbackPhase::Init;
        if (mState->phase.compare_exchange_strong(
                expected, CallbackPhase::Signaled,
                std::memory_order_seq_cst,
                std::memory_order_seq_cst))
        {
            // We won: Send() ran before await_suspend.
            // Don't push — await_ready() will return true, or await_suspend will push.
            return;
        }

        // We lost: phase was Suspended (await_suspend already ran).
        // Push the node — we're the second to arrive.
        if (expected == CallbackPhase::Suspended)
        {
            auto* node = new SignaledNode{mState, mState->handle, mState->queueIndex, nullptr};
            mState->stack->Push(node);
        }
        // If phase was Cancelled or Signaled, do nothing.
    }

private:
    std::shared_ptr<CallbackSharedState<T>> mState;
};

template <>
class Sender<void>
{
public:
    Sender() = default;

    explicit Sender(std::shared_ptr<CallbackSharedState<void>> state)
        : mState(std::move(state))
    {
    }

    void Send() const
    {
        if (!mState)
            return;

        auto expected = CallbackPhase::Init;
        if (mState->phase.compare_exchange_strong(
                expected, CallbackPhase::Signaled,
                std::memory_order_seq_cst,
                std::memory_order_seq_cst))
        {
            return;
        }

        if (expected == CallbackPhase::Suspended)
        {
            auto* node = new SignaledNode{mState, mState->handle, mState->queueIndex, nullptr};
            mState->stack->Push(node);
        }
    }

private:
    std::shared_ptr<CallbackSharedState<void>> mState;
};

} // namespace tokoro::internal
