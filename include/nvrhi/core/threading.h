#ifndef NVRHI_CORE_THREADING_H
#define NVRHI_CORE_THREADING_H

#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <nvrhi/core/memory.h>

// X86 PAUSE or ARM YIELD: reduces contention between hyper-threads while spinning.
#if defined(_MSC_VER) && ((_M_IX86_FP >= 2) || defined(_M_X64))
#include <emmintrin.h>
#define NVRHI_CPU_PAUSE() _mm_pause()
#elif (defined(__clang__) || defined(__GNUC__)) && (defined(__i386__) || defined(__x86_64__))
#define NVRHI_CPU_PAUSE() __builtin_ia32_pause()
#elif (defined(__clang__) || defined(__GNUC__)) && (defined(__arm__) || defined(__aarch64__))
#define NVRHI_CPU_PAUSE() asm volatile("yield")
#else
#define NVRHI_CPU_PAUSE() ((void)0)
#endif

namespace nvrhi {

/// Spin lock implementation
class SpinLock {
public:
    // See https://rigtorp.se/spinlock/
    SpinLock() noexcept {}

    // clang-format off
    SpinLock             (const SpinLock&)  = delete;
    SpinLock& operator = (const SpinLock&)  = delete;
    SpinLock             (      SpinLock&&) = delete;
    SpinLock& operator = (      SpinLock&&) = delete;
    // clang-format on

    void lock() noexcept {
        while (true) {
            // Assume that lock is free on the first try.
            const auto WasLocked = m_IsLocked.exchange(true, std::memory_order_acquire);
            if (!WasLocked)
                return;  // The lock was not acquired when this thread performed the exchange

            Wait();
        }
    }

    bool try_lock() noexcept {
        // First do a relaxed load to check if lock is free in order to prevent
        // unnecessary cache misses if someone does while (!try_lock()).
        if (is_locked())
            return false;

        const auto WasLocked = m_IsLocked.exchange(true, std::memory_order_acquire);
        return !WasLocked;
    }

    void unlock() noexcept {
        NVRHI_VERIFY(
            is_locked(),
            "Attempting to unlock a spin lock that is not locked. This is a strong indication of a flawed logic.");
        m_IsLocked.store(false, std::memory_order_release);
    }

    bool is_locked() const noexcept {
        // Use relaxed load as we only want to check the value.
        // To impose ordering, lock()/try_lock() must be used.
        return m_IsLocked.load(std::memory_order_relaxed);
    }

private:
    void Wait() noexcept {
        // Wait for the lock to be released without generating cache misses.
        constexpr size_t NumAttemptsToYield = 64;
        for (size_t Attempt = 0; Attempt < NumAttemptsToYield; ++Attempt) {
            if (!is_locked())
                return;
            NVRHI_CPU_PAUSE();
        }
        std::this_thread::yield();
    }

private:
    std::atomic<bool> m_IsLocked{ false };
};

using SpinLockGuard = std::lock_guard<SpinLock>;

class Signal {
public:
    Signal() {
        m_SignaledValue.store(0);
        m_NumThreadsAwaken.store(0);
    }

    // clang-format off
    Signal           (const Signal&) = delete;
    Signal& operator=(const Signal&) = delete;
    Signal           (Signal&&)      = delete;
    Signal& operator=(Signal&&)      = delete;
    // clang-format on

    // http://en.cppreference.com/w/cpp/thread/condition_variable
    void Trigger(bool NotifyAll = false, int SignalValue = 1) {
        NVRHI_VERIFY(SignalValue != 0, "Signal value must not be zero");

        //  The thread that intends to modify the variable has to
        //  * acquire a std::mutex (typically via std::lock_guard)
        //  * perform the modification while the lock is held
        //  * execute notify_one or notify_all on the std::condition_variable (the lock does not need to be held for
        //  notification)
        {
            // std::condition_variable works only with std::unique_lock<std::mutex>
            std::lock_guard<std::mutex> Lock{ m_Mutex };
            NVRHI_VERIFY(SignalValue != 0, "Signal value must not be 0");
            NVRHI_VERIFY(
                m_SignaledValue.load() == 0 && m_NumThreadsAwaken.load() == 0,
                "Not all threads have been awaken since the signal was triggered last time, or the signal has not been "
                "reset");
            m_SignaledValue.store(SignalValue);
        }
        // Unlocking is done before notifying, to avoid waking up the waiting
        // thread only to block again (see notify_one for details)
        if (NotifyAll)
            m_CondVar.notify_all();
        else
            m_CondVar.notify_one();
    }

