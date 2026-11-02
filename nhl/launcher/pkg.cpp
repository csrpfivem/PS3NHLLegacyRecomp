#include "pkg.h"

#include <bcrypt.h>

#include <algorithm>
#include <cstring>

namespace {

constexpr uint8_t kPkgKey[16] = { 0x2e, 0x7b, 0x71, 0xd7, 0xc9, 0xc9, 0xa1, 0x4e,
                                  0xa3, 0x22, 0x1f, 0x18, 0x88, 0x28, 0xb8, 0xf8 };

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

// Adds n to a big-endian 128-bit counter.
void ctr_add(uint8_t c[16], uint64_t n)
{
    for (int i = 15; i >= 0 && n; i--) {
        const uint64_t s = (uint64_t)c[i] + (n & 0xff);
        c[i] = (uint8_t)s;
        n = (n >> 8) + (s >> 8);
    }
}

}  // namespace

void PkgFile::close()
{
    if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
    entries_.clear();
}

bool PkgFile::read_raw(uint64_t at, void* buf, size_t n)
{
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)at;
    DWORD got = 0;
    return SetFilePointerEx(h_, pos, nullptr, FILE_BEGIN) && ReadFile(h_, buf, (DWORD)n, &got, nullptr) && got == n;
}

// Retail stream cipher: block i of the data area is XORed with
// AES-128-ECB(PKG_AES_KEY, header IV + i).
bool PkgFile::decrypt(uint64_t off, uint8_t* buf, size_t n)
{
    const size_t blocks = (n + 15) / 16;
    std::vector<uint8_t> ks(blocks * 16);
    uint8_t ctr[16];
    memcpy(ctr, iv_, 16);
    ctr_add(ctr, off / 16);
    for (size_t i = 0; i < blocks; i++) { memcpy(&ks[i * 16], ctr, 16); ctr_add(ctr, 1); }
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    ULONG out = 0;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0 &&
              BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB,
                                sizeof(BCRYPT_CHAIN_MODE_ECB), 0) == 0 &&
              BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)kPkgKey, 16, 0) == 0 &&
              BCryptEncrypt(k, ks.data(), (ULONG)ks.size(), nullptr, nullptr, 0, ks.data(), (ULONG)ks.size(), &out, 0) == 0;
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (ok) for (size_t i = 0; i < n; i++) buf[i] ^= ks[i];
    return ok;
}

bool PkgFile::open(const std::wstring& file)
{
    close();
    h_ = CreateFileW(file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) { err_ = "cannot open the file"; return false; }
    LARGE_INTEGER sz;
    GetFileSizeEx(h_, &sz);
    size_ = (uint64_t)sz.QuadPart;
    uint8_t h[0x80];
    if (size_ < sizeof h || !read_raw(0, h, sizeof h) || memcmp(h, "\x7FPKG", 4) != 0) {
        err_ = "not a PS3 package (.pkg)";
        close();
        return false;
    }
    const uint16_t release = (uint16_t)(h[4] << 8 | h[5]), platform = (uint16_t)(h[6] << 8 | h[7]);
    if (release != 0x8000 || platform != 1) {
        err_ = "not a retail PS3 package";
        close();
        return false;
    }
    const uint32_t count = be32(h + 0x14);
    data_off_ = be64(h + 0x20);
    data_size_ = be64(h + 0x28);
    content_id_.assign((const char*)h + 0x30, strnlen((const char*)h + 0x30, 0x30));
    memcpy(iv_, h + 0x70, 16);
    if (be64(h + 0x18) != size_ || data_off_ + data_size_ > size_ || count > 100000) {
        err_ = "the package is truncated or damaged";
        close();
        return false;
    }
    // File table: count x 32 bytes at the start of the data area.
    std::vector<uint8_t> tab((size_t)count * 32);
    if (!read_raw(data_off_, tab.data(), tab.size()) || !decrypt(0, tab.data(), tab.size())) {
        err_ = "package read error";
        close();
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t* e = &tab[(size_t)i * 32];
        PkgEntry pe;
        const uint32_t name_off = be32(e), name_size = be32(e + 4);
        pe.offset = be64(e + 8);
        pe.size = be64(e + 16);
        pe.type = be32(e + 24);
        if (name_size == 0 || name_size > 4096 || name_off + (uint64_t)name_size > data_size_ ||
            pe.offset + pe.size > data_size_) {
            err_ = "package file table is damaged (wrong key?)";
            close();
            return false;
        }
        const uint64_t a = name_off & ~15ull;
        std::vector<uint8_t> nb((size_t)((name_off + name_size + 15 - a) & ~15ull));
        if (!read_raw(data_off_ + a, nb.data(), nb.size()) || !decrypt(a, nb.data(), nb.size())) {
            err_ = "package read error";
            close();
            return false;
        }
        pe.name.assign((const char*)nb.data() + (name_off - a), name_size);
        pe.name.resize(strnlen(pe.name.c_str(), pe.name.size()));
        for (char& c : pe.name) if (c == '\\') c = '/';
        if (pe.name.find("..") != std::string::npos || pe.name.empty() || pe.name[0] == '/') {
            err_ = "package has an unsafe file name";
            close();
            return false;
        }
        entries_.push_back(std::move(pe));
    }
    return true;
}

const PkgEntry* PkgFile::find(const char* name) const
{
    for (const PkgEntry& e : entries_)
        if (_stricmp(e.name.c_str(), name) == 0) return &e;
    return nullptr;
}

size_t PkgFile::read(const PkgEntry& e, uint64_t off, void* buf, size_t len)
{
    if (off >= e.size) return 0;
    len = (size_t)std::min<uint64_t>(len, e.size - off);
    const uint64_t start = e.offset + off, a = start & ~15ull;
    std::vector<uint8_t> tmp((size_t)((start + len + 15 - a) & ~15ull));
    const size_t avail = (size_t)std::min<uint64_t>(tmp.size(), data_size_ - a);
    if (!read_raw(data_off_ + a, tmp.data(), avail) || !decrypt(a, tmp.data(), tmp.size())) return 0;
    memcpy(buf, tmp.data() + (start - a), len);
    return len;
}

bool PkgFile::read_all(const PkgEntry& e, std::vector<uint8_t>& out)
{
    out.resize((size_t)e.size);
    return read(e, 0, out.data(), out.size()) == out.size();
}

// The last 0x20 bytes are the SHA-1 of everything before them (plus padding).
bool PkgFile::verify_digest(const std::function<bool(double)>& progress)
{
    if (size_ < 0x60) return false;
    uint8_t want[20];
    if (!read_raw(size_ - 0x20, want, 20)) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) return false;
    bool ok = BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) == 0;
    std::vector<uint8_t> buf(4u << 20);
    for (uint64_t at = 0, end = size_ - 0x20; ok && at < end;) {
        const size_t n = (size_t)std::min<uint64_t>(buf.size(), end - at);
        ok = read_raw(at, buf.data(), n) && BCryptHashData(hh, buf.data(), (ULONG)n, 0) == 0;
        at += n;
        if (progress && !progress((double)at / (double)end)) ok = false;
    }
    uint8_t got[20];
    ok = ok && BCryptFinishHash(hh, got, 20, 0) == 0 && memcmp(got, want, 20) == 0;
    if (hh) BCryptDestroyHash(hh);
    BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}
