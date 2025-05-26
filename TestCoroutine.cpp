#include "tokoro.h"
#include <cassert>
#include <iostream>
#include <vector>

using namespace tokoro;

Coro<int> DelayedValue(int value, double delaySeconds)
{
    co_await Wait(delaySeconds);
    co_return value;
}

Coro<void> Delayed(double delaySeconds)
{
    co_await Wait(delaySeconds);
    co_return;
}

// Simple helper to drive a scheduler until a condition or max frames
void RunSchedulerUntil(Scheduler& sched, std::function<bool()> done, int maxIterations = 1000000)
{
    int count = 0;
    while (!done() && count++ < maxIterations)
    {
        sched.Update();
    }
    assert(done() && "Scheduler did not finish in time");
}

// Test awaiting a single coroutine with a return value
void TestSingleAwaitValue()
{
    Scheduler sched;
    bool      completed = false;
    int       result    = 0;

    auto h = sched.Start([&]() -> Coro<void> {
        result    = co_await DelayedValue(42, 0.0);
        completed = true;
    });

    RunSchedulerUntil(sched, [&] { return completed; });
    assert(completed);
    assert(result == 42);
    assert(h.IsDown());
    std::cout << "TestSingleAwaitValue passed\n";
}

// Test awaiting a single void coroutine
void TestSingleAwaitVoid()
{
    Scheduler sched;
    bool      completed = false;

    auto h = sched.Start([&]() -> Coro<void> {
        co_await Delayed(0.0);
        completed = true;
    });

    RunSchedulerUntil(sched, [&] { return completed; });
    assert(completed);
    assert(h.IsDown());
    std::cout << "TestSingleAwaitVoid passed\n";
}

// Test All combinator
void TestAllCombinator()
{
    Scheduler sched;
    bool      completed = false;
    int       a = 0, b = 0, c = 0;

    auto h = sched.Start([&]() -> Coro<void> {
        std::tie(a, b, c) = co_await All(
            DelayedValue(1, 0.0),
            DelayedValue(2, 0.0),
            DelayedValue(3, 0.0));
        completed = true;
    });

    RunSchedulerUntil(sched, [&] { return completed; });
    assert(completed);
    assert(a == 1 && b == 2 && c == 3);
    assert(h.IsDown());
    std::cout << "TestAllCombinator passed\n";
}

// Test Any combinator
void TestAnyCombinator()
{
    Scheduler          sched;
    bool               completed = false;
    std::optional<int> a, b;

    auto h = sched.Start([&]() -> Coro<void> {
        auto tup = co_await Any(
            DelayedValue(10, 0.02),
            DelayedValue(20, 0.0));
        a         = std::get<0>(tup);
        b         = std::get<1>(tup);
        completed = true;
    });

    RunSchedulerUntil(sched, [&] { return completed; });
    assert(completed);
    assert(!a.has_value() && b.has_value() && b.value() == 20);
    assert(h.IsDown());
    std::cout << "TestAnyCombinator passed\n";
}

// Recursive Fibonacci coroutine
Coro<int> Fib(int n)
{
    if (n < 2)
        co_return n;
    auto a  = Fib(n - 1);
    auto b  = Fib(n - 2);
    int  ra = co_await a;
    int  rb = co_await b;
    co_return ra + rb;
}

// Stress test: spawn many coroutines computing Fibonacci and cancel some
void TestStress(size_t count, int fibN)
{
    Scheduler                    sched;
    std::vector<CoroHandle<int>> handles;
    handles.reserve(count);

    // Start coroutines
    for (size_t i = 0; i < count; ++i)
    {
        auto h = sched.Start([fibN]() -> Coro<int> {
            co_return co_await Fib(fibN);
        });
        handles.push_back(std::move(h));
    }

    // Cancel half
    for (size_t i = 0; i < count; i += 2)
    {
        handles[i].Stop();
    }

    // Drive scheduler until remaining complete
    auto done = [&]() {
        for (size_t i = 1; i < count; i += 2)
        {
            if (!handles[i].IsDown())
                return false;
        }
        return true;
    };
    RunSchedulerUntil(sched, done, 10000000);

    // Verify results
    for (size_t i = 1; i < count; i += 2)
    {
        auto r = handles[i].GetReturn();
        assert(r.has_value());
        // Fibonacci correctness for small fibN
    }
    std::cout << "TestStress(" << count << ", " << fibN << ") passed\n";
}

// Test NextFrame ordering
void TestNextFrame()
{
    Scheduler sched;
    int       count = 0;

    auto h = sched.Start([&]() -> Coro<void> {
        co_await NextFrame(); // resume 1
        count += 1;
        co_await NextFrame(); // resume 2
        count += 2;
    });

    // Before any update, count==0
    assert(count == 0);
    sched.Update(); // first resume
    assert(count == 1);
    sched.Update(); // second resume
    assert(count == 3);
    assert(h.IsDown());
    std::cout << "TestNextFrame passed\n";
}

// Test Stop and cancellation
void TestStop()
{
    Scheduler sched;
    int       loops = 0;

    auto h = sched.Start([&]() -> Coro<void> {
        while (true)
        {
            co_await NextFrame();
            loops++;
        }
    });

    // run a few frames
    for (int i = 0; i < 5; ++i)
        sched.Update();
    assert(loops == 5);

    assert(!h.IsDown());
    h.Stop();
    assert(h.IsDown());
    sched.Update();
    assert(loops == 5);
    std::cout << "TestStop passed\n";
}

// Test global scheduler and GetReturn
void TestGlobalScheduler()
{
    bool done   = false;
    auto handle = GlobalScheduler().Start([&]() -> Coro<int> {
        co_await Wait(0.0);
        co_return 123;
    });

    for (int i = 0; i < 10 && !handle.IsDown(); ++i)
    {
        GlobalScheduler().Update();
    }
    assert(handle.IsDown());
    auto ret = handle.GetReturn();
    assert(ret.has_value() && ret.value() == 123);
    std::cout << "TestGlobalScheduler passed\n";
}

int main()
{
    TestSingleAwaitValue();
    TestSingleAwaitVoid();
    TestAllCombinator();
    TestAnyCombinator();
    TestNextFrame();
    TestStop();
    TestStress(10000, 10); // ����ѹ������������ Fibonacci ���
    TestGlobalScheduler();

    std::cout << "All tests passed successfully." << std::endl;
    return 0;
}
