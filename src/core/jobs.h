// core/jobs.h — deterministic parallel-for over a persistent worker pool.
//
// Deliberately NOT OpenMP. Two reasons: the target toolchain (TDM-GCC 10.3)
// ships without libgomp, and more importantly OpenMP makes no guarantee about
// how iterations are partitioned between threads. Bit-reproducibility requires
// that chunk boundaries be a pure function of (n, workerCount, chunkIndex) and
// nothing else -- not load, not timing, not scheduler mood.
//
// Rules that callers must honour (see ARCHITECTURE.md §5.2):
//   * A parallel body reads from one set of arrays and writes to another, or
//     writes only to its own index. No cross-chunk reads of freshly written data.
//   * Never accumulate floats across chunks with atomics. Float addition is not
//     associative, so a racing sum produces a different total every run. Use
//     parallelReduce, which sums per-chunk partials in ascending chunk order.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace gen {

class JobSystem {
public:
    JobSystem() = default;
    ~JobSystem() { shutdown(); }

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;

    // workerCount is the number of BACKGROUND threads. The calling thread also
    // takes a chunk, so the number of chunks is workerCount + 1. Pass 0 for a
    // fully serial (but still correct) system.
    void start(unsigned workerCount) {
        shutdown();
        if (workerCount + 1 > kMaxChunks) workerCount = kMaxChunks - 1;
        m_stop.store(false, std::memory_order_relaxed);
        m_workerCount = workerCount;
        m_workers.reserve(workerCount);
        for (unsigned i = 0; i < workerCount; ++i)
            m_workers.emplace_back([this, i] { workerLoop(i); });
    }

    void shutdown() {
        if (m_workers.empty()) { m_workerCount = 0; return; }
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_stop.store(true, std::memory_order_relaxed);
        }
        m_wake.notify_all();
        for (auto& t : m_workers) if (t.joinable()) t.join();
        m_workers.clear();
        m_workerCount = 0;
    }

    unsigned workerCount() const { return m_workerCount; }
    unsigned chunkCount()  const { return m_workerCount + 1; }

    // Suggested default: leave one core for the UI thread, floor of 1.
    static unsigned recommendedWorkers() {
        unsigned hw = std::thread::hardware_concurrency();
        if (hw <= 2) return 1;
        return hw - 1;
    }

    // fn(begin, end, chunkIndex) is invoked once per chunk with a half-open
    // contiguous range. Chunk boundaries depend only on (n, chunkCount).
    //
    // `minItems` is the point below which the call runs serially because thread
    // wakeup would cost more than it saved. It is per-call and NOT one global
    // number, because the right value depends entirely on how expensive the body
    // is. A tile update is a few nanoseconds an element and wants thousands
    // before parallelism pays; building an agent's 48 sensory inputs is a couple
    // of microseconds an element and pays from a few dozen. A single threshold
    // tuned for the cheap case silently ran the expensive agent stages
    // single-threaded at any population under 2048 -- which is to say, at every
    // population this program has actually reached.
    template <typename Fn>
    void parallelFor(size_t n, Fn&& fn, size_t minItems = kSerialThreshold) {
        const unsigned chunks = chunkCount();
        if (n == 0) return;
        if (chunks <= 1 || n < minItems) { fn(size_t(0), n, 0u); return; }

        m_body = [&fn, n, chunks](unsigned chunkIndex) {
            size_t b, e;
            chunkRange(n, chunks, chunkIndex, b, e);
            if (b < e) fn(b, e, chunkIndex);
        };
        dispatch(chunks);
        m_body = nullptr;
    }

    // Float/double reduction that is safe for determinism: each chunk sums into
    // its own slot, then the slots are added on the calling thread in ascending
    // chunk order. Same operand order every run, therefore same bits.
    template <typename T, typename Fn>
    T parallelReduce(size_t n, T identity, Fn&& fn) {
        const unsigned chunks = chunkCount();
        if (n == 0) return identity;
        if (chunks <= 1 || n < kSerialThreshold) return fn(size_t(0), n, 0u);

        // Stack-allocated so there is no heap traffic per reduction and no
        // type-punning of a shared buffer. dispatch() blocks until every chunk
        // has written its slot, so the array outlives all uses of it.
        T partials[kMaxChunks];
        for (unsigned i = 0; i < chunks; ++i) partials[i] = identity;
        // Captured as a pointer, not as an array: capturing the array by value
        // would copy it into a const lambda member and the writes below would
        // land on the copy.
        T* slots = partials;

        m_body = [&fn, n, chunks, slots, identity](unsigned chunkIndex) {
            size_t b, e;
            chunkRange(n, chunks, chunkIndex, b, e);
            slots[chunkIndex] = (b < e) ? fn(b, e, chunkIndex) : identity;
        };
        dispatch(chunks);
        m_body = nullptr;

        T total = identity;
        for (unsigned i = 0; i < chunks; ++i) total += partials[i];
        return total;
    }

    // Exposed so callers can pre-size per-chunk scratch buffers identically.
    static void chunkRange(size_t n, unsigned chunks, unsigned index,
                           size_t& begin, size_t& end) {
        const size_t base = n / chunks;
        const size_t rem  = n % chunks;
        // The first `rem` chunks get one extra element. Pure integer maths, so
        // the split is identical on every machine and every run.
        const size_t i = index;
        begin = base * i + (i < rem ? i : rem);
        end   = begin + base + (i < rem ? 1 : 0);
    }

    // Upper bound on chunks, so parallelReduce can use a stack array.
    static constexpr unsigned kMaxChunks = 128;

private:
    // Default for cheap per-element bodies. Callers with expensive bodies pass
    // something far smaller.
    static constexpr size_t kSerialThreshold = 2048;

public:
    // Suggested minimum for the agent stages, whose bodies cost microseconds
    // per element rather than nanoseconds.
    static constexpr size_t kAgentGrain = 48;

private:

    void dispatch(unsigned chunks) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_pending.store(chunks - 1, std::memory_order_relaxed);
            m_generation++;
        }
        m_wake.notify_all();

        // The calling thread always takes chunk 0, so it is never idle.
        m_body(0);

        std::unique_lock<std::mutex> lk(m_mutex);
        m_done.wait(lk, [this] { return m_pending.load(std::memory_order_relaxed) == 0; });
    }

    void workerLoop(unsigned workerIndex) {
        uint64_t seenGeneration = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_mutex);
                m_wake.wait(lk, [this, &seenGeneration] {
                    return m_stop.load(std::memory_order_relaxed) || m_generation != seenGeneration;
                });
                if (m_stop.load(std::memory_order_relaxed)) return;
                seenGeneration = m_generation;
            }
            // Worker i handles chunk i+1; chunk 0 belongs to the caller.
            m_body(workerIndex + 1);
            if (m_pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_done.notify_one();
            }
        }
    }

    std::vector<std::thread>          m_workers;
    std::function<void(unsigned)>     m_body;
    std::mutex                        m_mutex;
    std::condition_variable           m_wake;
    std::condition_variable           m_done;
    std::atomic<bool>                 m_stop{false};
    std::atomic<unsigned>             m_pending{0};
    uint64_t                          m_generation = 0;
    unsigned                          m_workerCount = 0;
};

}  // namespace gen
