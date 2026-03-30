#include "tokoro.h"
#include <cassert>
#include <iostream>
#include <source_location>
#include <thread>
#include <vector>

using namespace tokoro;

Async<int> DelayedValue(int value, double delaySeconds)
{
    co_await Wait(delaySeconds);
    co_return value;
}

Async<void> Delayed(double delaySeconds)
{
    co_await Wait(delaySeconds);
    co_return;
}

// Test awaiting a single coroutine with a return value
void TestSingleAwaitValue()
{
    Scheduler sched;
    bool      completed = false;
    int       result    = 0;

    auto h = sched.Start([&]() -> Async<void> {
        result    = co_await DelayedValue(42, 0.0);
        completed = true;
    });

    // Drive scheduler until completed or timeout
    for (int iter = 0; iter < 1000000 && !completed; ++iter)
    {
        sched.Update();
    }
    assert(completed && "Scheduler did not finish in time");
    assert(result == 42);
    assert(h.GetState().value() == AsyncState::Succeed);
    std::cout << "TestSingleAwaitValue passed\n";
}

// Test awaiting a single void coroutine
void TestSingleAwaitVoid()
{
    Scheduler sched;
    bool      completed = false;

    auto h = sched.Start([&]() -> Async<void> {
        co_await Delayed(0.0);
        completed = true;
    });

    // Drive scheduler until completed or timeout
    for (int iter = 0; iter < 1000000 && !completed; ++iter)
    {
        sched.Update();
    }
    assert(completed && "Scheduler did not finish in time");
    assert(h.GetState().value() == AsyncState::Succeed);
    std::cout << "TestSingleAwaitVoid passed\n";
}

// Test All combinator
void TestAllCombinator()
{
    Scheduler      sched;
    bool           completed = false;
    int            a = 0, b = 0, c = 0;
    std::monostate d;

    auto h = sched.Start([&]() -> Async<void> {
        std::tie(a, b, c, d) = co_await All(
            DelayedValue(1, 0.0),
            DelayedValue(2, 0.001),
            DelayedValue(3, 0.0),
            Delayed(0.0));
        completed = true;
    });

    // Drive scheduler until completed or timeout
    for (int iter = 0; iter < 1000000 && !completed; ++iter)
    {
        sched.Update();
    }
    assert(completed && "Scheduler did not finish in time");
    assert(a == 1 && b == 2 && c == 3);
    assert(h.GetState().value() == AsyncState::Succeed);
    std::cout << "TestAllCombinator passed\n";
}

// Test Any combinator
void TestAnyCombinator()
{
    Scheduler          sched;
    bool               completed = false;
    std::optional<int> a, b;

    auto h = sched.Start([&]() -> Async<void> {
        auto tup = co_await Any(DelayedValue(10, 10),
                                DelayedValue(20, 0.0));

        a = std::get<0>(tup);
        b = std::get<1>(tup);

        co_await Any(DelayedValue(10, 10), Delayed(0.000000000000000001));

        completed = true;
    });

    // Drive scheduler until completed or timeout
    for (int iter = 0; iter < 1000000 && !completed; ++iter)
    {
        sched.Update();
    }
    assert(completed && "Scheduler did not finish in time");
    assert(!a.has_value() && b.has_value() && b.value() == 20);
    assert(h.GetState().value() == AsyncState::Succeed);
    std::cout << "TestAnyCombinator passed\n";
}

// Test NextFrame ordering
void TestNextFrame()
{
    Scheduler sched;
    int       count = 0;

    auto h = sched.Start([&]() -> Async<void> {
        co_await Wait(); // resume 1
        count += 1;
        co_await Wait(); // resume 2
        count += 2;
    });

    // Before any update, count==0
    assert(count == 0);
    sched.Update(); // first resume
    assert(count == 1);
    sched.Update(); // second resume
    assert(count == 3);
    assert(h.GetState().value() == AsyncState::Succeed);
    std::cout << "TestNextFrame passed\n";
}

// Test Stop and cancellation
void TestStop()
{
    Scheduler sched;
    int       loops = 0;

    auto h = sched.Start([&]() -> Async<void> {
        while (true)
        {
            co_await Wait();
            loops++;
        }
    });

    // run a few frames
    for (int i = 0; i < 5; ++i)
        sched.Update();
    assert(loops == 5);

    assert(h.GetState().value() == AsyncState::Running);
    h.Stop();
    assert(h.GetState().value() == AsyncState::Stopped);
    sched.Update();
    assert(loops == 5);
    std::cout << "TestStop passed\n";
}

