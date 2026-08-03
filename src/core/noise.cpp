#include "core/noise.h"

#include <cmath>

namespace gen {

void Noise::seed(uint64_t s) {
    Rng rng(s);
    seed(rng);
}

void Noise::seed(Rng& rng) {
    uint8_t p[256];
    for (int i = 0; i < 256; ++i) p[i] = static_cast<uint8_t>(i);
    // Fisher-Yates, descending, so the index draw is always in [0, i].
    for (int i = 255; i > 0; --i) {
        const int j = static_cast<int>(rng.nextBounded(static_cast<uint32_t>(i + 1)));
        const uint8_t t = p[i];
        p[i] = p[j];
        p[j] = t;
    }
    for (int i = 0; i < 256; ++i) {
        m_perm[i] = p[i];
        m_perm[i + 256] = p[i];
    }
}

float Noise::grad(int hash, float x, float y) {
    // 8 gradient directions selected by the low 3 bits: the standard 2D
    // reduction of Perlin's 3D gradient set.
    switch (hash & 7) {
        case 0: return  x + y;
        case 1: return -x + y;
        case 2: return  x - y;
        case 3: return -x - y;
        case 4: return  x;
        case 5: return -x;
        case 6: return  y;
        default: return -y;
    }
}

float Noise::perlin(float x, float y) const {
    const float fx = std::floor(x);
    const float fy = std::floor(y);
    const int xi = static_cast<int>(static_cast<int64_t>(fx) & 255);
    const int yi = static_cast<int>(static_cast<int64_t>(fy) & 255);
    const float xf = x - fx;
    const float yf = y - fy;

    const float u = fade(xf);
    const float v = fade(yf);

    const int aa = m_perm[m_perm[xi] + yi];
    const int ab = m_perm[m_perm[xi] + yi + 1];
    const int ba = m_perm[m_perm[xi + 1] + yi];
    const int bb = m_perm[m_perm[xi + 1] + yi + 1];

    const float x1 = lerp(grad(aa, xf, yf),        grad(ba, xf - 1.0f, yf),        u);
    const float x2 = lerp(grad(ab, xf, yf - 1.0f), grad(bb, xf - 1.0f, yf - 1.0f), u);
    // Perlin's 2D output is bounded by sqrt(2)/2; scale to approximately [-1,1].
    return lerp(x1, x2, v) * 1.4142135f;
}

float Noise::fbm(float x, float y, int octaves, float lacunarity, float gain) const {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * perlin(x * freq, y * freq);
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return (norm > 0.0f) ? sum / norm : 0.0f;
}

float Noise::ridged(float x, float y, int octaves, float lacunarity, float gain) const {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        const float n = 1.0f - std::fabs(perlin(x * freq, y * freq));
        sum += amp * n * n;  // squaring sharpens the crests
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return (norm > 0.0f) ? sum / norm : 0.0f;
}

float Noise::billow(float x, float y, int octaves, float lacunarity, float gain) const {
    float sum = 0.0f, amp = 1.0f, freq = 1.0f, norm = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * std::fabs(perlin(x * freq, y * freq));
        norm += amp;
        amp *= gain;
        freq *= lacunarity;
    }
    return (norm > 0.0f) ? sum / norm : 0.0f;
}

float Noise::warpedFbm(float x, float y, int octaves, float lacunarity, float gain,
                       float warpAmount) const {
    // Two independent offsets keep the warp field from being diagonal.
    const float wx = fbm(x + 5.2f, y + 1.3f, 4, lacunarity, gain);
    const float wy = fbm(x - 3.7f, y + 9.1f, 4, lacunarity, gain);
    return fbm(x + warpAmount * wx, y + warpAmount * wy, octaves, lacunarity, gain);
}

}  // namespace gen
