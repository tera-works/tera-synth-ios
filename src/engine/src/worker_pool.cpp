#include "hsynth/worker_pool.h"

#include <algorithm>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <dispatch/dispatch.h>
#include <pthread.h>
#else
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define HSYNTH_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(__arm__)
#define HSYNTH_PAUSE() __asm__ __volatile__("yield")
#else
#define HSYNTH_PAUSE() ((void)0)
#endif

namespace hsynth {

// ---------------------------------------------------------------- semaphore
struct WorkerPool::Sem {
#if defined(_WIN32)
    HANDLE h = CreateSemaphoreW(nullptr, 0, 0x7fffffff, nullptr);
    ~Sem() { CloseHandle(h); }
    void post() { ReleaseSemaphore(h, 1, nullptr); }
    void wait() { WaitForSingleObject(h, INFINITE); }
#elif defined(__APPLE__)
    dispatch_semaphore_t s = dispatch_semaphore_create(0);
    ~Sem() { dispatch_release(s); }
    void post() { dispatch_semaphore_signal(s); }
    void wait() { dispatch_semaphore_wait(s, DISPATCH_TIME_FOREVER); }
#else
    sem_t s;
    Sem() { sem_init(&s, 0, 0); }
    ~Sem() { sem_destroy(&s); }
    void post() { sem_post(&s); }
    void wait() {
        while (sem_wait(&s) != 0) {}  // EINTR
    }
#endif
};

namespace {

// Helpers render audio: give them the priority of audio work where the
// platform allows it without special permissions.
void raiseThreadPriority() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
    sched_param sp{};
    sp.sched_priority = 1;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0) {
        // no real-time scheduling (Android apps): use the audio nice level
        setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -19);
    }
#endif
}

}  // namespace

int autoRenderThreads() {
    const int hw = (int)std::thread::hardware_concurrency();
    return std::max(1, std::min(4, hw / 2));
}

WorkerPool::WorkerPool(int helpers) {
    numHelpers_ = std::max(0, std::min(kMaxHelpers, helpers));
    h_.reset(new Helper[(size_t)std::max(1, numHelpers_)]);
    for (int i = 0; i < numHelpers_; ++i) {
        h_[i].sem = new Sem();
        h_[i].thread = std::thread([this, i] { helperMain(i); });
    }
}

WorkerPool::~WorkerPool() {
    quit_.store(true);
    for (int i = 0; i < numHelpers_; ++i) h_[i].sem->post();
    for (int i = 0; i < numHelpers_; ++i) {
        if (h_[i].thread.joinable()) h_[i].thread.join();
        delete h_[i].sem;
    }
}

void WorkerPool::work(int worker) {
    for (;;) {
        const int i = next_.fetch_add(1, std::memory_order_relaxed);
        if (i >= count_) return;
        fn_(ctx_, worker, i);
    }
}

void WorkerPool::run(Fn fn, void* ctx, int count, int helpers) {
    if (count <= 0) return;
    helpers = std::max(0, std::min(helpers, numHelpers_));
    fn_ = fn;
    ctx_ = ctx;
    count_ = count;
    next_.store(0, std::memory_order_relaxed);
    for (int i = 0; i < helpers; ++i) {
        Helper& h = h_[i];
        h.slot.store(kOffered, std::memory_order_seq_cst);  // publishes the job (release)
        if (h.sleeping.load(std::memory_order_seq_cst)) h.sem->post();
    }
    work(0);
    for (int i = 0; i < helpers; ++i) {
        Helper& h = h_[i];
        int expected = kOffered;
        if (h.slot.compare_exchange_strong(expected, kIdle, std::memory_order_acq_rel)) continue;  // never taken
        while (h.slot.load(std::memory_order_acquire) != kDone) HSYNTH_PAUSE();
        h.slot.store(kIdle, std::memory_order_relaxed);
    }
}

int WorkerPool::threadIds(int* out, int max) const {
    int n = 0;
    for (int i = 0; i < numHelpers_ && n < max; ++i)
        if (const int t = h_[i].tid.load()) out[n++] = t;
    return n;
}

void WorkerPool::helperMain(int index) {
    raiseThreadPriority();
    Helper& h = h_[index];
#if !defined(_WIN32) && !defined(__APPLE__)
    h.tid.store((int)syscall(SYS_gettid));
#endif
    for (;;) {
        // spin a little (the next audio callback may come soon), then sleep
        bool offered = false;
        for (int spin = 0; spin < 4000 && !offered; ++spin) {
            offered = h.slot.load(std::memory_order_relaxed) == kOffered;
            if (!offered) HSYNTH_PAUSE();
        }
        if (!offered) {
            h.sleeping.store(true, std::memory_order_seq_cst);
            if (h.slot.load(std::memory_order_seq_cst) != kOffered && !quit_.load()) h.sem->wait();
            h.sleeping.store(false, std::memory_order_relaxed);
        }
        if (quit_.load()) return;
        int expected = kOffered;
        if (!h.slot.compare_exchange_strong(expected, kRunning, std::memory_order_acq_rel)) continue;  // withdrawn
        work(index + 1);
        h.slot.store(kDone, std::memory_order_release);
    }
}

}  // namespace hsynth
