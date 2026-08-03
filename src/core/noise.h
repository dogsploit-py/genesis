// core/noise.h — Perlin noise and the fractal variants worldgen needs.
//
// Hand-rolled rather than pulled from a library so the permutation table is
// seeded from our own RNG stream and the output is byte-identical everywhere.
#pragma once

#include <cstdint>

#include "core/rng.h"

namespace gen {

class Noise {
public:
    Noise() { seed(0); }
    explicit Noise(uint64_t s) { seed(s); }

    // Fisher-Yates shuffle of 0..255 using the supplied seed, duplicated to 512
    // entries so the hash lookups never need a modulo.
    void seed(uint64_t s);
    void seed(Rng& rng);

    // Classic 2D Perlin, output in roughly [-1, 1].
    float perlin(float x, float y) const;

    // Fractal Brownian motion: sum of `octaves` Perlin layers, each at
    // `lacunarity` times the frequency and `gain` times the amplitude of the
    // last. Normalised so the result stays in roughly [-1, 1].
    float fbm(float x, float y, int octaves, float lacunarity, float gain) const;

    // Ridged multifractal: 1 - |perlin|, which turns the zero crossings into
    // sharp crests. This is what makes mountain ranges look like ranges rather
    // than lumps. Output in [0, 1].
    float ridged(float x, float y, int octaves, float lacunarity, float gain) const;

    // Billowy: |perlin|, giving rounded hills. Output in [0, 1].
    float billow(float x, float y, int octaves, float lacunarity, float gain) const;

    // Domain warp: offset the sample point by another noise field before
    // sampling. Breaks up the grid-aligned look of raw fBm.
    float warpedFbm(float x, float y, int octaves, float lacunarity, float gain,
                    float warpAmount) const;

private:
    static float fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }
    static float lerp(float a, float b, float t) { return a + t * (b - a); }
    static float grad(int hash, float x, float y);

    uint8_t m_perm[512] = {0};
};

}  // namespace gen
