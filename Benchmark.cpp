#include "tokoro.h"
#include <chrono>
#include <iostream>
#include <vector>
#include <iomanip>
#include <numeric>
#include <cmath>

using namespace tokoro;

// ============================================================================
// Benchmark utilities
// ============================================================================

struct BenchResult
{
    const char* name;
    double      ms;
};

template <typename Func>
double MeasureMs(Func&& f)
{
    auto start = std::chrono::high_resolution_clock::now();
    f();
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

// ============================================================================
// Scenario 1: Mass next-frame waits
// Simulates a game with thousands of entities each doing work every frame.
// This is the dominant real-world pattern.
// ============================================================================

Async<void> EveryFrameWorker(int& counter, int totalFrames)
{
    for (int i = 0; i < totalFrames; ++i)
    {
        counter++;
        co_await Wait();
    }
}

BenchResult BenchMassNextFrame(int coroCount, int frameCount)
{
    double simTime = 0.0;

    Scheduler sched;
    sched.SetCustomTimer(internal::PresetTimeType::Realtime, [&]() { return simTime; });

    int totalWork = 0;
    std::vector<Handle<void>> handles;
    handles.reserve(coroCount);

    for (int i = 0; i < coroCount; ++i)
    {
        handles.push_back(sched.Start(EveryFrameWorker, std::ref(totalWork), frameCount));
    }

    double ms = MeasureMs([&] {
        for (int f = 0; f < frameCount; ++f)
        {
            simTime += 0.016666;
            sched.Update();
        }
    });

    return {"MassNextFrame", ms};
}

// ============================================================================
// Scenario 2: Timed waits (simulating cooldowns, delays, timers)
// Coroutines wait for varying durations, staggered over time.
// ============================================================================

Async<void> TimedWorker(double delay, int repeats)
{
    for (int i = 0; i < repeats; ++i)
    {
        co_await Wait(delay);
    }
}

BenchResult BenchTimedWaits(int coroCount, int repeats)
{
    double simTime = 0.0;

    Scheduler sched;
    sched.SetCustomTimer(internal::PresetTimeType::Realtime, [&]() { return simTime; });

    std::vector<Handle<void>> handles;
    handles.reserve(coroCount);

    int finished = 0;

    for (int i = 0; i < coroCount; ++i)
    {
        double delay = 0.05 + (i % 20) * 0.01; // 50ms to 240ms delays
        auto h = sched.Start([&finished](double d, int r) -> Async<void> {
            for (int j = 0; j < r; ++j)
            {
                co_await Wait(d);
            }
            finished++;
        }, delay, repeats);
        handles.push_back(std::move(h));
    }

    double ms = MeasureMs([&] {
        // Run enough frames to complete all coroutines
        // Max delay * repeats = 0.24 * repeats seconds, at 60fps
        int maxFrames = static_cast<int>(std::ceil(0.25 * repeats / 0.016666)) + 100;
        for (int f = 0; f < maxFrames && finished < coroCount; ++f)
        {
            simTime += 0.016666;
            sched.Update();
        }
    });

    return {"TimedWaits", ms};
}

// ============================================================================
// Scenario 3: Churn (continuous start/stop, simulating entity spawn/despawn)
// ============================================================================

Async<void> ShortLivedCoro(int frames)
{
    for (int i = 0; i < frames; ++i)
    {
        co_await Wait();
    }
}

BenchResult BenchChurn(int spawnsPerFrame, int frameCount)
{
    double simTime = 0.0;

    Scheduler sched;
    sched.SetCustomTimer(internal::PresetTimeType::Realtime, [&]() { return simTime; });

    std::vector<Handle<void>> handles;
    handles.reserve(spawnsPerFrame * 10); // Ring buffer style

    double ms = MeasureMs([&] {
        for (int f = 0; f < frameCount; ++f)
        {
            // Spawn new coroutines
            for (int i = 0; i < spawnsPerFrame; ++i)
            {
                int lifespan = 3 + (f * spawnsPerFrame + i) % 10; // 3-12 frames
                handles.push_back(sched.Start(ShortLivedCoro, lifespan));
            }

            // Stop some old ones (simulate despawn)
            if (handles.size() > static_cast<size_t>(spawnsPerFrame * 8))
            {
                for (int i = 0; i < spawnsPerFrame * 2 && !handles.empty(); ++i)
                {
                    handles.back().Stop();
                    handles.pop_back();
                }
            }

            simTime += 0.016666;
            sched.Update();
        }
    });

    return {"Churn", ms};
}

// ============================================================================
// Scenario 4: Nested coroutines (deep call chains)
// ============================================================================

Async<int> NestedAdd(int depth, int value)
{
    if (depth <= 0)
    {
        co_await Wait();
        co_return value;
    }
    int result = co_await NestedAdd(depth - 1, value + 1);
    co_return result;
}

BenchResult BenchNested(int coroCount, int depth)
{
    double simTime = 0.0;

    Scheduler sched;
    sched.SetCustomTimer(internal::PresetTimeType::Realtime, [&]() { return simTime; });

    int finished = 0;
    std::vector<Handle<int>> handles;
    handles.reserve(coroCount);

    for (int i = 0; i < coroCount; ++i)
    {
        auto h = sched.Start([&finished](int d) -> Async<int> {
            int r = co_await NestedAdd(d, 0);
            finished++;
            co_return r;
        }, depth);
        handles.push_back(std::move(h));
    }

    double ms = MeasureMs([&] {
        for (int f = 0; f < 10000 && finished < coroCount; ++f)
        {
            simTime += 0.016666;
            sched.Update();
        }
    });

    return {"Nested", ms};
}

// ============================================================================
// Scenario 5: All/Any combinators
// ============================================================================

Async<int> QuickTask(int value, int frames)
{
    for (int i = 0; i < frames; ++i)
        co_await Wait();
    co_return value;
}

BenchResult BenchCombinators(int iterations)
{
    double simTime = 0.0;

    Scheduler sched;
    sched.SetCustomTimer(internal::PresetTimeType::Realtime, [&]() { return simTime; });

    int finished = 0;
    std::vector<Handle<void>> handles;
    handles.reserve(iterations);

    for (int i = 0; i < iterations; ++i)
    {
        auto h = sched.Start([&finished, i]() -> Async<void> {
            if (i % 2 == 0)
            {
                // All: wait for both
                auto [a, b] = co_await All(QuickTask(1, 2), QuickTask(2, 3));
            }
            else
            {
                // Any: wait for first
                auto [a, b] = co_await Any(QuickTask(1, 2), QuickTask(2, 5));
            }
            finished++;
        });
        handles.push_back(std::move(h));
    }

    double ms = MeasureMs([&] {
        for (int f = 0; f < 10000 && finished < iterations; ++f)
        {
            simTime += 0.016666;
            sched.Update();
        }
    });

    return {"Combinators", ms};
}

// ============================================================================
// Main
// ============================================================================

int main()
{
    std::cout << "=== tokoro Benchmark ===" << std::endl;
    std::cout << std::fixed << std::setprecision(2);

    // Warmup
    BenchMassNextFrame(100, 10);

    // Run benchmarks
    std::vector<BenchResult> results;

    results.push_back(BenchMassNextFrame(10000, 1000));     // 10k coroutines, 1000 frames
    results.push_back(BenchTimedWaits(5000, 10));           // 5k coroutines, 10 repeats each
    results.push_back(BenchChurn(100, 1000));               // 100 spawns/frame, 1000 frames
    results.push_back(BenchNested(2000, 10));               // 2k coroutines, depth 10
    results.push_back(BenchCombinators(2000));              // 2k combinator uses

    // Print results
    std::cout << "\n--- Results ---" << std::endl;
    double total = 0;
    for (auto& r : results)
    {
        std::cout << std::setw(20) << r.name << ": " << std::setw(10) << r.ms << " ms" << std::endl;
        total += r.ms;
    }
    std::cout << std::setw(20) << "TOTAL" << ": " << std::setw(10) << total << " ms" << std::endl;

    return 0;
}
