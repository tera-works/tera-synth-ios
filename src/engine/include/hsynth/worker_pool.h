// Real-time worker pool used to spread voice rendering over several cores.
//
// run() is called from the audio thread. It offers the job to up to `helpers`
// pool threads and works on it itself (worker index 0); items are claimed
// dynamically, so a helper that wakes up late (or never) only means less
// parallelism, never a missed item. run() returns when every item is done and
// no helper touches the job any more.
//
// Protocol per helper (slot_): Idle -> Offered (audio thread) -> Running
// (helper, CAS) -> Done (helper) -> Idle (audio thread). The audio thread
// withdraws an offer that was not taken yet (CAS Offered -> Idle), so it only
// waits for helpers that are actually running an item.
//
// run() never allocates or locks; waking a sleeping helper is a semaphore post
// (a system call, but non-blocking). Helpers spin briefly after a job, then
// sleep on their semaphore.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace hsynth {

class WorkerPool {
public:
    using Fn = void (*)(void* ctx, int worker, int item);
    static constexpr int kMaxHelpers = 7;

    explicit WorkerPool(int helpers);
    ~WorkerPool();
    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    int helpers() const { return numHelpers_; }
    // OS thread ids of the helpers that have started (Linux / Android tids,
    // 0 elsewhere); for hosts that register audio threads with the OS.
    int threadIds(int* out, int max) const;

    // Audio thread. Runs fn(ctx, worker, item) for item in [0, count) on the
    // calling thread (worker 0) and up to `helpers` pool threads (1..helpers).
    void run(Fn fn, void* ctx, int count, int helpers);

private:
    enum : int { kIdle = 0, kOffered = 1, kRunning = 2, kDone = 3 };
    struct Sem;
    struct alignas(64) Helper {
        std::atomic<int> slot{kIdle};
        std::atomic<bool> sleeping{false};
        std::atomic<int> tid{0};
        Sem* sem = nullptr;
        std::thread thread;
    };
    void helperMain(int index);
    void work(int worker);

    int numHelpers_ = 0;
    std::unique_ptr<Helper[]> h_;
    std::atomic<bool> quit_{false};
    // current job (written by run() before the offers are published)
    Fn fn_ = nullptr;
    void* ctx_ = nullptr;
    int count_ = 0;
    alignas(64) std::atomic<int> next_{0};
};

// Number of render threads to use for "auto": about half the hardware threads
// (the other half is left to the audio callback's neighbours / UI), 1..4.
int autoRenderThreads();

}  // namespace hsynth
