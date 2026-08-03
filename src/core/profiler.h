// core/profiler.h — per-stage tick timing.
//
// Written before the optimisation pass rather than after it, because the whole
// point of M9 is to make the program faster where it is actually slow. Guessing
// at that is how you end up hand-vectorising a stage that costs 0.3% of the
// tick while the real cost sits in a std::unordered_map lookup nobody looked at.
//
// The timer is a plain steady_clock pair per stage and a rolling mean. It costs
// two clock reads per stage per tick, which at ~15 stages is under a microsecond
// against tick costs measured in milliseconds. It is always on: a profiler you
// have to switch on is a profiler that is off when you need it.
//
// It is NOT part of the simulation. It reads no simulation state, writes none,
// and consumes no randomness, so enabling or disabling it cannot change a run.
// That is why the timings are excluded from snapshots.
#pragma once

#include <chrono>
#include <cstdint>

namespace gen {

// Every stage that gets its own timer. Order here is tick order, so the UI can
// print the list straight down and have it read as the tick.
enum class Stage : uint8_t {
    Disasters = 0,
    Clock,
    Thermal,
    Hydrology,
    Ecology,
    Weather,
    Geology,
    SpatialIndex,
    Sense,
    Think,
    Neighbours,
    Act,
    Physics,
    Metabolism,
    Reproduction,
    Chemistry,
    Economy,
    Reap,
    Species,
    Telemetry,
    PopGenetics,
    Autosave,
    Render,
    Count
};

const char* stageName(Stage s);
// One line on what the stage does, for the profiler panel's tooltips.
const char* stageNote(Stage s);
// True for stages that only run on a schedule, so a zero reading means "did not
// run this tick" rather than "was free".
bool stageIsPeriodic(Stage s);

class Profiler {
public:
    using Clock = std::chrono::steady_clock;

    // Called once per tick, before any stage. Rolls the per-tick accumulators
    // into the rolling means.
    void beginTick() {
        for (int i = 0; i < static_cast<int>(Stage::Count); ++i) {
            // Exponential moving average with a long tail, so a single
            // expensive periodic tick does not make the display jump and a
            // genuine regression still shows up within a second or so.
            m_mean[i] = m_mean[i] * (1.0 - kAlpha) + m_thisTick[i] * kAlpha;
            m_total[i] += m_thisTick[i];
            if (m_thisTick[i] > m_peak[i]) m_peak[i] = m_thisTick[i];
            m_thisTick[i] = 0.0;
        }
        ++m_ticks;
    }

    void begin(Stage s) { m_start[static_cast<int>(s)] = Clock::now(); }
    void end(Stage s) {
        const int i = static_cast<int>(s);
        const std::chrono::duration<double, std::milli> d = Clock::now() - m_start[i];
        // Accumulated rather than assigned, because a stage can legitimately be
        // entered more than once in a tick.
        m_thisTick[i] += d.count();
        ++m_calls[i];
    }

    double meanMs(Stage s) const { return m_mean[static_cast<int>(s)]; }
    double peakMs(Stage s) const { return m_peak[static_cast<int>(s)]; }
    double totalMs(Stage s) const { return m_total[static_cast<int>(s)]; }
    uint64_t calls(Stage s) const { return m_calls[static_cast<int>(s)]; }
    uint64_t ticks() const { return m_ticks; }

    double meanTickMs() const {
        double sum = 0.0;
        for (int i = 0; i < static_cast<int>(Stage::Count); ++i) {
            if (static_cast<Stage>(i) == Stage::Render) continue;   // not part of the tick
            sum += m_mean[i];
        }
        return sum;
    }
    double totalTickMs() const {
        double sum = 0.0;
        for (int i = 0; i < static_cast<int>(Stage::Count); ++i) {
            if (static_cast<Stage>(i) == Stage::Render) continue;
            sum += m_total[i];
        }
        return sum;
    }

    // Which stage currently costs the most. This is what the status bar reports
    // as the bottleneck, replacing the old "last stage that ran" guess.
    Stage dominant() const {
        int best = 0;
        double bestMs = -1.0;
        for (int i = 0; i < static_cast<int>(Stage::Count); ++i) {
            if (static_cast<Stage>(i) == Stage::Render) continue;
            if (m_mean[i] > bestMs) { bestMs = m_mean[i]; best = i; }
        }
        return static_cast<Stage>(best);
    }

    void reset() {
        for (int i = 0; i < static_cast<int>(Stage::Count); ++i) {
            m_mean[i] = m_thisTick[i] = m_total[i] = m_peak[i] = 0.0;
            m_calls[i] = 0;
        }
        m_ticks = 0;
    }

private:
    static constexpr double kAlpha = 0.02;

    Clock::time_point m_start[static_cast<int>(Stage::Count)] = {};
    double m_thisTick[static_cast<int>(Stage::Count)] = {};
    double m_mean[static_cast<int>(Stage::Count)] = {};
    double m_total[static_cast<int>(Stage::Count)] = {};
    double m_peak[static_cast<int>(Stage::Count)] = {};
    uint64_t m_calls[static_cast<int>(Stage::Count)] = {};
    uint64_t m_ticks = 0;
};

// A scope guard, so a stage cannot be left un-ended on an early return.
class ScopedStage {
public:
    ScopedStage(Profiler& p, Stage s) : m_p(p), m_s(s) { p.begin(s); }
    ~ScopedStage() { m_p.end(m_s); }
    ScopedStage(const ScopedStage&) = delete;
    ScopedStage& operator=(const ScopedStage&) = delete;

private:
    Profiler& m_p;
    Stage m_s;
};

}  // namespace gen