void TestUseHandleAfterSchedulerDestroyed()
{
    Scheduler* sched = new Scheduler();

    auto h1 = sched->Start([&]() -> Async<int> {
        co_await Wait(0.00000000001);
        co_return 123;
    });

    auto h2 = sched->Start([&]() -> Async<void> {
        co_await Wait(0.00000000001);
        co_return;
    });

    for (int iter = 0; iter < 1000000000 && h1.IsRunning(); ++iter)
    {
        sched->Update();
    }

    assert(h1.TakeResult().has_value());
    assert(h1.GetState().value() == AsyncState::Succeed);
    assert(h2.GetState().value() == AsyncState::Succeed);

    delete sched;

    assert(!h1.TakeResult().has_value());
    assert(!h1.GetState().has_value());

    // Call Async<void>'s TakeResult() to check whether it works as expect.
    // It returns nothing but can throw exceptions if there is.
    h2.TakeResult();

    std::cout << "TestUseHandleAfterSchedulerDestroyed passed\n";
}

void TestStartInCoroutine()
{
    Scheduler sched;
    int       frame = 0;

    sched.Start([&]() -> Async<void> {
             co_await Wait();

             auto innerCoro = [&]() -> Async<void> {
                 co_await Wait();
                 // Inner coroutine should always resume in next frame
                 assert(frame == 1);
             };

             // Star more than 2 to make sure some inner coros insert after outer coros
             sched.Start(innerCoro).Forget();
             sched.Start(innerCoro).Forget();
             sched.Start(innerCoro).Forget();
             sched.Start(innerCoro).Forget();

             assert(frame == 0);
         })
        .Forget();

    // One more outer coro to make sure the first update won't exceed immediately.
    sched.Start([&]() -> Async<void> {
             co_await Wait();
             assert(frame == 0);
         })
        .Forget();

    for (; frame < 5; ++frame)
    {
        sched.Update();
    }

    std::cout << "TestStartInCoroutine passed\n";
}

static Scheduler& GlobalScheduler()
{
    static Scheduler s;
    return s;
}

// Test global scheduler and GetReturn
void TestGlobalScheduler()
{
    auto handle = GlobalScheduler().Start([&]() -> Async<int> {
        co_await Wait(0.0);
        co_return 123;
    });

    // Drive global scheduler until done or timeout
    for (int iter = 0; iter < 10 && handle.GetState().value() != AsyncState::Succeed; ++iter)
    {
        GlobalScheduler().Update();
    }
    assert(handle.GetState().value() == AsyncState::Succeed);
    auto ret = handle.TakeResult();
    assert(ret.has_value() && ret.value() == 123);
    std::cout << "TestGlobalScheduler passed\n";
}

// A test to make sure no extra object constructed or copied.
//
class LifeTimeChecker
{
public:
    std::string name;

    inline static int ConstructCount = 0;
    inline static int CopyCount = 0;
    inline static int MoveCount = 0;
    inline static int CopyAssignCount = 0;
    inline static int MoveAssignCount = 0;
    inline static int DestructCount = 0;

    static void ClearCounts()
    {
        ConstructCount = 0;
        CopyCount = 0;
        MoveCount = 0;
        CopyAssignCount = 0;
        MoveAssignCount = 0;
        DestructCount = 0;
    }

    static bool IsBalanced()
    {
        return (ConstructCount + CopyCount + MoveCount ) == DestructCount;
    }

    // LCOV_EXCL_START This function should never be used. Prepared for incorrect construction.
    LifeTimeChecker(const std::source_location& location = std::source_location::current())
        : name("default")
    {
        ++ConstructCount;
        log("Default Constructor", location);
    }
    // LCOV_EXCL_STOP

    LifeTimeChecker(const std::string& n, const std::source_location& location = std::source_location::current())
        : name(n)
    {
        ++ConstructCount;
        log("Custom Constructor", location);
    }

    LifeTimeChecker(const LifeTimeChecker& other,
        const std::source_location& location = std::source_location::current())
        : name(other.name + "-c")
    {
        ++CopyCount;
        log("Copy Constructor", location);
    }

    LifeTimeChecker(LifeTimeChecker&& other,
        const std::source_location& location = std::source_location::current()) noexcept
        : name(std::move(other.name))
    {
        ++MoveCount;
        log("Move Constructor", location);
    }

    LifeTimeChecker& operator=(const LifeTimeChecker& other)
    {
        ++CopyAssignCount;
        log("Copy Assignment", std::source_location::current());
        if (this != &other)
        {
            name = other.name + "-c";
        }
        return *this;
    }

