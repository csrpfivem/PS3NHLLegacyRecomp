#include "self_decrypt.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstring>

#include "miniz.h"

namespace {

// NPDRM SELF key, revision 0x1C (firmware 4.46 era).
constexpr uint16_t kNpdrmRevision = 0x001C;
constexpr uint8_t kNpdrmErk[32] = {
    0x81, 0x03, 0xEA, 0x9D, 0xB7, 0x90, 0x57, 0x82, 0x19, 0xC4, 0xCE, 0xDF, 0x05, 0x92, 0xB4, 0x30,
    0x64, 0xA7, 0xD9, 0x8B, 0x60, 0x1B, 0x6C, 0x7B, 0xC4, 0x51, 0x08, 0xC4, 0x04, 0x7A, 0xA8, 0x0F };
constexpr uint8_t kNpdrmRiv[16] = {
    0x24, 0x6F, 0x4B, 0x83, 0x28, 0xBE, 0x6A, 0x2D, 0x39, 0x4E, 0xDE, 0x20, 0x47, 0x92, 0x47, 0xC5 };
// Free-license klicensee and the key it is wrapped with.
constexpr uint8_t kKlicFree[16] = {
    0x72, 0xF9, 0x90, 0x78, 0x8F, 0x9C, 0xFF, 0x74, 0x57, 0x25, 0xF0, 0x8E, 0x4C, 0x12, 0x83, 0x87 };
constexpr uint8_t kKlicKey[16] = {
    0xF2, 0xFB, 0xCA, 0x7A, 0x75, 0xB0, 0x4E, 0xDC, 0x13, 0x90, 0x63, 0x8C, 0xCD, 0xFD, 0xD1, 0xEE };

uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }

// AES via CNG. mode: BCRYPT_CHAIN_MODE_ECB / _CBC. Decrypts or encrypts in place
// (length a multiple of 16).
bool aes(const uint8_t* key, ULONG key_len, const wchar_t* mode, const uint8_t* iv, uint8_t* data, size_t n, bool enc)
{
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0 &&
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)mode, (ULONG)(wcslen(mode) + 1) * 2, 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key, key_len, 0) == 0) {
        uint8_t ivc[16] = {};
        if (iv) memcpy(ivc, iv, 16);
        ULONG out = 0;
        ok = (enc ? BCryptEncrypt(k, data, (ULONG)n, nullptr, iv ? ivc : nullptr, iv ? 16 : 0, data, (ULONG)n, &out, 0)
                  : BCryptDecrypt(k, data, (ULONG)n, nullptr, iv ? ivc : nullptr, iv ? 16 : 0, data, (ULONG)n, &out, 0)) == 0;
    }
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// AES-128-CTR (big-endian 128-bit counter starting at iv), in place.
bool aes_ctr(const uint8_t key[16], const uint8_t iv[16], uint8_t* data, size_t n)
{
    std::vector<uint8_t> ks((n + 15) & ~size_t(15));
    uint8_t ctr[16];
    memcpy(ctr, iv, 16);
    for (size_t off = 0; off < ks.size(); off += 16) {
        memcpy(&ks[off], ctr, 16);
        for (int i = 15; i >= 0 && ++ctr[i] == 0; i--) {}
    }
    if (!aes(key, 16, BCRYPT_CHAIN_MODE_ECB, nullptr, ks.data(), ks.size(), true)) return false;
    for (size_t i = 0; i < n; i++) data[i] ^= ks[i];
    return true;
}

}  // namespace