    // WARNING!
    // If multiple threads are waiting for a signal in an infinite loop,
    // autoresetting the signal does not guarantee that one thread cannot
    // go through the loop twice. In this case, every thread must wait for its
    // own auto-reset signal or the threads must be blocked by another signal

    int Wait(bool AutoReset = false, int NumThreadsWaiting = 0) {
        //  Any thread that intends to wait on std::condition_variable has to
        //  * acquire a std::unique_lock<std::mutex>, on the SAME MUTEX as used to protect the shared variable
        //  * execute wait, wait_for, or wait_until. The wait operations atomically release the mutex
        //    and suspend the execution of the thread.
        //  * When the condition variable is notified, a timeout expires, or a spurious wakeup occurs,
        //    the thread is awakened, and the mutex is atomically reacquired:
        //    - The thread should then check the condition and resume waiting if the wake-up was spurious.
        std::unique_lock<std::mutex> Lock(m_Mutex);
        // It is safe to check m_SignaledValue since we are holding
        // the mutex
        if (m_SignaledValue.load() == 0) {
            m_CondVar.wait(Lock, [&] { return m_SignaledValue.load() != 0; });
        }
        auto SignaledValue = m_SignaledValue.load();
        // Update the number of threads awaken while holding the mutex
        const auto NumThreadsAwaken = m_NumThreadsAwaken.fetch_add(1) + 1;
        // fetch_add returns the original value immediately preceding the addition.
        if (AutoReset) {
            NVRHI_VERIFY(
                NumThreadsWaiting > 0, "Number of waiting threads must not be 0 when auto resetting the signal");
            // Reset the signal while holding the mutex. If Trigger() is executed by another
            // thread, it will wait until we release the mutex
            if (NumThreadsAwaken == NumThreadsWaiting) {
                m_SignaledValue.store(0);
                m_NumThreadsAwaken.store(0);
            }
        }
        return SignaledValue;
    }

    void Reset() {
        std::lock_guard<std::mutex> Lock{ m_Mutex };
        m_SignaledValue.store(0);
        m_NumThreadsAwaken.store(0);
    }

    bool IsTriggered() const { return m_SignaledValue.load() != 0; }

private:
    std::mutex m_Mutex;
    std::condition_variable m_CondVar;
    std::atomic_int m_SignaledValue{ 0 };
    std::atomic_int m_NumThreadsAwaken{ 0 };
};

namespace details {
#if defined(_WIN64) || (defined(__linux__) && defined(__x86_64__))
inline constexpr uint64_t SListHeaderCounterBits = 17;
inline constexpr uint64_t SListHeaderPtrMask = (~0ull) >> SListHeaderCounterBits;
inline constexpr uint64_t SListHeaderCounterMask = ~SListHeaderPtrMask;
inline constexpr uint64_t SListHeaderCounterInc = SListHeaderPtrMask + 1;
#else
inline constexpr uint64_t SListHeaderCounterBits = 32;
inline constexpr uint64_t SListHeaderPtrMask = (-1ULL) >> SListHeaderCounterBits;
inline constexpr uint64_t SListHeaderCounterMask = ~SListHeaderPtrMask;
inline constexpr uint64_t SListHeaderCounterInc = SListHeaderPtrMask + 1;
#endif

inline constexpr uint64_t SharedSlimLockExclusiveMask = 1ULL << 63;
inline constexpr uint64_t SharedSlimLockSharedMask = ~SharedSlimLockExclusiveMask;
}  // namespace details

struct LFStackEntry {
    LFStackEntry *next;
};

/**
 * Lock free stack
 * @ref gcc/+/master/libsanitizer/sanitizer_common/sanitizer_lfstack.h
 */
struct LFStack {
    LFStack() : head_{0} {}
    ~LFStack() {}

    bool Empty() const {
        uint64_t cmp = head_.load(std::memory_order_relaxed);
        return (cmp & details::SListHeaderPtrMask) == 0;
    }

    LFStackEntry *Top() const {
        uint64_t cmp = head_.load(std::memory_order_relaxed);
        return (LFStackEntry *)(uintptr_t)(cmp & details::SListHeaderPtrMask);
    }

    void Push(LFStackEntry *p) {
        uint64_t cmp = head_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t cnt = (cmp & details::SListHeaderCounterMask) + details::SListHeaderCounterInc;
            uint64_t xch = (uint64_t)(uintptr_t)p | cnt;
            p->next = (LFStackEntry *)(uintptr_t)(cmp & details::SListHeaderPtrMask);
            if (head_.compare_exchange_weak(cmp, xch, std::memory_order_release)) break;
        }
    }