    LifeTimeChecker& operator=(LifeTimeChecker&& other) noexcept
    {
        ++MoveAssignCount;
        log("Move Assignment", std::source_location::current());
        if (this != &other)
        {
            name = std::move(other.name);
        }
        return *this;
    }

    ~LifeTimeChecker()
    {
        ++DestructCount;
        log("Destructor", std::source_location::current());
    }

private:
    void log(const std::string& action, const std::source_location& location) const
    {
        // std::cout << "[" << action << "] "
        //           << "Object: " << name << " | "
        //           << "At: " << location.file_name() << ":" << location.line()
        //           << " in " << location.function_name() << "\n";
    }
};

// TestCustomUpdateAndTimers
//
enum class UpdateType
{
    Update = 0,
    PreUpdate,
    PostUpdate,
    Count,
};

enum class TimeType
{
    EmuRealTime = 0,
    GameTime,
    Count,
};

// Give alias names for ease of life.
// Note: You can still use Scheduler, Wait and Handle if you really like them.
// Just don't introduce 'using namespace tokoro' to your code.
using MyScheduler = SchedulerBP<UpdateType, TimeType>;
using MyWait      = WaitBP<UpdateType, TimeType>;

void TestCustomUpdateAndTimers()
{
    MyScheduler sched;

    double emuRealTime = 0;
    // Note: this timer is only for test,
    // in most applications the default timer is good enough for 'real time'
    sched.SetCustomTimer(TimeType::EmuRealTime, [&]() -> double { return emuRealTime; });

    double gameTime = 0;
    sched.SetCustomTimer(TimeType::GameTime, [&]() -> double { return gameTime; });

    bool gamePaused = false;

    // Help variable for reality checking
    UpdateType curUpdateType = UpdateType::PreUpdate;

    // Define the test coroutine
    Handle handle = sched.Start([&]() -> Async<void> {
        // Check wait in realtime
        co_await MyWait(1);
        assert(emuRealTime >= 1);

        // Check the ability to switch between updates.
        co_await MyWait(0, UpdateType::PreUpdate);
        assert(curUpdateType == UpdateType::PreUpdate);

        co_await MyWait(0, UpdateType::Update);
        assert(curUpdateType == UpdateType::Update);

        co_await MyWait(0, UpdateType::PostUpdate);
        assert(curUpdateType == UpdateType::PostUpdate);

        // Start another coro to stop and start the game time.
        sched.Start([&]() -> Async<void> {
                 gamePaused = true;
                 co_await MyWait(2);
                 gamePaused = false;
             })
            .Forget();

        const double saveGameTime = gameTime;
        co_await MyWait(0, UpdateType::Update, TimeType::GameTime); // Wait one game frame
        // Game time should be still paused.
        // Please note this compare assumes EmuRealTime update is before GameTime. Or there will be one frame time diff.
        assert(saveGameTime == gameTime);

        // Wait until the game time start to move again.
        co_await MyWait(0.1, UpdateType::Update, TimeType::GameTime);
        assert(saveGameTime < gameTime);
        assert(gameTime < emuRealTime);

        // Check the ability to switch between updates ub game time.
        co_await MyWait(0, UpdateType::PreUpdate, TimeType::GameTime);
        assert(curUpdateType == UpdateType::PreUpdate);

        co_await MyWait(0, UpdateType::Update, TimeType::GameTime);
        assert(curUpdateType == UpdateType::Update);

        co_await MyWait(0, UpdateType::PostUpdate, TimeType::GameTime);
        assert(curUpdateType == UpdateType::PostUpdate);
    });

    // Game Loop
    //
    constexpr double frameTime = 0.166666;
    for (int i = 0; i < 100 && handle.IsRunning(); ++i)
    {
        emuRealTime += frameTime;
        if (!gamePaused)
            gameTime += frameTime;

        curUpdateType = UpdateType::PreUpdate;
        sched.Update(UpdateType::PreUpdate, TimeType::EmuRealTime);
        sched.Update(UpdateType::PreUpdate, TimeType::GameTime);

        curUpdateType = UpdateType::Update;
        sched.Update(UpdateType::Update, TimeType::EmuRealTime);
        sched.Update(UpdateType::Update, TimeType::GameTime);

        curUpdateType = UpdateType::PostUpdate;
        sched.Update(UpdateType::PostUpdate, TimeType::EmuRealTime);
        sched.Update(UpdateType::PostUpdate, TimeType::GameTime);
    }

    // task should finish in time
    assert(handle.GetState().value() == AsyncState::Succeed);
    std::cout << "TestCustomUpdateAndTimers passed\n";
}

