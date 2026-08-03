// core/rng.h — deterministic pseudo-random number generation.
//
// xoshiro256++ (Blackman & Vigna 2018) seeded through SplitMix64. Chosen over
// the standard library because <random> engines are not specified bit-for-bit
// across implementations, and reproducibility is a hard requirement here.
//
// The important structure is RngBank: one INDEPENDENT stream per subsystem. A
// god-mode intervention that consumes extra randomness from the God stream
// cannot shift the number sequence seen by Climate or Genetics. Without this,
// spawning an agent would silently change the weather three years later.
#pragma once

#include <cstdint>
#include <cmath>

namespace gen {

// SplitMix64 — used only to expand a single seed into well-mixed state words.
inline uint64_t splitmix64(uint64_t& x) {
    uint64_t z = (x += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

class Rng {
public:
    Rng() { seed(0x243F6A8885A308D3ULL); }
    explicit Rng(uint64_t s) { seed(s); }

    void seed(uint64_t s) {
        uint64_t x = s;
        for (int i = 0; i < 4; ++i) m_s[i] = splitmix64(x);
        // Guard against the all-zero state, which xoshiro cannot escape.
        if ((m_s[0] | m_s[1] | m_s[2] | m_s[3]) == 0) m_s[0] = 0x9E3779B97F4A7C15ULL;
    }

    uint64_t next() {
        const uint64_t r = rotl(m_s[0] + m_s[3], 23) + m_s[0];
        const uint64_t t = m_s[1] << 17;
        m_s[2] ^= m_s[0];
        m_s[3] ^= m_s[1];
        m_s[1] ^= m_s[2];
        m_s[0] ^= m_s[3];
        m_s[2] ^= t;
        m_s[3] = rotl(m_s[3], 45);
        return r;
    }

    // Uniform in [0, 1). 53 significant bits, the most a double can hold.
    double nextDouble() { return static_cast<double>(next() >> 11) * 0x1.0p-53; }
    float  nextFloat()  { return static_cast<float>(next() >> 40) * 0x1.0p-24f; }

    // Uniform in [0, bound). Lemire's debiased multiply-shift; rejection loop
    // keeps the distribution exactly uniform rather than modulo-biased.
    uint32_t nextBounded(uint32_t bound) {
        if (bound == 0) return 0;
        uint64_t m = static_cast<uint64_t>(static_cast<uint32_t>(next() >> 32)) * bound;
        uint32_t l = static_cast<uint32_t>(m);
        if (l < bound) {
            const uint32_t t = (0u - bound) % bound;
            while (l < t) {
                m = static_cast<uint64_t>(static_cast<uint32_t>(next() >> 32)) * bound;
                l = static_cast<uint32_t>(m);
            }
        }
        return static_cast<uint32_t>(m >> 32);
    }

    int32_t range(int32_t lo, int32_t hi) {  // inclusive lo, exclusive hi
        if (hi <= lo) return lo;
        return lo + static_cast<int32_t>(nextBounded(static_cast<uint32_t>(hi - lo)));
    }
    float rangef(float lo, float hi) { return lo + (hi - lo) * nextFloat(); }
    bool chance(double p) { return nextDouble() < p; }

    // Box-Muller with a cached second variate. Deterministic because the cache
    // is part of the object state and is serialised with it.
    float gaussian(float mean = 0.0f, float sigma = 1.0f) {
        if (m_hasSpare) {
            m_hasSpare = false;
            return mean + sigma * m_spare;
        }
        double u, v, s;
        do {
            u = nextDouble() * 2.0 - 1.0;
            v = nextDouble() * 2.0 - 1.0;
            s = u * u + v * v;
        } while (s >= 1.0 || s == 0.0);
        const double f = std::sqrt(-2.0 * std::log(s) / s);
        m_spare = static_cast<float>(v * f);
        m_hasSpare = true;
        return mean + sigma * static_cast<float>(u * f);
    }

    // Raw state access, for snapshot save/load.
    const uint64_t* state() const { return m_s; }
    void setState(const uint64_t* s, bool hasSpare, float spare) {
        for (int i = 0; i < 4; ++i) m_s[i] = s[i];
        m_hasSpare = hasSpare;
        m_spare = spare;
    }
    bool  hasSpare() const { return m_hasSpare; }
    float spare()    const { return m_spare; }

private:
    static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

    uint64_t m_s[4] = {0, 0, 0, 0};
    float    m_spare = 0.0f;
    bool     m_hasSpare = false;
};

// One stream per subsystem. Adding an entry is safe at any time: streams are
// seeded from the enum value, so a new stream cannot disturb existing ones as
// long as it is appended before Count.
enum class Stream : uint32_t {
    WorldGen = 0,
    Climate,
    Hydrology,
    Geology,
    Ecology,
    Genetics,
    Mutation,
    Development,
    Brain,
    Behavior,
    Repro,
    Social,
    Culture,
    Disease,
    Chemistry,
    God,
    Misc,
    Count
};

inline const char* streamName(Stream s) {
    switch (s) {
        case Stream::WorldGen:    return "WorldGen";
        case Stream::Climate:     return "Climate";
        case Stream::Hydrology:   return "Hydrology";
        case Stream::Geology:     return "Geology";
        case Stream::Ecology:     return "Ecology";
        case Stream::Genetics:    return "Genetics";
        case Stream::Mutation:    return "Mutation";
        case Stream::Development: return "Development";
        case Stream::Brain:       return "Brain";
        case Stream::Behavior:    return "Behavior";
        case Stream::Repro:       return "Repro";
        case Stream::Social:      return "Social";
        case Stream::Culture:     return "Culture";
        case Stream::Disease:     return "Disease";
        case Stream::Chemistry:   return "Chemistry";
        case Stream::God:         return "God";
        case Stream::Misc:        return "Misc";
        case Stream::Count:       break;
    }
    return "?";
}

class RngBank {
public:
    RngBank() { reseed(0); }

    void reseed(uint64_t worldSeed) {
        m_worldSeed = worldSeed;
        for (uint32_t i = 0; i < static_cast<uint32_t>(Stream::Count); ++i) {
            // Decorrelate stream seeds: mix the stream index by the 64-bit
            // golden ratio before combining, so adjacent stream ids do not
            // produce correlated SplitMix64 expansions.
            m_streams[i].seed(worldSeed ^ (0x9E3779B97F4A7C15ULL * (i + 1)));
        }
    }

    Rng& operator[](Stream s) { return m_streams[static_cast<uint32_t>(s)]; }
    Rng& get(Stream s)        { return m_streams[static_cast<uint32_t>(s)]; }
    uint64_t worldSeed() const { return m_worldSeed; }

    static constexpr uint32_t kCount = static_cast<uint32_t>(Stream::Count);

private:
    Rng      m_streams[kCount];
    uint64_t m_worldSeed = 0;
};

}  // namespace gen
