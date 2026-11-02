// PARAM.SFO reader (string and integer entries).
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

inline std::map<std::string, std::string> sfo_parse(const std::vector<uint8_t>& d)
{
    std::map<std::string, std::string> out;
    if (d.size() < 20 || memcmp(d.data(), "\0PSF", 4) != 0) return out;
    auto u16 = [&](size_t o) { return (uint32_t)(d[o] | d[o + 1] << 8); };
    auto u32 = [&](size_t o) { return (uint32_t)(d[o] | d[o + 1] << 8 | d[o + 2] << 16 | (uint32_t)d[o + 3] << 24); };
    const uint32_t keys = u32(8), data = u32(12), n = u32(16);
    for (uint32_t i = 0; i < n && 20 + i * 16 + 16 <= d.size(); i++) {
        const size_t e = 20 + i * 16;
        const uint32_t ko = keys + u16(e), fmt = u16(e + 2), len = u32(e + 4), dof = data + u32(e + 12);
        if (ko >= d.size() || dof + len > d.size()) continue;
        std::string key((const char*)d.data() + ko, strnlen((const char*)d.data() + ko, d.size() - ko));
        if (fmt == 0x0404 && len >= 4) out[key] = std::to_string(u32(dof));
        else out[key] = std::string((const char*)d.data() + dof, strnlen((const char*)d.data() + dof, len));
    }
    return out;
}