void TestWaitUntilAndWhile()
{
    Scheduler sched;
    int       frame = 0;

    sched.Start([&]() -> Async<void> {
             co_await WaitUntil([&]() { return frame == 10; });
             assert(frame == 10);

             co_await WaitWhile([&]() { return frame < 20; });
             assert(frame == 20);
         })
        .Forget();

    for (; frame < 100; ++frame)
    {
        sched.Update();
    }

    std::cout << "TestWaitUntilAndWhile passed\n";
}

Async<void> WaitForFrames(int frameCount)
{
    for (int i = 0; i < frameCount; ++i)
    {
        co_await Wait();
    }
}

void TestThrowException()
{
    static constexpr char message1[] = "test coroutine exception!";
    static constexpr char message2[] = "test nested exception !";
    static constexpr char message3[] = "test cache nested coro exception!";
    static constexpr char message4[] = "test throw under any!";
    static constexpr char message5[] = "test throw under all!";

    Scheduler sched;

    // Test throw from a coro and catch exception from TakeResult.
    auto h1 = sched.Start([&]() -> Async<int> {
        co_await Wait();
        throw std::runtime_error(message1);
        co_return 1;
    });

    // Test throw from nested coros and catch exception from TakeResult.
    auto h2 = sched.Start([&]() -> Async<void> {
        co_await Wait();
        co_await []() -> Async<void> {
            co_await Wait();
            throw std::runtime_error(message2);
        }();
    });

    // Skip this test with clang on windows:
    // https://github.com/llvm/llvm-project/issues/143235
#if !(defined(_WIN32) && defined(__clang__))
    // Test throw from nested coros and catch in upper coro
    auto h3 = sched.Start([&]() -> Async<void> {
        co_await Wait();

        try
        {
            co_await []() -> Async<void> {
                co_await Wait();

                co_await []() -> Async<void> {
                    co_await Wait();
                    throw std::runtime_error(message3);
                }();
            }();
        }
        catch (std::runtime_error e)
        {
            const std::string errMsg = e.what();
            assert(errMsg == message3);
        }
    });
#endif

    // Test throw exception under any.
    auto h4 = sched.Start([&]() -> Async<void> {
        co_await Wait();

        co_await Any(WaitForFrames(10),
                     WaitForFrames(3),
                     []() -> Async<void> {
                         co_await Wait();
                         throw std::runtime_error(message4);
                     }());
    });

    auto h5 = sched.Start([&]() -> Async<void> {
        co_await Wait();

        co_await All(WaitForFrames(3),
                     WaitForFrames(3),
                     []() -> Async<void> {
                         co_await Wait();
                         throw std::runtime_error(message5);
                     }());
    });

    // The game loop
    for (int i = 0; i < 5; ++i)
    {
        sched.Update();
    }

    try
    {
        auto ret = h1.TakeResult();
        // The code should never reach here because exception rethrow by GetReturn();
        assert(false); // LCOV_EXCL_LINE
    }
    catch (std::runtime_error e)
    {
        const std::string errMsg = e.what();
        assert(errMsg == message1);
    }

    try
    {
        h2.TakeResult();
        // The code should never reach here because exception rethrow by GetReturn();
        assert(false); // LCOV_EXCL_LINE
    }
    catch (std::runtime_error e)
    {
        const std::string errMsg = e.what();
        assert(errMsg == message2);
    }

#if !(defined(_WIN32) && defined(__clang__))
    try
    {
        h3.TakeResult();
    }
    catch (std::runtime_error e) // LCOV_EXCL_LINE
    {
        // The code should never reach here because the exception has been catch in the root coroutine.
        assert(false); // LCOV_EXCL_LINE
    } // LCOV_EXCL_LINE
#endif

    try
    {
        h4.TakeResult();
        // The code should never reach here because exception rethrow by GetReturn();
        assert(false); // LCOV_EXCL_LINE
    }
    catch (std::runtime_error e)
    {
        const std::string errMsg = e.what();
        assert(errMsg == message4);
    }

    try
    {
        h5.TakeResult();
        // The code should never reach here because exception rethrow by GetReturn();
        assert(false); // LCOV_EXCL_LINE
    }
    catch (std::runtime_error e)
    {
        const std::string errMsg = e.what();
        assert(errMsg == message5);
    }

    std::cout << "TestThrowException passed\n";
}

