#include "iso9660.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr uint32_t kSector = 2048;

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

std::string joliet_name(const uint8_t* p, int n)
{
    std::wstring w;
    for (int i = 0; i + 1 < n; i += 2) w += (wchar_t)(p[i] << 8 | p[i + 1]);
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), len, nullptr, nullptr);
    return s;
}
}  // namespace

void IsoImage::close()
{
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
    files_.clear();
}

bool IsoImage::read_sectors(uint32_t lba, uint32_t count, void* buf)
{
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)lba * kSector;
    DWORD got = 0;
    return SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) &&
           ReadFile(h_, buf, count * kSector, &got, nullptr) && got == count * kSector;
}

bool IsoImage::open(const std::wstring& file)
{
    close();
    h_ = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                     FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) { err_ = "cannot open the file"; return false; }

    // Volume descriptors from sector 16: 1 = primary, 2 = supplementary (Joliet
    // when its escape sequence is %/@, %/C or %/E), 255 = terminator.
    uint8_t vd[kSector];
    uint8_t root_pvd[34] = {}, root_jol[34] = {};
    bool have_pvd = false, have_jol = false;
    for (uint32_t s = 16; s < 64; s++) {
        if (!read_sectors(s, 1, vd) || memcmp(vd + 1, "CD001", 5) != 0) break;
        if (vd[0] == 1 && !have_pvd) { memcpy(root_pvd, vd + 156, 34); have_pvd = true; }
        if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && (vd[90] == '@' || vd[90] == 'C' || vd[90] == 'E')) {
            memcpy(root_jol, vd + 156, 34); have_jol = true;
        }
        if (vd[0] == 255) break;
    }
    if (!have_pvd) { err_ = "not an ISO 9660 disc image"; close(); return false; }
    const uint8_t* root = have_jol ? root_jol : root_pvd;
    if (!walk(le32(root + 2), le32(root + 10), "", have_jol, 0)) { close(); return false; }
    if (files_.empty()) { err_ = "the disc image has no files"; close(); return false; }
    return true;
}

bool IsoImage::walk(uint32_t lba, uint32_t size, const std::string& prefix, bool joliet, int depth)
{
    if (depth > 32) { err_ = "directory tree too deep"; return false; }
    const uint32_t nsec = (size + kSector - 1) / kSector;
    std::vector<uint8_t> dir((size_t)nsec * kSector);
    if (!read_sectors(lba, nsec, dir.data())) { err_ = "read error in a directory"; return false; }

    std::string last_name;   // multi-extent files repeat the record per extent
    for (uint32_t off = 0; off < size;) {
        const uint8_t* r = dir.data() + off;
        if (r[0] == 0) { off = (off / kSector + 1) * kSector; continue; }   // records never cross sectors
        if (off + r[0] > dir.size() || r[0] < 34) { err_ = "corrupt directory record"; return false; }
        const uint8_t flags = r[25], nlen = r[32];
        const uint8_t* np = r + 33;
        off += r[0];
        if (nlen == 1 && (np[0] == 0 || np[0] == 1)) continue;   // . and ..
        std::string name = joliet ? joliet_name(np, nlen) : std::string((const char*)np, nlen);
        if (size_t semi = name.find(';'); semi != std::string::npos) name.resize(semi);
        if (!name.empty() && name.back() == '.') name.pop_back();
        const std::string path = prefix.empty() ? name : prefix + "/" + name;
        if (flags & 2) {
            if (!walk(le32(r + 2), le32(r + 10), path, joliet, depth + 1)) return false;
            last_name.clear();
            continue;
        }
        if (!files_.empty() && path == last_name) {
            files_.back().extents.push_back({ le32(r + 2), le32(r + 10) });
            files_.back().size += le32(r + 10);
        } else {
            IsoFile f;
            f.path = path;
            f.size = le32(r + 10);
            f.extents.push_back({ le32(r + 2), le32(r + 10) });
            files_.push_back(std::move(f));
        }
        last_name = (flags & 0x80) ? path : std::string();
    }
    return true;
}

const IsoFile* IsoImage::find(const char* path) const
{
    for (const IsoFile& f : files_)
        if (_stricmp(f.path.c_str(), path) == 0) return &f;
    return nullptr;
}

size_t IsoImage::read(const IsoFile& f, uint64_t off, void* buf, size_t n)
{
    size_t done = 0;
    uint64_t base = 0;
    for (const auto& [lba, bytes] : f.extents) {
        if (done == n) break;
        if (off < base + bytes) {
            const uint64_t in = off - base;
            const size_t take = (size_t)std::min<uint64_t>(bytes - in, n - done);
            LARGE_INTEGER pos;
            pos.QuadPart = (LONGLONG)lba * kSector + (LONGLONG)in;
            DWORD got = 0;
            if (!SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) ||
                !ReadFile(h_, (uint8_t*)buf + done, (DWORD)take, &got, nullptr))
                return done;
            done += got;
            off += got;
            if (got != take) return done;
        }
        base += bytes;
    }
    return done;
}

bool IsoImage::read_all(const IsoFile& f, std::vector<uint8_t>& out)
{
    out.resize((size_t)f.size);
    return read(f, 0, out.data(), out.size()) == out.size();
}

uint64_t IsoImage::total_bytes() const
{
    uint64_t t = 0;
    for (const IsoFile& f : files_) t += f.size;
    return t;
}