std::string self_to_elf(const std::vector<uint8_t>& s, std::vector<uint8_t>& elf)
{
    // SCE header (0x20) + extended header (0x50), all big-endian.
    if (s.size() < 0x70 || memcmp(s.data(), "SCE\0", 4) != 0) return "not a SELF file";
    const uint16_t revision = be16(&s[8]);
    const uint32_t meta_off = be32(&s[12]);
    const uint64_t hsize = be64(&s[16]);
    const uint64_t progid_off = be64(&s[0x28]), ehdr_off = be64(&s[0x30]), phdr_off = be64(&s[0x38]),
                   shdr_off = be64(&s[0x40]), supp_off = be64(&s[0x58]), supp_size = be64(&s[0x60]);
    if (hsize > s.size() || progid_off + 0x20 > s.size() || ehdr_off + 0x40 > s.size())
        return "truncated SELF header";
    if (be32(&s[progid_off + 12]) != 8) return "not an NPDRM SELF";
    if (revision != kNpdrmRevision) return "unsupported SELF key revision";

    // NPDRM supplemental header (type 3): license 3 = free (klicensee = kKlicFree).
    bool free_license = false;
    for (uint64_t off = supp_off; off + 16 <= supp_off + supp_size && off + 16 <= s.size();) {
        const uint32_t type = be32(&s[off]), size = be32(&s[off + 4]);
        if (type == 3 && off + 0x20 <= s.size()) free_license = be32(&s[off + 0x18]) == 3;
        if (!size) break;
        off += size;
    }
    if (!free_license) return "the EBOOT needs a license file (not a free-license NPDRM SELF)";

    // Metadata info (0x40): NPDRM layer (AES-128-CBC, key = ECB-unwrapped klicensee,
    // IV 0), then AES-256-CBC with the SELF key.
    const size_t mi = (size_t)meta_off + 0x20;
    if (mi + 0x40 > hsize) return "bad metadata offset";
    uint8_t info[0x40], klic[16], zero_iv[16] = {};
    memcpy(info, &s[mi], 0x40);
    memcpy(klic, kKlicFree, 16);
    if (!aes(kKlicKey, 16, BCRYPT_CHAIN_MODE_ECB, nullptr, klic, 16, false) ||
        !aes(klic, 16, BCRYPT_CHAIN_MODE_CBC, zero_iv, info, 0x40, false) ||
        !aes(kNpdrmErk, 32, BCRYPT_CHAIN_MODE_CBC, kNpdrmRiv, info, 0x40, false))
        return "AES failure";
    if (info[0x10] != 0 || info[0x30] != 0) return "metadata did not decrypt (wrong key)";

    // Metadata header + section headers + data keys: AES-128-CTR with info's key/iv.
    std::vector<uint8_t> meta(s.begin() + mi + 0x40, s.begin() + (size_t)hsize);
    if (meta.size() < 0x20 || !aes_ctr(info, info + 0x20, meta.data(), meta.size())) return "metadata decrypt failed";
    const uint32_t nsec = be32(&meta[12]), nkeys = be32(&meta[16]);
    const size_t keys_at = 0x20 + (size_t)nsec * 0x30;
    if (keys_at + (size_t)nkeys * 16 > meta.size()) return "bad metadata header";

    // ELF: header, program headers and section headers are stored in the clear.
    const uint8_t* eh = &s[ehdr_off];
    if (memcmp(eh, "\x7F" "ELF", 4) != 0 || eh[4] != 2) return "embedded header is not ELF64";
    const uint64_t e_phoff = be64(eh + 0x20), e_shoff = be64(eh + 0x28);
    const uint16_t phnum = be16(eh + 0x38), shnum = be16(eh + 0x3C);
    if (phdr_off + phnum * 0x38ull > s.size() || (shdr_off && shdr_off + shnum * 0x40ull > s.size()))
        return "truncated ELF headers";

    std::vector<uint8_t> out;
    auto put = [&out](uint64_t at, const uint8_t* p, size_t n) {
        if (out.size() < at + n) out.resize((size_t)(at + n));
        memcpy(&out[(size_t)at], p, n);
    };
    put(0, eh, 0x40);
    std::vector<uint8_t> buf, plain;
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t* h = &meta[0x20 + (size_t)i * 0x30];
        const uint64_t data_off = be64(h), data_size = be64(h + 8);
        const uint32_t type = be32(h + 16), idx = be32(h + 20), enc = be32(h + 32), key_i = be32(h + 36),
                       iv_i = be32(h + 40), comp = be32(h + 44);
        if (type != 2) continue;   // program segments only
        if (idx >= phnum || data_off + data_size > s.size()) return "bad section header";
        buf.assign(s.begin() + (size_t)data_off, s.begin() + (size_t)(data_off + data_size));
        if (enc == 3) {
            if (key_i >= nkeys || iv_i >= nkeys) return "bad section key index";
            if (!aes_ctr(&meta[keys_at + key_i * 16], &meta[keys_at + iv_i * 16], buf.data(), buf.size()))
                return "segment decrypt failed";
        }
        const uint8_t* ph = &s[phdr_off + idx * 0x38ull];
        const uint64_t p_offset = be64(ph + 8), p_filesz = be64(ph + 0x20);
        if (comp == 2) {
            plain.resize((size_t)p_filesz);
            mz_ulong n = (mz_ulong)p_filesz;
            const int rc = mz_uncompress(plain.data(), &n, buf.data(), (mz_ulong)buf.size());
            if (rc != MZ_OK && rc != MZ_BUF_ERROR) return "segment " + std::to_string(idx) + " inflate failed";
            put(p_offset, plain.data(), n);
        } else {
            put(p_offset, buf.data(), buf.size());
        }
    }
    if (shdr_off) put(e_shoff, &s[shdr_off], shnum * 0x40ull);
    put(e_phoff, &s[phdr_off], phnum * 0x38ull);   // last, as segment data may overlap
    elf.swap(out);
    return "";
}