void TestHandle()
{
    Handle<int> moveHandle;
    assert(!moveHandle.IsValid());
    assert(!moveHandle.IsRunning());
    assert(!moveHandle.GetState().has_value());
    assert(!moveHandle.TakeResult().has_value());
    moveHandle.Stop(); // Nothing will happen

    Handle<void> moveVoidHandle;
    assert(!moveVoidHandle.IsValid());
    assert(!moveVoidHandle.IsRunning());
    assert(!moveVoidHandle.GetState().has_value());
    moveVoidHandle.TakeResult(); // Nothing will happen

    {
        Scheduler sched;

        moveHandle = sched.Start([]() -> Async<int> {
            co_await Wait();
            co_return 42;
        });

        assert(moveHandle.IsValid());

        Handle noyieldHandle = sched.Start([]() -> Async<void> {
            co_return;
        });

        assert(noyieldHandle.IsValid());
        assert(!noyieldHandle.IsRunning());
        assert(noyieldHandle.GetState().has_value());
        assert(noyieldHandle.GetState().value() == AsyncState::Succeed);
        noyieldHandle.Stop();
        assert(noyieldHandle.GetState().value() != AsyncState::Stopped);

        Handle neverEndHandle = sched.Start([]() -> Async<void> {
            while (true)
            {
                co_await Wait();
            }
        });

        constexpr static char cstr[] = "hello world";

        Handle retHandle = sched.Start([]() -> Async<std::string> {
            co_await Wait();

            std::string msg = co_await []() -> Async<std::string> {
                co_await Wait();
                co_return cstr;
            }();

            co_return msg;
        });
        assert(!retHandle.TakeResult().has_value());

        constexpr static char err[] = "exceptHandle exception";

        // Immediately exception coroutine test.
        Handle exceptHandle = sched.Start([]() -> Async<void> {
            throw std::runtime_error(err);
            co_return;
        });
        assert(!exceptHandle.IsRunning());
        try
        {
            exceptHandle.TakeResult();
            assert(false && "This line should never execute."); // LCOV_EXCL_LINE
        }
        catch (std::runtime_error e)
        {
            assert(e.what() == std::string(err));
        }
        try
        {
            exceptHandle.TakeResult(); // Second TakeResult() call
        }
        catch (std::runtime_error e) // LCOV_EXCL_LINE
        {
            // Second TakeResult should not have exception throw.
            assert(false && "This line should never execute."); // LCOV_EXCL_LINE
        } // LCOV_EXCL_LINE

        // Forget and not forget test
        int forgetResult    = 0;
        int notForgetResult = 0;
        {
            auto forgetHandle1 = sched.Start([&]() -> Async<void> {
                co_await Wait();
                forgetResult++;
            });

            forgetHandle1.Forget();

            // Do some move to make sure forget works with move.
            auto forgetHandle2(std::move(forgetHandle1));
            forgetHandle1 = std::move(forgetHandle2);

            auto notForgetHandle1 = sched.Start([&]() -> Async<void> {
                co_await Wait();
                notForgetResult++;
            });

            // Do some move to make sure forget works with move.
            auto notForgetHandle2(std::move(notForgetHandle1));
            notForgetHandle1 = std::move(notForgetHandle2);
        }

        for (int i = 0; i < 10; ++i)
        {
            sched.Update();
        }

        assert(!retHandle.IsRunning());
        assert(retHandle.TakeResult() == cstr);
        assert(!retHandle.TakeResult().has_value() && "Call TakeResult() on same handle returns nullopt.");

        assert(neverEndHandle.IsValid());
        assert(neverEndHandle.IsRunning());
        neverEndHandle.TakeResult(); // Nothing will happen
        neverEndHandle.Stop();
        assert(*neverEndHandle.GetState() == AsyncState::Stopped);
        neverEndHandle.TakeResult(); // Nothing will happen

        assert(moveHandle.GetState().value() == AsyncState::Succeed);

        assert(forgetResult != 0);
        assert(notForgetResult == 0);
    }

    assert(!moveHandle.IsRunning());
    assert(!moveHandle.GetState().has_value() && "Handle became invalid because the scheduler go out of scope.");
    assert(!moveHandle.TakeResult().has_value());
    moveHandle.Stop(); // Nothing will happen.

    std::cout << "TestHandle passed\n";
}

// Member function test
void TestMemberCoroutines()
{
    class Test
    {
    public:
        void StartCount()
        {
            GlobalScheduler().Start(&Test::CountCoro, this, 3, 2).Forget();
        }
        int value = 0;

    private:
        Async<void> CountCoro(int countTimes, int step)
        {
            for (int i = 0; i < countTimes; ++i)
            {
                co_await Wait();
                value += step;
            }
        }
    };

    Test test;
    test.StartCount();

    for (int i = 0; i < 10; ++i)
    {
        GlobalScheduler().Update();
    }

    assert(test.value == 6);
    std::cout << "TestMemberCoroutines passed\n";
}

