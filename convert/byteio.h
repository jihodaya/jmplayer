// Small endian helpers shared by the format readers and writers.
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace jmpconv {

using Bytes = std::vector<uint8_t>;

inline uint16_t rdLE16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }

inline uint32_t rdLE32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

inline uint16_t rdBE16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }

inline uint32_t rdBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline void wrLE16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
}

inline void wrBE16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8); p[1] = uint8_t(v);
}

inline void wrLE32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
}

inline void pushBE16(Bytes& b, uint16_t v) {
    b.push_back(uint8_t(v >> 8)); b.push_back(uint8_t(v));
}

inline void pushBE32(Bytes& b, uint32_t v) {
    b.push_back(uint8_t(v >> 24)); b.push_back(uint8_t(v >> 16));
    b.push_back(uint8_t(v >> 8));  b.push_back(uint8_t(v));
}

// MIDI variable-length quantity.
inline void pushVarLen(Bytes& b, uint32_t v) {
    uint32_t buffer = v & 0x7F;
    while ((v >>= 7) != 0) {
        buffer <<= 8;
        buffer |= 0x80 | (v & 0x7F);
    }
    for (;;) {
        b.push_back(uint8_t(buffer & 0xFF));
        if (buffer & 0x80) buffer >>= 8; else break;
    }
}

inline uint32_t readVarLen(const uint8_t* d, size_t& p) {
    uint32_t v = 0;
    for (;;) {
        uint8_t b = d[p++];
        v = (v << 7) | (b & 0x7F);
        if (!(b & 0x80)) return v;
    }
}

bool readFile(const std::string& path, Bytes& out);
bool writeFile(const std::string& path, const Bytes& data);

}  // namespace jmpconv
