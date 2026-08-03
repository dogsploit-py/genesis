#include "core/serialize.h"

namespace gen {

uint32_t currentCompilerTag() {
#if defined(_MSC_VER)
    return 0x4D530000u | static_cast<uint32_t>(_MSC_VER & 0xFFFF);
#elif defined(__clang__)
    return 0x434C0000u | static_cast<uint32_t>((__clang_major__ << 8) | __clang_minor__);
#elif defined(__GNUC__)
    return 0x47430000u | static_cast<uint32_t>((__GNUC__ << 8) | __GNUC_MINOR__);
#else
    return 0x3F3F0000u;
#endif
}

const char* compilerTagName(uint32_t tag) {
    switch (tag >> 16) {
        case 0x4D53: return "MSVC";
        case 0x434C: return "Clang";
        case 0x4743: return "GCC";
        default:     return "unknown";
    }
}

// ---------------------------------------------------------------------------

BinaryWriter::BinaryWriter(const std::string& path) {
    m_file = std::fopen(path.c_str(), "wb");
}

BinaryWriter::BinaryWriter(std::vector<uint8_t>& buffer) : m_buffer(&buffer) {}

BinaryWriter::~BinaryWriter() { close(); }

void BinaryWriter::close() {
    if (m_inChunk) endChunk();
    if (m_file) { std::fclose(m_file); m_file = nullptr; }
}

// Written field by field rather than blitted. Blitting a struct writes its
// alignment padding, which is indeterminate -- two runs that computed exactly
// the same state would produce files that differ in the padding bytes, and a
// snapshot written by MSVC could not be trusted to read back under MinGW.
void BinaryWriter::writeHeader(const SnapshotHeader& h) {
    pod(h.magic0);
    pod(h.magic1);
    pod(h.version);
    pod(h.compilerTag);
    pod(h.worldSeed);
    pod(h.workerCount);
    pod(h.reserved);
    pod(h.tick);
}

void BinaryWriter::beginChunk(uint32_t tag) {
    // Chunk framing needs seek-back to patch the length, which a growing
    // buffer does not support. Memory blobs are bare records, not containers.
    if (!m_file) return;
    pod(tag);
    m_chunkLengthPos = std::ftell(m_file);
    const uint64_t placeholder = 0;
    pod(placeholder);
    m_chunkStart = std::ftell(m_file);
    m_inChunk = true;
}

void BinaryWriter::endChunk() {
    if (!m_file || !m_inChunk) return;
    const long end = std::ftell(m_file);
    const uint64_t length = static_cast<uint64_t>(end - m_chunkStart);
    std::fseek(m_file, m_chunkLengthPos, SEEK_SET);
    std::fwrite(&length, sizeof(length), 1, m_file);
    std::fseek(m_file, end, SEEK_SET);
    m_inChunk = false;
}

void BinaryWriter::raw(const void* data, size_t bytes) {
    if (bytes == 0) return;
    if (m_buffer) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        m_buffer->insert(m_buffer->end(), p, p + bytes);
        return;
    }
    if (!m_file) return;
    std::fwrite(data, 1, bytes, m_file);
}

void BinaryWriter::str(const std::string& s) {
    const uint32_t n = static_cast<uint32_t>(s.size());
    pod(n);
    if (n) raw(s.data(), n);
}

// ---------------------------------------------------------------------------

BinaryReader::BinaryReader(const std::string& path) {
    m_file = std::fopen(path.c_str(), "rb");
    if (!m_file) { m_failed = true; m_error = "cannot open " + path; }
}

BinaryReader::BinaryReader(const uint8_t* data, size_t size)
    : m_data(data), m_size(size) {
    if (!data) { m_failed = true; m_error = "null buffer"; }
}

BinaryReader::~BinaryReader() { close(); }

void BinaryReader::close() {
    if (m_file) { std::fclose(m_file); m_file = nullptr; }
}

bool BinaryReader::readHeader(SnapshotHeader& h) {
    if (!m_file) return false;
    pod(h.magic0);
    pod(h.magic1);
    pod(h.version);
    pod(h.compilerTag);
    pod(h.worldSeed);
    pod(h.workerCount);
    pod(h.reserved);
    pod(h.tick);
    if (m_failed) return false;
    if (h.magic0 != kSnapshotMagic0 || h.magic1 != kSnapshotMagic1) {
        m_failed = true;
        m_error = "not a GENESIS snapshot (bad magic)";
        return false;
    }
    if (h.version > kSnapshotVersion) {
        m_failed = true;
        m_error = "snapshot format version " + std::to_string(h.version) +
                  " is newer than this build supports (" +
                  std::to_string(kSnapshotVersion) + ")";
        return false;
    }
    return true;
}

bool BinaryReader::nextChunk(uint32_t& tag, uint64_t& length) {
    if (!m_file || m_failed) return false;
    // If the previous chunk was only partially consumed, jump to its end so a
    // reader that stops early cannot desynchronise the stream.
    if (m_chunkEnd > 0 && std::ftell(m_file) != m_chunkEnd)
        std::fseek(m_file, m_chunkEnd, SEEK_SET);

    if (std::fread(&tag, sizeof(tag), 1, m_file) != 1) return false;
    if (std::fread(&length, sizeof(length), 1, m_file) != 1) return false;
    m_chunkEnd = std::ftell(m_file) + static_cast<long>(length);
    return true;
}

void BinaryReader::skipChunk() {
    if (m_file && m_chunkEnd > 0) std::fseek(m_file, m_chunkEnd, SEEK_SET);
}

void BinaryReader::raw(void* data, size_t bytes) {
    if (m_failed || bytes == 0) return;
    if (m_data) {
        if (m_pos + bytes > m_size) {
            m_failed = true;
            if (m_error.empty()) m_error = "unexpected end of buffer";
            return;
        }
        std::memcpy(data, m_data + m_pos, bytes);
        m_pos += bytes;
        return;
    }
    if (!m_file) return;
    if (std::fread(data, 1, bytes, m_file) != bytes) {
        m_failed = true;
        if (m_error.empty()) m_error = "unexpected end of file";
    }
}

void BinaryReader::str(std::string& s) {
    uint32_t n = 0;
    pod(n);
    if (m_failed || n > (1u << 28)) { m_failed = true; s.clear(); return; }
    s.assign(n, '\0');
    if (n) raw(&s[0], n);
}

}  // namespace gen