void TestReturnObjLifetime()
{
    LifeTimeChecker::ClearCounts();

    auto handle = GlobalScheduler().Start([]() -> Async<LifeTimeChecker> {
        co_return LifeTimeChecker("A");
    });

    for (int i = 0; i < 10; ++i)
    {
        GlobalScheduler().Update();
    }

    LifeTimeChecker ret = handle.TakeResult().value();
    assert(LifeTimeChecker::ConstructCount == 1);
    assert(LifeTimeChecker::CopyCount == 0);

    LifeTimeChecker::ClearCounts();

    std::cout << "TestReturnObjLifetime passed\n";
}

class Rand
{
public:
    static void SetSeed(uint32_t seed)
    {
        mState = seed;
    }

    static uint32_t Int(uint32_t min, uint32_t max)
    {
        return Next() % (max - min) + min;
    }

    static float Float(float min, float max)
    {
        constexpr float invUInt32MaxPlus1 = 1.0f / 4294967296.0f;
        float           normalized        = Next() * invUInt32MaxPlus1;
        return min + normalized * (max - min);
    }

private:
    static uint32_t Next()
    {
        mState = mState * 1664525u + 1013904223u;
        return mState;
    }

    static uint32_t mState;
};

Async<uint32_t> FibCoro(uint32_t n)
{
    if (n < 2)
        co_return n;

    co_await Wait(Rand::Float(0.0f, 1.0f));

    auto a  = FibCoro(n - 1);
    auto b  = FibCoro(n - 2);
    int  ra = co_await a;
    int  rb = co_await b;
    co_return ra + rb;
}

int Fibonacci(int n)
{
    if (n <= 1)
        return n;

    int a = 0, b = 1;
    for (int i = 2; i <= n; ++i)
    {
        int temp = a + b;
        a        = b;
        b        = temp;
    }
    return b;
}

uint32_t Rand::mState = 0;

// Stress test: spawn many coroutines computing Fibonacci and cancel some
void StressTest(size_t count)
{
    double simTime = 0.0f;

    Scheduler sched;
    sched.SetCustomTimer(internal::PresetTimeType::Realtime, [&]() { return simTime; });

    std::vector<Handle<int>> handles;
    handles.reserve(count);

    uint32_t finished = 0;

    // Start coroutines
    for (size_t i = 0; i < count; ++i)
    {
        Rand::SetSeed(static_cast<uint32_t>(i));
        const auto fabi = Rand::Int(3, 11);

        auto h = sched.Start([&](uint32_t fabIndex) -> Async<int> {
            int rootValue = co_await FibCoro(fabIndex);
            finished++;
            co_return rootValue;
        },
                             fabi);

        handles.push_back(std::move(h));
    }

    // Test cancel half
    for (size_t i = 0; i < count; i += 2)
    {
        handles[i].Stop();
    }

    double maxUpdateTime = 0;
    auto   start         = std::chrono::high_resolution_clock::now();

    // Drive scheduler until remaining complete or timeout
    for (int iter = 0; iter < 10000000 && finished != count / 2; ++iter)
    {
        auto updateStart = std::chrono::high_resolution_clock::now();
        sched.Update();
        auto updateEnd = std::chrono::high_resolution_clock::now();

        const auto   timeGap       = std::chrono::duration_cast<std::chrono::microseconds>(updateEnd - updateStart).count();
        const double curUpdateTime = timeGap / 1000.0;
        if (maxUpdateTime < curUpdateTime)
            maxUpdateTime = curUpdateTime;

        simTime += 0.0166666666f;
    }

    std::cout << "max update time " << maxUpdateTime << "ms" << std::endl;

    auto end = std::chrono::high_resolution_clock::now();
    std::cout << "stress test time "
              << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() / 1000.0 << "ms" << std::endl;

    assert(finished == count / 2 && "Scheduler did not finish in time");

    // Verify results
    //
    std::array<uint32_t, 10> fibResults;
    for (int i = 0; i < 10; ++i)
    {
        fibResults[i] = Fibonacci(i);
    }

    for (size_t i = 1; i < count; i += 2)
    {
        auto r = handles[i].TakeResult();
        assert(r.has_value());

        Rand::SetSeed(static_cast<uint32_t>(i));
        const auto fabi = Rand::Int(3, 11);
        assert(r.value() == fibResults[fabi]);
    }

    std::cout << "TestStress(" << count << ") passed\n";
}

