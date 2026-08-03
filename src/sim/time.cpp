#include "sim/time.h"

#include <cstdio>

namespace gen {

const char* seasonName(Season s) {
    switch (s) {
        case Season::Spring: return "Spring";
        case Season::Summer: return "Summer";
        case Season::Autumn: return "Autumn";
        case Season::Winter: return "Winter";
    }
    return "?";
}

// Invented month names, so the calendar reads as this world's rather than
// borrowing Earth's and implying an Earth year length it does not have.
const char* monthName(int monthIndex) {
    static const char* kNames[12] = {
        "Thaw",   "Seedfall", "Greening", "Highsun",
        "Emberis","Dustwind",  "Harvest",  "Falling",
        "Rimefirst","Deepcold","Longnight","Waking"
    };
    if (monthIndex < 0 || monthIndex > 11) return "?";
    return kNames[monthIndex];
}

std::string DateTime::toString() const {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Year %lld, %s %d, %02d:00",
                  static_cast<long long>(year), monthName(month), day + 1, hour);
    return std::string(buf);
}

std::string DateTime::toShortString() const {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Y%lld M%02d D%02d %02dh",
                  static_cast<long long>(year), month + 1, day + 1, hour);
    return std::string(buf);
}

DateTime tickToDate(uint64_t tick) {
    DateTime d;
    d.hour  = static_cast<int>(tick % kHoursPerDay);
    const uint64_t days = tick / kHoursPerDay;
    d.day   = static_cast<int>(days % kDaysPerMonth);
    const uint64_t months = days / kDaysPerMonth;
    d.month = static_cast<int>(months % kMonthsPerYear);
    d.year  = static_cast<int64_t>(months / kMonthsPerYear);
    return d;
}

uint64_t dateToTick(int64_t year, int month, int day, int hour) {
    if (year < 0) year = 0;
    if (month < 0) month = 0;
    if (month > 11) month = 11;
    if (day < 0) day = 0;
    if (day > 29) day = 29;
    if (hour < 0) hour = 0;
    if (hour > 23) hour = 23;
    return static_cast<uint64_t>(year) * kHoursPerYear
         + static_cast<uint64_t>(month) * kHoursPerMonth
         + static_cast<uint64_t>(day) * kHoursPerDay
         + static_cast<uint64_t>(hour);
}

uint64_t SpeedController::ticksOwed(double realSeconds, bool& unlimited) {
    unlimited = false;

    // Step requests take priority and are honoured regardless of pause state.
    if (m_stepRequest > 0) return 0;  // caller drains via takeStepRequest()

    if (m_paused) return 0;
    if (m_max) { unlimited = true; return 0; }

    m_accumulator += realSeconds * m_requested;
    // Guard against a huge accumulated debt after the process was suspended
    // (debugger break, laptop lid). Without this the sim would sprint to catch
    // up on wall-clock time it was never meant to owe.
    const double kMaxDebtTicks = 100000.0;
    if (m_accumulator > kMaxDebtTicks) m_accumulator = kMaxDebtTicks;

    if (m_accumulator < 1.0) return 0;
    const uint64_t n = static_cast<uint64_t>(m_accumulator);
    m_accumulator -= static_cast<double>(n);
    return n;
}

}  // namespace gen
