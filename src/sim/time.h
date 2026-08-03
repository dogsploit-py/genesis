// sim/time.h — calendar, tick clock and speed control.
//
// One tick is one simulated hour. The calendar is deliberately regular
// (24h days, 30-day months, 12-month years => 8640 ticks/year) so that date
// arithmetic is exact integer division with no leap-year special cases, which
// matters because dates are compared and differenced constantly.
//
// Speed is expressed as SIMULATED HOURS PER REAL SECOND, so 1x means one tick
// per second. MAX is uncapped: the sim thread runs flat out and the achieved
// multiplier is measured and reported rather than promised.
#pragma once

#include <cstdint>
#include <string>

namespace gen {

constexpr uint64_t kHoursPerDay    = 24;
constexpr uint64_t kDaysPerMonth   = 30;
constexpr uint64_t kMonthsPerYear  = 12;
constexpr uint64_t kHoursPerMonth  = kHoursPerDay * kDaysPerMonth;    // 720
constexpr uint64_t kHoursPerYear   = kHoursPerMonth * kMonthsPerYear; // 8640

enum class Season : uint8_t { Spring = 0, Summer, Autumn, Winter };

const char* seasonName(Season s);
const char* monthName(int monthIndex);  // 0..11

struct DateTime {
    int64_t year = 0;   // 0-based
    int     month = 0;  // 0..11
    int     day = 0;    // 0..29
    int     hour = 0;   // 0..23

    std::string toString() const;      // "Year 12, Thermid 4, 09:00"
    std::string toShortString() const; // "Y12 M03 D04 09h"
};

DateTime tickToDate(uint64_t tick);
uint64_t dateToTick(int64_t year, int month, int day, int hour);

inline Season seasonOfTick(uint64_t tick) {
    const int month = static_cast<int>((tick / kHoursPerMonth) % kMonthsPerYear);
    return static_cast<Season>((month / 3) % 4);
}

// Fraction through the year, 0..1. Drives insolation and seasonal temperature.
inline double yearPhase(uint64_t tick) {
    return static_cast<double>(tick % kHoursPerYear) / static_cast<double>(kHoursPerYear);
}

// Fraction through the day, 0..1, where 0 is midnight and 0.5 is noon.
inline double dayPhase(uint64_t tick) {
    return static_cast<double>(tick % kHoursPerDay) / static_cast<double>(kHoursPerDay);
}

// ---------------------------------------------------------------------------

// The discrete speed presets exposed as buttons in the time bar. MAX is
// represented by the separate `maxSpeed` flag rather than a huge multiplier,
// because "uncapped" and "very fast" are different behaviours: uncapped skips
// the pacing accumulator entirely.
constexpr double kSpeedPresets[] = {1.0, 2.0, 5.0, 10.0, 25.0, 50.0, 100.0};
constexpr int    kSpeedPresetCount = 7;

class SpeedController {
public:
    // Requested speed in simulated hours per real second.
    double requested() const { return m_requested; }
    bool   paused() const { return m_paused; }
    bool   maxSpeed() const { return m_max; }

    void setPaused(bool p) { m_paused = p; if (!p) m_accumulator = 0.0; }
    void togglePause() { setPaused(!m_paused); }
    void setSpeed(double hoursPerSecond) {
        m_requested = (hoursPerSecond < 0.0) ? 0.0 : hoursPerSecond;
        m_max = false;
        m_paused = false;
        m_accumulator = 0.0;
    }
    void setMax() { m_max = true; m_paused = false; m_accumulator = 0.0; }

    // Step requests are honoured once even while paused; this is how the
    // +1 tick / +1 day / +1 month / +1 year buttons work.
    void requestSteps(uint64_t n) { m_stepRequest += n; }
    uint64_t takeStepRequest() { const uint64_t n = m_stepRequest; m_stepRequest = 0; return n; }
    uint64_t pendingSteps() const { return m_stepRequest; }

    // How many ticks are owed for `realSeconds` of wall clock. Returns
    // UINT64_MAX-as-"unlimited" via the `unlimited` out-parameter when in MAX
    // mode, in which case the caller runs whatever fits its time budget.
    uint64_t ticksOwed(double realSeconds, bool& unlimited);

    void resetAccumulator() { m_accumulator = 0.0; }

private:
    double   m_requested = 1.0;
    double   m_accumulator = 0.0;
    uint64_t m_stepRequest = 0;
    bool     m_paused = true;   // worlds start paused so you can look before it moves
    bool     m_max = false;
};

// ---------------------------------------------------------------------------

// "Run until" targets. The sim runs at MAX speed until the condition trips,
// then auto-pauses. Conditions that depend on agents are declared here and
// evaluated in later milestones; the ones M1 can evaluate are marked.
enum class RunUntilKind : uint8_t {
    None = 0,
    Year,            // M1: reach a given simulated year
    ElapsedYears,    // M1: run for N more simulated years
    PopulationBelow, // M2
    PopulationAbove, // M2
    Extinction,      // M2
    FirstDiscovery,  // M6: named discovery, e.g. "iron smelting"
    Custom           // M5: Lua predicate
};

struct RunUntil {
    RunUntilKind kind = RunUntilKind::None;
    double       value = 0.0;
    std::string  text;        // discovery name or Lua expression
    uint64_t     startTick = 0;
    bool         active = false;

    void clear() { kind = RunUntilKind::None; active = false; value = 0.0; text.clear(); }
};

// A named snapshot of a moment you can jump back to.
struct Bookmark {
    std::string name;
    uint64_t    tick = 0;
    std::string filePath;   // snapshot written to disk
};

}  // namespace gen