// Demonstrate use-after-free in Any::await_suspend when a non-last child completes synchronously.
//
// SyncValue() has no co_await, so it completes synchronously during handle.resume().
// When Is=0 (the first child) finishes:
//   1. OnWaitComplete returns mParentHandle → parent resumes via symmetric transfer
//   2. Parent calls Any::await_resume() → Any is destroyed (~Any runs, mWaitedCoros freed)
//   3. handle.resume() for Is=0 returns back to the fold loop
//   4. Fold loop continues to Is=1 → accesses already-destroyed mWaitedCoros
//
// Expected log order that proves the bug:
//   [Any@X] await_suspend: starting child Is=0
//   [Any@X] OnWaitComplete: child done, resuming parent
//   [Any@X] await_resume called
//   [Any@X] ~Any DESTROYED          ← Any is gone
//   [Any@X] await_suspend: child Is=0 handle.resume() returned   ← dangling this
//   [Any@X] await_suspend: starting child Is=1                   ← UAF: accessing destroyed mWaitedCoros
void TestAnySyncChildUAF()
{
    std::cout << "\n--- TestAnySyncChildUAF ---\n";

    Scheduler sched;
    bool      completed = false;

    // A coroutine with no co_await: completes synchronously when resumed
    auto SyncValue = [](int val) -> Async<int> {
        co_return val;
    };

    auto h = sched.Start([&]() -> Async<void> {
        // Is=0 (SyncValue(1)) will complete synchronously.
        // If Any is destroyed before Is=1 is started, that is use-after-free.
        auto [r0, r1] = co_await Any(SyncValue(1), SyncValue(2));
        std::cout << "parent resumed: r0=" << r0.value_or(-1)
                  << " r1=" << r1.value_or(-1) << "\n";
        completed = true;
    });

    sched.Update();

    std::cout << "completed=" << completed << "\n";
    std::cout << "--- TestAnySyncChildUAF end ---\n\n";
}

// ---------- Callback Awaiter Tests ----------

void TestCallbackVoid()
{
    std::cout << "--- TestCallbackVoid ---\n";
    Scheduler sched;
    bool completed = false;

    Callback<>::Sender sender;

    auto h = sched.Start([&]() -> Async<void> {
        Callback<> cb;
        sender = cb.GetSender();
        co_await cb;
        completed = true;
    });

    sched.Update();
    assert(!completed && "Should not complete before Send()");

    sender.Send();

    // Send() pushes to lock-free stack, but coroutine resumes on next Update()
    assert(!completed && "Should not complete before Update()");

    sched.Update();
    assert(completed && "Should complete after Update()");

    std::cout << "--- TestCallbackVoid passed ---\n\n";
}

void TestCallbackValue()
{
    std::cout << "--- TestCallbackValue ---\n";
    Scheduler sched;
    int result = 0;
    bool completed = false;

    Callback<int>::Sender sender;

    auto h = sched.Start([&]() -> Async<void> {
        Callback<int> cb;
        sender = cb.GetSender();
        result = co_await cb;
        completed = true;
    });

    sched.Update();
    assert(!completed);

    sender.Send(42);
    sched.Update();

    assert(completed);
    assert(result == 42);

    std::cout << "--- TestCallbackValue passed ---\n\n";
}

void TestCallbackFromThread()
{
    std::cout << "--- TestCallbackFromThread ---\n";
    Scheduler sched;
    int result = 0;
    bool completed = false;

    Callback<int>::Sender sender;

    auto h = sched.Start([&]() -> Async<void> {
        Callback<int> cb;
        sender = cb.GetSender();
        result = co_await cb;
        completed = true;
    });

    sched.Update(); // kick off coroutine, it suspends at co_await cb

    std::thread worker([sender]() {
        sender.Send(99);
    });
    worker.join();

    assert(!completed && "Should not resume until Update()");

    sched.Update();
    assert(completed);
    assert(result == 99);

    std::cout << "--- TestCallbackFromThread passed ---\n\n";
}

void TestCallbackStopBeforeSend()
{
    std::cout << "--- TestCallbackStopBeforeSend ---\n";
    Scheduler sched;

    Callback<int>::Sender sender;

    {
        auto h = sched.Start([&]() -> Async<void> {
            Callback<int> cb;
            sender = cb.GetSender();
            co_await cb;
            assert(false && "Should not reach here");
        });

        sched.Update(); // coroutine suspends at co_await cb

        // h goes out of scope -> coroutine stopped -> ~CallbackBP runs
    }

    // Send after coroutine is destroyed — should be harmless
    sender.Send(123);

    // Update should not crash
    sched.Update();

    std::cout << "--- TestCallbackStopBeforeSend passed ---\n\n";
}

