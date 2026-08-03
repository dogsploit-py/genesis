// core/spatial_hash.h — uniform-grid neighbour index.
//
// Rebuilt from scratch every tick by counting sort: count pass, prefix sum,
// scatter pass. All O(n), all allocation-free after the first build (the two
// arrays are sized once and reused), and deterministic because entities are
// scattered in ascending slot order, so a cell's contents are always listed in
// the same order regardless of thread timing.
//
// This is the structure that makes 10k agents' neighbour queries affordable:
// a radius query touches ceil(2r/cell)^2 cells instead of all n entities.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace gen {

class SpatialHash {
public:
    // cellSize should be about the largest neighbour query radius: smaller
    // means more cells to visit per query, larger means more candidates to
    // reject inside each cell.
    void configure(float worldWidth, float worldHeight, float cellSize) {
        m_cellSize = (cellSize > 0.0001f) ? cellSize : 1.0f;
        m_invCell = 1.0f / m_cellSize;
        m_cols = static_cast<int>(std::ceil(worldWidth * m_invCell));
        m_rows = static_cast<int>(std::ceil(worldHeight * m_invCell));
        if (m_cols < 1) m_cols = 1;
        if (m_rows < 1) m_rows = 1;
        m_cellStart.assign(static_cast<size_t>(m_cols) * m_rows + 1, 0);
    }

    // xs/ys are parallel arrays of positions in world units; `count` entries.
    // Entity i is identified by its index i, which is also its stable slot.
    void build(const float* xs, const float* ys, size_t count) {
        const size_t cells = static_cast<size_t>(m_cols) * m_rows;
        if (m_cellStart.size() != cells + 1) m_cellStart.assign(cells + 1, 0);
        else std::fill(m_cellStart.begin(), m_cellStart.end(), 0u);

        m_entries.resize(count);
        if (m_cellOf.size() < count) m_cellOf.resize(count);

        // Pass 1: count entities per cell.
        for (size_t i = 0; i < count; ++i) {
            const uint32_t c = cellIndex(xs[i], ys[i]);
            m_cellOf[i] = c;
            ++m_cellStart[c + 1];
        }
        // Pass 2: prefix sum turns counts into start offsets.
        for (size_t c = 0; c < cells; ++c) m_cellStart[c + 1] += m_cellStart[c];
        // Pass 3: scatter. Ascending i means cell contents are in slot order.
        std::vector<uint32_t> cursor(m_cellStart.begin(), m_cellStart.end() - 1);
        for (size_t i = 0; i < count; ++i)
            m_entries[cursor[m_cellOf[i]]++] = static_cast<uint32_t>(i);
    }

    // Calls fn(entityIndex) for every entity in a cell block covering the
    // circle (cx, cy, radius). Candidates outside the radius are included --
    // the caller does the exact distance test, which avoids a sqrt here.
    template <typename Fn>
    void query(float cx, float cy, float radius, Fn&& fn) const {
        int x0 = static_cast<int>(std::floor((cx - radius) * m_invCell));
        int x1 = static_cast<int>(std::floor((cx + radius) * m_invCell));
        int y0 = static_cast<int>(std::floor((cy - radius) * m_invCell));
        int y1 = static_cast<int>(std::floor((cy + radius) * m_invCell));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 >= m_cols) x1 = m_cols - 1;
        if (y1 >= m_rows) y1 = m_rows - 1;

        for (int y = y0; y <= y1; ++y) {
            const size_t rowBase = static_cast<size_t>(y) * m_cols;
            for (int x = x0; x <= x1; ++x) {
                const size_t c = rowBase + static_cast<size_t>(x);
                const uint32_t b = m_cellStart[c];
                const uint32_t e = m_cellStart[c + 1];
                for (uint32_t k = b; k < e; ++k) fn(m_entries[k]);
            }
        }
    }

    int   cols() const { return m_cols; }
    int   rows() const { return m_rows; }
    float cellSize() const { return m_cellSize; }
    size_t entryCount() const { return m_entries.size(); }

private:
    uint32_t cellIndex(float x, float y) const {
        int cx = static_cast<int>(x * m_invCell);
        int cy = static_cast<int>(y * m_invCell);
        if (cx < 0) cx = 0;
        if (cy < 0) cy = 0;
        if (cx >= m_cols) cx = m_cols - 1;
        if (cy >= m_rows) cy = m_rows - 1;
        return static_cast<uint32_t>(static_cast<size_t>(cy) * m_cols + cx);
    }

    std::vector<uint32_t> m_cellStart;  // size cells+1, prefix-summed offsets
    std::vector<uint32_t> m_entries;    // entity indices grouped by cell
    std::vector<uint32_t> m_cellOf;     // scratch: cell of entity i
    float m_cellSize = 1.0f;
    float m_invCell = 1.0f;
    int   m_cols = 1;
    int   m_rows = 1;
};

}  // namespace gen
