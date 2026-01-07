# tokoro
[![Coverage Status](https://codecov.io/gh/ShirenY/tokoro/branch/main/graph/badge.svg)](https://codecov.io/gh/ShirenY/tokoro) [![Linux Build Status](https://github.com/ShirenY/tokoro/actions/workflows/ci-linux.yml/badge.svg)](https://github.com/ShirenY/tokoro/actions/workflows/ci-linux.yml) [![macOS Build Status](https://github.com/ShirenY/tokoro/actions/workflows/ci-macos.yml/badge.svg)](https://github.com/ShirenY/tokoro/actions/workflows/ci-macos.yml) [![Windows Build Status](https://github.com/ShirenY/tokoro/actions/workflows/ci-windows.yml/badge.svg)](https://github.com/ShirenY/tokoro/actions/workflows/ci-windows.yml)

**tokoro** is a lightweight, header‑only C++20 coroutine library tailored for games, GUIs, and other update‑driven systems. It delivers efficient single‑threaded coroutine scheduling—ideal for latency‑sensitive, performance‑critical applications.

### ✨ Highlights

* 🔄 **Update-based scheduling** – perfect for frame-driven systems like games or UI rendering.
* ⚡ **Lightweight & efficient** – low memory footprint and minimal scheduling overhead.
* 🧩 **Plug-and-play** – just include the header, setup update call, and you’re ready to go.
* 🔧 **Highly customizable** – plug in your own update phases or timers.
* 🧵 **Single-threaded** – less power comes with less responsbility. Sometimes we just want to be safe.

### 🎯 Project Goal
Designed to be lightweight and modular, yet expressive enough to handle complex coroutine flows.

### 🚫 Non-Goals
Not a coroutine library for maximizing multi-core CPU throughput. ( There are quite some coroutine libraries doing that. ) However, you can still delegate computation to threads and return results into tokoro coroutines.

### Hello tokoro
Here's a simple, compilable example showcasing tokoro in action.
```C++
// This example is compilable, make sure you enable the C++ 20 flag.
#include "tokoro.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace tokoro;
Scheduler scheduler;

Async<void> awkwardHello(std::string somebody, double holdSeconds)
{
    std::cout << "Hello, ";
    
    // Sometimes you need a moment to recall their name.
    co_await Wait(holdSeconds);
    
    std::cout << somebody << "!" << std::endl;
}

int main()
{    
    scheduler.Start(awkwardHello, "tokoro", 1).Forget();
    
    // Simulate a game update loop
    while(true)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        
        scheduler.Update();
    }

    return 0;
}
```



## 📖 Table of Contents

- [📚 Tutorial](#-tutorial)
  - [Integrating tokoro](#integrating-tokoro)
  - [Decide Scheduler Scope](#decide-scheduler-scope)
  - [Creating a Coroutine](#creating-a-coroutine)
  - [Starting a Root Coroutine](#starting-a-root-coroutine)
  - [Coroutine Lifetimes](#coroutine-lifetimes)
  - [The Way to Handle It](#the-way-to-handle-it)
  - [Awaiters](#awaiters)
  - [Custom Updates](#custom-updates)
  - [Execution Flow](#execution-flow)
  - [Exceptions](#exceptions)
- [Performance](#performance)
- [FAQ](#faq)
- [Best Practices](#best-practices)
- [Next Steps](#next-steps)
- [Inspiring](#inspiring)
- [Platform Compatibility](#platform-compatibility)
- [License](#license)



## 📚 Tutorial

### Integrating tokoro
**tokoro** is a lightweight, header-only library with zero dependencies. To integrate it into your project:

- Copy or clone the repository.
- Add `path_to_tokoro/include` to your compiler's include search paths.
   *(Note: the `.cpp` and `.bat` files are only for unit testing.)*
- Add `#include "tokoro.h"` wherever you need coroutine support.

That’s it — you’re ready to go!

### Decide Scheduler Scope
`tokoro::Scheduler` can be used either as a local instance or as a global singleton, depending on your project’s architecture and needs.

#### Using a Scheduler Locally

The `Scheduler` can be used anywhere regular updates are needed. You can easily embed it into a small part of a game.

> **Note:** The `Scheduler` is neither copyable nor movable.  
> While being non-copyable is straightforward, it is also non-movable because member coroutines typically relies on `this` pointers in local usage, which cannot be rebound, so it's non-movable to avoid miss use.

```c++
// The concept of an "Entity" exists in most game engines.
// In Unreal Engine, it's called an "Actor"; in Unity, it's a "GameObject".
// All of them typically receive similar update calls from the engine.
class Entity
{
public:
    ...
    virtual void Update()
    {
        schedular.Update();
    }
private:
    ...
    tokoro::Scheduler schedular;
}
```
#### Using a Global Singleton Scheduler
tokoro does not comes with a buildin global scheduler, however you can easily create one.
```c++
static tokoro::Scheduler& GlobalScheduler() 
{
    static tokoro::Scheduler s;
    return s;
}
...
void Engine::Update()
{
    ...
    GlobalScheduler().Update();
}
```
### Creating a Coroutine

A **tokoro coroutine** must contain at least two elements:

1. It returns `Async<T>`, where `T` can be any type that supports copy or move semantics.
2. It includes **at least one** `co_await` or `co_return` expression. tokoro dose not support `co_yield`.

Below is a minimal example:
```C++
Async<int> Square(int value) 
{
    co_await tokoro::Wait();
    co_return value * value;
}
```

**Lambdas can be coroutines** too. To avoid the well-known [pitfall](https://quuxplusone.github.io/blog/2019/07/10/ways-to-get-dangling-references-with-coroutines/) when using lambdas as C++ coroutines, `tokoro::Scheduler` caches the start lambda together with the coroutine object. However, the usual lambda capture limitations still apply: make sure any references captured in the lambda remain valid for the lifetime of the coroutine.

**Coroutines can also be nested** using `co_await`, allowing you to compose and reuse generic coroutine logic. Example:
`co_await awkwardHello("you", 1);`

tokoro also provides two helper coroutines: `WaitUntil` and `WaitWhile`. They repeatedly check a condition on every update until the provided lambda returns `true`. They're especially useful when you need to wait for external signals or state changes.

```cpp
co_await WaitUntil([&]()->bool{return launchComplete;});
co_await WaitWhile([&]()->bool{return playingStartCutscene;});
```

In the [awaiters](#awaiters) section, we'll introduce **combinator awaiters**—`All` and `Any`—which enable you to construct even more complex coroutine execution flows.

### Starting a Root Coroutine

`tokoro::Scheduler::Start()` is the only entry point for launching a **root coroutine**. A **root coroutine** is one that is started directly by the `Scheduler`. Coroutines that are started within a coroutine with co_await are called **sub-coroutines**. All coroutines must be run either as root coroutines or as sub-coroutines.

```C++
Handle<T> Scheduler::Start(CoroutineFunc, Args ...)
```

`Scheduler::Start()` is a templated function that accepts a coroutine function (`CoroutineFunc`) along with its arguments. The `CoroutineFunc` must return an `Async<T>`, and the argument list (`Args...`) must match the coroutine function's parameters.

This function returns a `Handle<T>`, which allows you to:

* monitor the coroutine's status,

* stop it,

* or retrieve its result.

> ⚠️ **Important:** When the returned `Handle<T>` goes out of scope, the associated coroutine is automatically stopped. Therefore, you should never ignore or discard the return value of `Scheduler::Start()`, as doing so will immediately stop the coroutine.

If you intend to **fire-and-forget** a coroutine, explicitly call `.Forget()` on the handle:

```cpp
Scheduler::Start(Fire).Forget();
```

**Launching coroutines from member functions** can be slightly confusing to some users. Here's an example to clarify how it works:

```cpp
class Soldier
{
public:
    void StartPatrol(int routeId, int count)
    {
        patrolTask = GlobalScheduler().Start(
            &Soldier::Patrol,    // Note: '&' and the class scope 'Soldier::' are required
            this,                // Explicitly pass 'this' as the first argument
            routeId,             // The rest are regular method arguments
            count
        );

        // Reassigning a Handle will automatically stop the previous coroutine.
        // This is the intended behavior in this case.
    }

private:
    Async<void> Patrol(int routeId, int count)
    {
        ...
    }

    Handle<void> patrolTask;
};
```

### Coroutine Lifetimes

The `Scheduler` manages the lifetime of all root coroutine objects it starts. A coroutine remains alive and active until it stops, which can happen in one of three ways:

1. The coroutine completes and returns normally.

2. An unhandled exception is thrown from the coroutine.

3. The coroutine is explicitly stopped via its associated `Handle`:

   * Most commonly, this happens automatically when the handle goes out of scope, leveraging RAII.

   * Alternatively, you can manually stop a coroutine at any time by calling `Handle::Stop()`.

Just like in regular C++, when a root coroutine is destroyed, all of its sub-coroutines—and any objects within its scope—are also destroyed recursively.

> 💡 **Tip:** Since a coroutine can be interrupted at any time (due to manual stops or exceptions), using **RAII** is the recommended approach for resource management inside coroutines.

### The Way to Handle It
`tokoro::Handle` is a simple yet powerful tool for externally managing coroutines. In this section, we’ll walk through each of its available methods and how to use them effectively.

#### Construction & Destruction
`Handle<T>` is a template class, where `T` is the return type of the associated coroutine. Typically, you obtain a handle by starting a coroutine. The type `T` is automatically deduced:
```cpp
auto handle = schedular.Start(DelayAction);
```
You can also create an invalid handle using the default constructor. `Handle` is **non-copyable** but **movable**—it provides a move constructor and a move assignment operator. This ensures that **only one valid handle** is associated with a single root coroutine at any time.
```cpp
Handle<void> handle; // handle.IsInvalid() == false
handle = schedular.Start(DelayAction); // handle.IsInvalid() == true
```
When a `Handle` is destroyed, its destructor will stop and release the associated coroutine. Explicitly call `.Forget()` to **fire-and-forget**.
```cpp
schedular.Start(DelayAction).Forget(); // Handle is discard, but the coroutine will keep running to its end.
```
> ⚠️ **Note:** `Forget` handles without understanding their lifetime or dependencies is generally **not recommended**. Managing coroutine lifetimes intentionally is essential for writing safe and robust coroutine logic.
We’ll explore this further in the [Best Practices](#best-practices) section.

#### bool Handle::IsValid()
`IsValid()` indicates whether the handle was obtained from a call to `Scheduler::Start()`.\
It does **not** reflect the current status of the coroutine or the scheduler.

#### void Handle::Stop()
`Stop()` is the only method in tokoro’s cancellation system that allows you to externally stop a suspended coroutine.\
Unlike many modern coroutine libraries that use cancellation tokens, tokoro relies on **RAII** and suspend points to safely handle coroutine cancellation. This design frees you from managing cumbersome cancellation tokens and passing them through nested coroutines.

This simplified **stop mechanism** eliminates much of the complexity and pain associated with cancellation token systems. In most cases, you don’t need to add special cancellation handling inside your coroutines. (tokoro’s focus on single-threaded execution helps achieve this simplicity.)

However, in some cases, you still need to ensure proper resource cleanup and state rollback using RAII, especially when coroutines are stopped prematurely. We will cover this in [Best Practices](#best-practices) section.

#### std::optional\<AsyncState\> Handle::GetState()
`GetState()` provides two layers of information about the coroutine's status.

* If it returns `std::nullopt`, it means **either** the handle is invalid **or** the associated **scheduler** has been destroyed. To differentiate between these two cases, you can check `IsValid()` on the handle.

* Otherwise, it returns an `AsyncState` value representing the current state of the coroutine:

  * **Running**: The coroutine is currently running. Note that not every newly started handle will immediately return `Running`, as the coroutine might already have completed by the time `Scheduler::Start()` returns.

  * **Succeeded**: The coroutine finished execution successfully without exceptions.

  * **Failed**: The coroutine threw an exception that was not caught internally. Calling `Handle::TakeResult()` will rethrow the exception to the caller.

  * **Stopped**: The coroutine was manually stopped by `Handle::Stop()`.

> **Note:** Receiving an `AsyncState` from `GetState()` does **not** guarantee that the underlying coroutine object still exists; it might have already been destroyed. Refer to the [Coroutine Lifetimes](#coroutine-lifetimes) section for details.

#### bool Handle::IsRunning()
Sometimes repeatedly writing this can be tedious:
```cpp
auto state = handle.GetState();
if (state.has_value() && *state == AsyncState::Running)
    ...
```
`IsRunning()` provides a convenient shorthand for that check.

#### std::optional\<T\> Handle::TakeResult()
`TakeResult()` is a one-time call that extracts and returns the coroutine’s result to the caller.
* If the coroutine has produced a return value, the **first call** to `TakeResult()` will return that result.
* Subsequent calls will return `std::nullopt`.
* If the coroutine is still running, `TakeResult()` will also return `std::nullopt`. To distinguish whether the coroutine is still running or the result has already been taken, you can call `IsRunning()`.
If the coroutine ended due to an unhandled exception, `TakeResult()` will rethrow that exception. This exception will only be thrown once—subsequent calls will return `std::nullopt`.

#### Handle::Forget()
As mentioned earlier, `Forget()` is typically used for **fire-and-forget** coroutines—when you want to start a coroutine without holding onto its handle.
However, you can still use the handle normally **after** calling `Forget()`. All other handle functions will continue to work as expected.

### Awaiters
Currently, tokoro provides only **three types of explicit awaiters** you can use directly. (There are some implicit awaiters under the hood, but as a library user, you don’t need to worry about those.)

#### Wait
`Wait` is the most fundamental awaiter in tokoro. There are two ways to use it:

1. `co_await Wait();` Suspends the current coroutine until the **next** `Scheduler::Update()` call.
2. `co_await Wait(double sec);` Suspends the coroutine and adds it to the scheduler’s timed queue, where it will be resumed after approximately `sec` seconds during a future `Scheduler::Update()`.

Internally, the timed queue only checks and resumes coroutines that are due at the current time point, making it highly efficient. For example, `Wait(std::numeric_limits<double>::max())` only incurs the cost of inserting into the queue and minimal memory overhead—no extra overhead in regular updates.

You can also specify custom update and time types via `Wait(UpdateType, TimeType)`. For details on using your own update types and timers, please refer to the [Custom Updates](#custom-updates) section.

#### All
`All` waits for **all** coroutines it holds to finish. It returns a tuple containing the differen types of return values of each sub-coroutine.

```cpp
auto [terrain, texture] = co_await All(GenerateTerrain(), LoadTexture(path));
```

#### Any
`Any` waits until **any one** of its sub-coroutines finishes. It returns a tuple of `std::optional<T>`, where `T` is the return type of each coroutine. You can check each tuple element to determine which coroutine finished first.

```cpp
auto [texture, timeout] = co_await All(LoadMesh(meshPath), Delay(10));
if(timeout.has_value())
{
    LOG_Error("Timeout after 10 second when try to load %s", meshPath);
}
```
**Note:** When **any** coroutine finishes, all other sub-coroutines are immediately stopped by the `Any` awaiter. If you want the other coroutines to keep running after `Any` completes, you need to start them as **root coroutines** and wait for their handles separately.

```cpp
Handle<Mesh> handle1 = GlobalScheduler().Start(LoadMesh, mesh1);
Handle<Mesh> handle2 = GlobalScheduler().Start(LoadMesh, mesh2);
// You can probably co_await WaitWhile([&](){return handle1.IsRunning() || handle2.IsRunning()});
// But this is just a example.
auto [handle1Finished, handle2Finished] = co_await Any(WaitWhile([&](){return handle1.IsRunning()}), WaitWhile([&](){return handle2.IsRunning()}));

if(handle1Finished.has_value())
{
    LOG("%s loaded first", mesh1);
}
else
{
    LOG("%s loaded first", mesh2);
}
```

### Custom Updates
tokoro provides a default **tokoro::Scheduler**, designed for applications with a single regular update loop. This makes it easy to get started with coroutines right away.
However, most modern game engines (like Unity) have **multiple update phases**, such as `Update`, `LateUpdate`, and `FixedUpdate`. Unity also distinguishes between **real time** and **game time** (which can be paused). We want tokoro to support all of these cases.
Rather than embedding all possible update types and timers into the core library—which would add complexity and confuse users who don't need them—tokoro allows users to **define custom update types**.
Below is an example. 
> ⚠️ **Note:** **some constraints are not enforced by the compiler**, so please read the comments carefully.

```cpp
enum class UpdateType
{
    Update = 0, // Update type start with 0 value, the zero enum will be taken as default.
    PreUpdate,
    PostUpdate,
    Count, // Count at last of enum is a must have for tokoro.
};

enum class TimeType
{
    EmuRealTime = 0, // Time type should start with 0 value too, the zero enum will be taken as default.
    GameTime,
    Count, // Count at last of enum is a must have for tokoro.
};

// Give alias names for ease of life. Suffix BP is for Blueprint.
//
// Note: You can still use Scheduler, Wait if you really like these names.
// Just don't introduce 'using namespace tokoro' to your code.
using MyScheduler = SchedulerBP<UpdateType, TimeType>;
using MyWait      = WaitBP<UpdateType, TimeType>;

// There's no way to give alias to functions in C++, so we have to use function ptr for WaitUntil & WaitWhile.
// Also because of that, WaitUntilBP and WaitWhileBP does not support to resume at a user wanted CustomUpdate.
// But it's very easy to implement one if you look into WaitUntilBP's implement, it's literally 2 lines of code.
inline auto MyWaitUntil = WaitUntilBP<UpdateType, TimeType>;
inline auto MyWaitWhile = WaitWhileBP<UpdateType, TimeType>;

MyScheduler sched;
double emuRealTime = 0;
double gameTime = 0;
bool gamePaused = false;

int main()
{
    ... 
        
    // SetCustomTimer accept a get function of the time: double getTime()
    // Note: this timer is only for test,
    // in most applications the default timer is good enough for 'real time'
    sched.SetCustomTimer(TimeType::EmuRealTime, [&]() -> double { return emuRealTime; });
    sched.SetCustomTimer(TimeType::GameTime, [&]() -> double { return gameTime; });    
    
    // Simulate a game engine loop
    while(true)
    {
        emuRealTime += frameTime;
        if (!gamePaused)
            gameTime += frameTime;

        ...

        // It's your responsibility to setup all Update calls in the engine. The compiler have no way to detect if all Update types are called.
        // If you have some update in other threads, you have to setup the sync point with your 'main' gameplay threads with mutex, to call
        // these Updates. Just like how Unity do with their FixedUpdate. Scheduler is NOT thread safe.
        sched.Update(UpdateType::PreUpdate, TimeType::EmuRealTime);
        sched.Update(UpdateType::PreUpdate, TimeType::GameTime);

        sched.Update(UpdateType::Update, TimeType::EmuRealTime);
        sched.Update(UpdateType::Update, TimeType::GameTime);

        sched.Update(UpdateType::PostUpdate, TimeType::EmuRealTime);
        sched.Update(UpdateType::PostUpdate, TimeType::GameTime);

        ...
    }
}

// Example coroutine usage
Handle handle = sched.Start([&]() -> Async<void> {
     co_await MyWait(UpdateType::PreUpdate); // Wait next PreUpdate
     co_await MyWait(UpdateType::Update); // Wait next Update
     co_await MyWait(UpdateType::PostUpdate); // Wait next PostUpdate
     co_await MyWait(1, UpdateType::Update, TimeType::GameTime); // Wait for 1 sec in GameTime, and resume in Update.
     co_await MyWait(1); // Use default parameters of update and time, equals to MyWait(1, UpdateType::Update, TimeType::EmuRealTime)
 }).Forget();
```

### Execution Flow
While it may seem obvious, it's important to clearly understand **when a coroutine yields control** and the main game loop resumes processing.
Coroutines in tokoro **begin executing immediately** when created by `Scheduler::Start()` or by a parent coroutine. **They only suspend and yield control when they hit an `co_await` on a suspendable awaiter**, such as `Wait()`.
Take the following example:
```cpp
scheduler.Start([]()->Async<void>{
	std::cout<< "Current Frame: " << Time.Frame() << std::endl;

    co_await []()->Async<void>{
    	std::cout<< "Current Frame: " << Time.Frame() << std::endl;
        co_await Wait(); // Suspend for next update.
        std::cout<< "Current Frame: " << Time.Frame() << std::endl;
    }
    
	std::cout<< "Current Frame: " << Time.Frame() << std::endl;
}).Forget();
```
If this coroutine is started during **frame 10**, the console output would be:
```bash
Current Frame: 10
Current Frame: 10
Current Frame: 11
Current Frame: 11
```
**Explanation**:
1. The root coroutine starts immediately in `Scheduler::Start()` call of frame 10.
2. It invokes the inner coroutine, which also runs immediately—until it hits `co_await Wait()`, where it suspends.
3. The main coroutine does not continue until the sub-coroutine is resumed (next frame).
4. On frame 11, the scheduler resumes the suspended inner coroutine, it prints again.
5. After the inner coroutine finishes, the outer coroutine resumes and prints once more.

### Exceptions
tokoro fully supports exceptions—yes, even though I personally don't see why you'd want to use them in C++ game code 😆, the support is there.

When an exception is thrown inside a coroutine, it will propagate **upward through the coroutine chain**, just like in regular function calls. If it's not caught along the way, the exception will eventually reach the **root coroutine**. During this unwinding process, all scoped objects will be destroyed properly, so **RAII** should be your go-to for resource cleanup.

If the exception goes unhandled, the root coroutine will end in the `AsyncState::Failed` state, and no result will be produced. However, if you later call `Handle::TakeResult()` on that coroutine, the stored exception will be **rethrown once**, allowing you to catch and process it outside the coroutine.

**Know Issue**: clang v20.1.0 on windows has a [bug](https://github.com/llvm/llvm-project/issues/143235) that you maybe not able to catch exceptions correctly from sub-coroutines.

> ⚠️ **Important note (not specific to tokoro)**: If your project is compiled with exceptions disabled (e.g., `/EHs-` or `-fno-exceptions`), any exception will cause a crash immediately. Use with care.



## Performance

In game development, **scheduling performance** is one of the most critical metrics when evaluating a coroutine system. tokoro is designed to ensure that the **CPU cost of scheduling a coroutine is only O(log N)**.

To benchmark this, we use the **Fibonacci coroutines stress test** in `TestCoroutine.cpp`. The test runs **10,000 active coroutines**, each randomly scheduled to resume within one second. The results show that the **maximum cost per update is just 0.35ms**—which is **only 2.1% of a single frame at 60 FPS**. This leaves ample headroom for even coroutine-heavy game logic.

> ✅ In short, tokoro is built to handle **massive concurrent coroutine usage** with minimal scheduling overhead.



## FAQ

#### What's the point of single-threaded coroutines?

While coroutines are often associated with multi-threaded concurrency, **single-threaded coroutines are especially useful for game development**. Because a coroutine is essentially a localized state machine, it fits naturally with frame-based gameplay logic.

By focusing on single-thread scheduling:

* You avoid complex data races and locking mechanisms.
* You keep game logic deterministic and easier to reason about.
* You get **linear-style code** for asynchronous behavior, improving readability and cohesion.

In short, this approach gives you **the benefits of async logic** without the chaos of multithreading.

On the other hand, you can still use other threading tools with tokoro. Launched threads inside coroutines, then use `WaitUntil` or `WaitWhile` to check for their completion on each frame.

#### How to debug coroutines?

Debugging coroutines in C++ is harder than debugging regular functions, mainly due to two limitations:

1. **No visible local variables** like in normal functions. Coroutine local state is stored in compiler-generated structures, which most debuggers don’t visualize cleanly (yet). You may need to manually inspect those internal structures to understand current state.
2. **No usable call stack** to trace how execution arrived at the current point. Coroutine call chains are broken up across suspension points, so traditional stack traces don't apply. This is not unique to C++; all coroutine systems (e.g., in C#, Lua, etc.) face similar limitations.

> 🔧 In the future, macro tools might help capture coroutine call chains or improve introspection, but we haven’t yet found an elegant, general-purpose solution.



## Best Practices

Coroutines can feel so elegant and powerful that **beginners often overuse or misuse them**. Here are some practical tips to keep your coroutine usage safe and robust.

#### Using RAII to Ensure Proper Resource Cleanup When Coroutine Is Stopped Externally
Coroutines can be stopped at any time from outside via `Handle::Stop()` or by exceptions. Therefore, it is critical to use RAII (Resource Acquisition Is Initialization) inside coroutines to manage resources safely. This ensures resources are properly released even if the coroutine is interrupted.
For example, if your coroutine acquires locks, opens files, or allocates memory, wrap them in RAII types. When the coroutine stops or throws, these RAII objects will be destructed automatically, releasing resources correctly.

#### Always ensure coroutine dependencies outlive the coroutine itself
Here, "dependencies" refer to any external references or pointers used inside a coroutine. Whenever a coroutine accesses data outside its own scope, you must be careful to ensure that data outlives the coroutine. Otherwise, once the coroutine resumes after a suspension, it might access invalid memory.
This issue is especially easy to overlook for beginners due to the delayed execution nature of coroutines.
A recommended approach is to take advantage of the `Handle`’s RAII behavior: If the resources your coroutine depends on are about to be destroyed, make sure the `Handle` is destroyed along with them, so the coroutine stops safely.

#### Single thread means no concurrent issues, but you still need to take care the shared status.
As mentioned earlier, references and pointers to data outside a coroutine can cause problems. In tokoro, since most operations run on a single thread, users generally don't need to worry about mutexes or atomic operations. However, if other parts of your program modify the same memory your coroutine accesses, data corruption can easily occur.

For example:
```cpp
Async<Entity> FindEnemy()
{
    for(auto& entity : currentVisibleEntities)
    {
        if(IsEnemy(entity))
            co_return entity;
        
        co_await Wait(); // Yield for one frame for smooth frame cost.
    }

    co_return None;
}
```
In this code, the intention is to check one entity per frame to spread out the workload. However, `currentVisibleEntities` is a container that may be modified concurrently by other components of the entity. This means the iterator used in the `for` loop can become invalid or corrupted.
There is no one-size-fits-all solution for handling such data consistency issues; you need to design an approach that fits your specific case. In this particular scenario, a better strategy might be to use a current index as the iterator and verify its validity each frame before accessing the container.

#### Be cautious with coroutines that can run multiple instances simultaneously
As mentioned in the previous tip, accessing shared and mutable data can be risky. If a coroutine reads&writes from/to the same data but can have multiple instances running at same time, it’s easy to end up with shared data being modified unexpectedly.
A simple rule of thumb: **if your coroutine modifies shared data, avoid launching multiple instances simultaneously.** You can keep a handle to the coroutine instance to check whether it’s safe to launch a second one, or you can design your system so that launching a new instance replaces the old one—whatever suits your case best.
On the other hand, if your coroutine only reads from shared data or does not access shared data at all, running multiple instances concurrently is usually safe. But remember to still be mindful of the concerns raised in the previous tip.

#### Coroutines are infectious
To use an `Async<T> func()`, you either launch it with `Scheduler.Start()` and wait for its completion via a handle, or you `co_await` it inside another `Async<T>` function. Sometimes it might seem easier to simply make your function return `Async<T>`, so you don’t have to manually manage the scheduler or handles—just let callers handle it.

However, if this mindset spreads, you’ll find that gradually _all_ your “normal” functions start returning `Async<T>`. Is this a good thing? Not really.

If you consider all the tips above, you’ll realize that coroutines are not a silver bullet. If everything returns `Async<T>`, you lose clarity on when coroutines finish, what shared data they use, and whether they can safely run multiple instances. This quickly leads to a messy and hard-to-maintain codebase.

Therefore, your project should have **strict rules** about which methods can be `Async<T>`. If a method can be “normal,” it should stay “normal.” If coroutine logic is truly necessary, it’s better to keep the `Async<T>` methods private within your implementation and expose simple, synchronous APIs to the rest of the codebase.



## Next Steps

Currently, tokoro can be considered feature-complete. However, there are several directions I’d like to explore further. These features are not guaranteed to be added to the library, but I’m glad to investigate them if we find a good approach.

* **Optimize allocation performance for TimeQueue insertions in the scheduler:**
  Currently, TimeQueue uses `std::multiset`, which fits our needs well but incurs dynamic allocations on every insert. I want to explore ways to optimize this further to reduce allocation overhead.

* **Implement a Callback Awaiter:**
  A Callback Awaiter would allow users to wait for external signals more efficiently. Game engines or frameworks could build on this to create custom awaiters—for example, an `AnimationAwaiter` letting users `co_await Entity.Play("Die")`. While `WaitUntil` and `WaitWhile` currently provide similar functionality, they require the coroutine to resume and check the condition every frame. A Callback Awaiter would enable resuming the coroutine only when the external event occurs, avoiding constant polling.

* **ThreadPool Awaiter:**
  Although tokoro focuses on single-threaded coroutines, it doesn’t exclude the possibility of providing a convenient thread pool tool for dispatching heavy CPU tasks. The challenge is to implement this without impacting single-threaded coroutine performance.

* **Debug utilities for tracking nested coroutine call chains:**
  Tools to help trace the call hierarchy of nested coroutines would greatly aid debugging and improve developer experience.



## Inspiring

tokoro is inspired by Unity’s coroutine system and its successor, UniTask.



## Platform Compatibility

tokoro is designed to work on any platform that supports C++20 coroutines. It has been tested on Linux, macOS, and Windows.



## License

tokoro is released under the MIT License.