void TestCallbackSignalBeforeAwait()
{
    std::cout << "--- TestCallbackSignalBeforeAwait ---\n";
    Scheduler sched;
    int result = 0;
    bool completed = false;

    auto h = sched.Start([&]() -> Async<void> {
        Callback<int> cb;
        auto sender = cb.GetSender();
        sender.Send(77); // Signal before co_await
        result = co_await cb; // await_ready() returns true, no suspend
        completed = true;
    });

    // The coroutine should complete synchronously (await_ready returns true)
    // after the initial Start() kick-off
    assert(completed);
    assert(result == 77);

    std::cout << "--- TestCallbackSignalBeforeAwait passed ---\n\n";
}

void TestCallbackWithAllCombinator()
{
    std::cout << "--- TestCallbackWithAllCombinator ---\n";
    Scheduler sched;
    bool completed = false;
    int r1 = 0, r2 = 0;

    Callback<int>::Sender sender1;
    Callback<int>::Sender sender2;

    auto h = sched.Start([&]() -> Async<void> {
        auto [v1, v2] = co_await All(
            [&]() -> Async<int> {
                Callback<int> cb;
                sender1 = cb.GetSender();
                co_return co_await cb;
            }(),
            [&]() -> Async<int> {
                Callback<int> cb;
                sender2 = cb.GetSender();
                co_return co_await cb;
            }()
        );
        r1 = v1;
        r2 = v2;
        completed = true;
    });

    sched.Update();
    assert(!completed);

    sender1.Send(10);
    sched.Update();
    assert(!completed && "All should wait for both");

    sender2.Send(20);
    sched.Update();
    assert(completed);
    assert(r1 == 10 && r2 == 20);

    std::cout << "--- TestCallbackWithAllCombinator passed ---\n\n";
}

void TestCallbackWithAnyCombinator()
{
    std::cout << "--- TestCallbackWithAnyCombinator ---\n";
    Scheduler sched;
    bool completed = false;

    Callback<int>::Sender sender1;
    Callback<int>::Sender sender2;

    auto h = sched.Start([&]() -> Async<void> {
        auto [v1, v2] = co_await Any(
            [&]() -> Async<int> {
                Callback<int> cb;
                sender1 = cb.GetSender();
                co_return co_await cb;
            }(),
            [&]() -> Async<int> {
                Callback<int> cb;
                sender2 = cb.GetSender();
                co_return co_await cb;
            }()
        );
        assert(v1.has_value() && *v1 == 55);
        assert(!v2.has_value());
        completed = true;
    });

    sched.Update();
    assert(!completed);

    sender1.Send(55);
    sched.Update();
    assert(completed);

    // sender2 should be harmless after Any stopped the second coroutine
    sender2.Send(66);
    sched.Update(); // should not crash

    std::cout << "--- TestCallbackWithAnyCombinator passed ---\n\n";
}

void TestCallbackSchedulerDestroyed()
{
    std::cout << "--- TestCallbackSchedulerDestroyed ---\n";

    Callback<int>::Sender sender;

    {
        Scheduler sched;
        auto h = sched.Start([&]() -> Async<void> {
            Callback<int> cb;
            sender = cb.GetSender();
            co_await cb;
        });

        sched.Update();
        // sched destroyed here
    }

    // Send after scheduler destroyed — should be harmless
    sender.Send(42);

    std::cout << "--- TestCallbackSchedulerDestroyed passed ---\n\n";
}

int main()
{
    TestAnySyncChildUAF();

    TestSingleAwaitValue();
    TestSingleAwaitVoid();
    TestAllCombinator();
    TestAnyCombinator();
    TestNextFrame();
    TestStop();
    TestUseHandleAfterSchedulerDestroyed();
    TestStartInCoroutine();
    TestGlobalScheduler();
    TestCustomUpdateAndTimers();
    TestWaitUntilAndWhile();
    TestThrowException();
    TestHandle();
    TestMemberCoroutines();
    TestReturnObjLifetime();

    StressTest(20000);

    TestCallbackVoid();
    TestCallbackValue();
    TestCallbackFromThread();
    TestCallbackStopBeforeSend();
    TestCallbackSignalBeforeAwait();
    TestCallbackWithAllCombinator();
    TestCallbackWithAnyCombinator();
    TestCallbackSchedulerDestroyed();

    std::cout << "All tests passed successfully." << std::endl;
    return 0;
}