    void Push(LFStackEntry *slice, LFStackEntry *slice_end) {
        uint64_t cmp = head_.load(std::memory_order_relaxed);
        for (;;) {
            uint64_t cnt = (cmp & details::SListHeaderCounterMask) + details::SListHeaderCounterInc;
            uint64_t xch = (uint64_t)(uintptr_t)slice | cnt;
            slice_end->next = (LFStackEntry *)(uintptr_t)(cmp & details::SListHeaderPtrMask);
            if (head_.compare_exchange_weak(cmp, xch, std::memory_order_release)) break;
        }
    }

    LFStackEntry *Pop() {
        uint64_t cmp = head_.load(std::memory_order_acquire);
        for (;;) {
            LFStackEntry *cur = (LFStackEntry *)(uintptr_t)(cmp & details::SListHeaderPtrMask);
            if (cur == nullptr) return nullptr;

            LFStackEntry *nxt = cur->next;
            uint64_t cnt = (cmp & details::SListHeaderCounterMask);
            uint64_t xch = (uint64_t)(uintptr_t)nxt | cnt;
            if (head_.compare_exchange_weak(cmp, xch, std::memory_order_acquire)) return cur;
        }
    }

    LFStackEntry *Flush() {
        uint64_t cmp = head_.load(std::memory_order_acquire);
        for (;;) {
            LFStackEntry *cur = (LFStackEntry *)(uintptr_t)(cmp & details::SListHeaderPtrMask);
            if (cur == nullptr) return nullptr;

            uint64_t cnt = (cmp & details::SListHeaderCounterMask);
            uint64_t xch = cnt;
            if (head_.compare_exchange_weak(cmp, xch, std::memory_order_acquire)) return cur;
        }
    }

    std::atomic<uint64_t> head_;
};

struct SharedSpinLock {
    SharedSpinLock() : shared_cnt_{} {}
    ~SharedSpinLock() {}

    void lock() noexcept {
        while (true) {
            uint64_t cmp = 0;
            uint64_t xch = details::SharedSlimLockExclusiveMask;
            if (shared_cnt_.compare_exchange_weak(cmp, xch, std::memory_order_acquire)) break;

            Wait();
        }
    }

    bool try_lock() noexcept {
        if (is_locked()) return false;

        uint64_t cmp = 0;
        uint64_t xch = details::SharedSlimLockExclusiveMask;
        return shared_cnt_.compare_exchange_weak(cmp, xch, std::memory_order_acquire);
    }

    void unlock() noexcept { shared_cnt_.store(0, std::memory_order_relaxed); }

    void lock_shared() noexcept {
        uint64_t cmp = shared_cnt_.load(std::memory_order_release);
        while (true) {
            cmp &= details::SharedSlimLockSharedMask;
            uint64_t xch = cmp + 1;
            if (shared_cnt_.compare_exchange_weak(cmp, xch)) break;

            WaitShared();
        }
    }

    bool try_lock_shared() noexcept {
        if (is_locked()) return false;
        uint64_t cmp = shared_cnt_.load(std::memory_order_release);
        cmp &= details::SharedSlimLockSharedMask;
        uint64_t xch = cmp + 1;
        return shared_cnt_.compare_exchange_weak(cmp, xch);
    }

    void unlock_shared() noexcept {
        shared_cnt_.fetch_sub(1, std::memory_order_release);
    }

 private:
    bool is_locked() noexcept {
        uint64_t cnt = shared_cnt_.load(std::memory_order_relaxed);
        return cnt == details::SharedSlimLockExclusiveMask;
    }

    bool is_locked_shared() noexcept {
        uint64_t cnt = shared_cnt_.load(std::memory_order_relaxed);
        return (details::SharedSlimLockSharedMask & cnt) == cnt;
    }

    void Wait() {
        // Wait for the lock to be released without generating cache misses.
        constexpr size_t NumAttemptsToYield = 64;
        for (size_t Attempt = 0; Attempt < NumAttemptsToYield; ++Attempt) {
            if (!is_locked()) return;
            NVRHI_CPU_PAUSE();
        }
        std::this_thread::yield();
    }

    void WaitShared() {
        // Wait for the lock to be released without generating cache misses.
        constexpr size_t NumAttemptsToYield = 64;
        for (size_t Attempt = 0; Attempt < NumAttemptsToYield; ++Attempt) {
            if (!is_locked_shared()) return;
            NVRHI_CPU_PAUSE();
        }
        std::this_thread::yield();
    }

    std::atomic<uint64_t> shared_cnt_;
};

} // namespace nvrhi


#endif /* NVRHI_CORE_THREADING_H */
