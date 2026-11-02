// Dev tool: test_pkg <file.pkg> [out_dir]
// Lists entries (type, size, name), checks the package digest, and with out_dir
// extracts every file and prints its SHA-1.
#include "pkg.h"   // windows.h first: bcrypt.h needs its types

#include <bcrypt.h>
#include <cstdio>
#include <filesystem>
#include "sfo.h"

static std::string sha1(const std::vector<uint8_t>& d)
{
    BCRYPT_ALG_HANDLE a;
    BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA1_ALGORITHM, nullptr, 0);
    uint8_t h[20];
    BCryptHash(a, nullptr, 0, (PUCHAR)d.data(), (ULONG)d.size(), h, 20);
    BCryptCloseAlgorithmProvider(a, 0);
    char s[41];
    for (int i = 0; i < 20; i++) sprintf(s + i * 2, "%02X", h[i]);
    return s;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { printf("usage: test_pkg <file.pkg> [out_dir]\n"); return 2; }
    PkgFile pkg;
    if (!pkg.open(argv[1])) { printf("open failed: %s\n", pkg.error().c_str()); return 1; }
    printf("content id %s (title %s), %zu entries\n", pkg.content_id().c_str(), pkg.title_id().c_str(), pkg.entries().size());
    printf("digest: %s\n", pkg.verify_digest(nullptr) ? "OK" : "MISMATCH");
    if (const PkgEntry* s = pkg.find("PARAM.SFO")) {
        std::vector<uint8_t> d;
        pkg.read_all(*s, d);
        auto kv = sfo_parse(d);
        printf("PARAM.SFO: TITLE_ID=%s APP_VER=%s TITLE=%s\n", kv["TITLE_ID"].c_str(), kv["APP_VER"].c_str(), kv["TITLE"].c_str());
    }
    for (const PkgEntry& e : pkg.entries()) {
        std::string h;
        if (argc >= 3 && !e.folder()) {
            std::vector<uint8_t> d;
            if (!pkg.read_all(e, d)) { printf("read failed: %s\n", e.name.c_str()); return 1; }
            h = sha1(d);
            std::filesystem::path out = std::filesystem::path(argv[2]) / std::filesystem::u8path(e.name);
            std::filesystem::create_directories(out.parent_path());
            FILE* f = _wfopen(out.c_str(), L"wb");
            fwrite(d.data(), 1, d.size(), f);
            fclose(f);
        }
        printf("%08X %12llu %s %s\n", e.type, (unsigned long long)e.size, h.c_str(), e.name.c_str());
    }
    return 0;
}
