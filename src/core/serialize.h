// core/serialize.h — chunked binary snapshot format.
//
// Layout:
//   Header  : magic "GENESIS\x1a", formatVersion, compilerTag, worldSeed,
//             workerCount, tickCount
//   Chunks  : [4-byte tag][8-byte payload length][payload bytes] ...
//
// Unknown chunks are skipped by length. That is what lets an M2 save load into
// an M6 build (the missing subsystems stay at defaults) and an M6 save load
// into an M2 build (the chemistry chunk is skipped). It is also why a moneyless
// world simply has no ECON chunk rather than an empty one.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace gen {

constexpr uint32_t kSnapshotMagic0 = 0x454E4547u;  // "GENE"
constexpr uint32_t kSnapshotMagic1 = 0x1A534953u;  // "SIS\x1a"
constexpr uint32_t kSnapshotVersion = 3;   // 2 adds inventory/knowledge, 3 adds species and economy

// Four-character chunk tags.
constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<uint32_t>(static_cast<uint8_t>(a))
         | (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8)
         | (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16)
         | (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}

constexpr uint32_t kChunkWorld   = fourcc('W', 'R', 'L', 'D');
constexpr uint32_t kChunkTiles   = fourcc('T', 'I', 'L', 'E');
constexpr uint32_t kChunkRng     = fourcc('R', 'N', 'G', 'S');
constexpr uint32_t kChunkTime    = fourcc('T', 'I', 'M', 'E');
constexpr uint32_t kChunkEvents  = fourcc('E', 'V', 'N', 'T');
constexpr uint32_t kChunkTelem   = fourcc('T', 'L', 'M', 'Y');
constexpr uint32_t kChunkIntervn = fourcc('I', 'L', 'O', 'G');
constexpr uint32_t kChunkAgents  = fourcc('A', 'G', 'N', 'T');  // M2
constexpr uint32_t kChunkGenomes = fourcc('G', 'E', 'N', 'E');  // M2
constexpr uint32_t kChunkBrains  = fourcc('B', 'R', 'A', 'N');  // M3
constexpr uint32_t kChunkPedigree= fourcc('P', 'E', 'D', 'G');  // M2
constexpr uint32_t kChunkKnow    = fourcc('K', 'N', 'O', 'W');  // M6
constexpr uint32_t kChunkChem    = fourcc('C', 'H', 'E', 'M');  // M6
constexpr uint32_t kChunkGod     = fourcc('G', 'O', 'D', 'S');  // M5
constexpr uint32_t kChunkEcon    = fourcc('E', 'C', 'O', 'N');  // M8, optional

struct SnapshotHeader {
    uint32_t magic0 = kSnapshotMagic0;
    uint32_t magic1 = kSnapshotMagic1;
    uint32_t version = kSnapshotVersion;
    uint32_t compilerTag = 0;   // identifies libm, which bounds bit-exactness
    uint64_t worldSeed = 0;
    uint32_t workerCount = 0;   // restored on load: chunk split affects float sums
    uint32_t reserved = 0;
    uint64_t tick = 0;
};

// Identifies the toolchain so a bit-exactness mismatch is reported rather than
// silently diverging through a different libm (see ARCHITECTURE.md §5.3).
uint32_t currentCompilerTag();
const char* compilerTagName(uint32_t tag);

class BinaryWriter {
public:
    explicit BinaryWriter(const std::string& path);
    // In-memory mode. Used by god-mode undo, which needs to serialise a single
    // agent into a byte buffer rather than a file. Chunk framing is not
    // available in this mode -- a blob is a bare record, not a container.
    explicit BinaryWriter(std::vector<uint8_t>& buffer);
    ~BinaryWriter();

    BinaryWriter(const BinaryWriter&) = delete;
    BinaryWriter& operator=(const BinaryWriter&) = delete;

    bool ok() const { return m_file != nullptr || m_buffer != nullptr; }
    void close();

    void writeHeader(const SnapshotHeader& h);

    // Begin a chunk, write its payload, then end it. The length is patched in
    // on endChunk, so payload size does not need to be known up front.
    void beginChunk(uint32_t tag);
    void endChunk();

    void raw(const void* data, size_t bytes);

    template <typename T>
    void pod(const T& v) {
        static_assert(sizeof(T) > 0, "incomplete type");
        raw(&v, sizeof(T));
    }

    // One fwrite per array: tile fields are contiguous by design.
    template <typename T>
    void array(const std::vector<T>& v) {
        const uint64_t n = v.size();
        pod(n);
        if (n) raw(v.data(), sizeof(T) * static_cast<size_t>(n));
    }

    void str(const std::string& s);

private:
    FILE*  m_file = nullptr;
    std::vector<uint8_t>* m_buffer = nullptr;
    long   m_chunkLengthPos = 0;
    long   m_chunkStart = 0;
    bool   m_inChunk = false;
};

class BinaryReader {
public:
    explicit BinaryReader(const std::string& path);
    // In-memory mode, the counterpart of the buffer writer.
    BinaryReader(const uint8_t* data, size_t size);
    ~BinaryReader();

    // Bytes consumed so far in memory mode. God-mode undo uses this to walk a
    // buffer holding several agent blobs back to back.
    size_t position() const { return m_pos; }

    BinaryReader(const BinaryReader&) = delete;
    BinaryReader& operator=(const BinaryReader&) = delete;

    bool ok() const { return (m_file != nullptr || m_data != nullptr) && !m_failed; }
    void close();

    bool readHeader(SnapshotHeader& h);

    // Returns false at end of file. On success the payload is positioned for
    // reading and `length` bytes long; call skipChunk() to ignore it.
    bool nextChunk(uint32_t& tag, uint64_t& length);
    void skipChunk();

    void raw(void* data, size_t bytes);

    template <typename T>
    void pod(T& v) { raw(&v, sizeof(T)); }

    template <typename T>
    void array(std::vector<T>& v) {
        uint64_t n = 0;
        pod(n);
        // Guard against a corrupt length asking for a terabyte allocation.
        if (m_failed || n > (1ull << 34)) { m_failed = true; v.clear(); return; }
        v.resize(static_cast<size_t>(n));
        if (n) raw(v.data(), sizeof(T) * static_cast<size_t>(n));
    }

    void str(std::string& s);

    const std::string& error() const { return m_error; }

private:
    FILE*          m_file = nullptr;
    const uint8_t* m_data = nullptr;
    size_t         m_size = 0;
    size_t         m_pos = 0;
    bool           m_failed = false;
    std::string    m_error;
    long           m_chunkEnd = 0;
};

}  // namespace gen
