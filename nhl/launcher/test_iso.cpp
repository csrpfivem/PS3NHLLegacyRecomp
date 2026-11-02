// Dev tool: test_iso <image.iso> [file-to-dump]
// Lists every file (path, size, extents) and the PARAM.SFO title id; with a
// second argument writes that file's bytes to stdout-named <file>.out.
#include <cstdio>

#include "iso9660.h"
#include "sfo.h"

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2) { printf("usage: test_iso <image.iso> [path-in-iso]\n"); return 2; }
    IsoImage iso;
    if (!iso.open(argv[1])) { printf("open failed: %s\n", iso.error().c_str()); return 1; }
    for (const IsoFile& f : iso.files())
        printf("%12llu  %zu ext  %s\n", (unsigned long long)f.size, f.extents.size(), f.path.c_str());
    printf("%zu files, %llu bytes\n", iso.files().size(), (unsigned long long)iso.total_bytes());
    if (const IsoFile* s = iso.find("PS3_GAME/PARAM.SFO")) {
        std::vector<uint8_t> d;
        iso.read_all(*s, d);
        auto kv = sfo_parse(d);
        printf("TITLE_ID=%s TITLE=%s VERSION=%s\n", kv["TITLE_ID"].c_str(), kv["TITLE"].c_str(), kv["VERSION"].c_str());
    }
    if (argc >= 3) {
        char path[1024];
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, path, sizeof path, nullptr, nullptr);
        const IsoFile* f = iso.find(path);
        if (!f) { printf("not found: %s\n", path); return 1; }
        std::vector<uint8_t> d;
        if (!iso.read_all(*f, d)) { printf("read failed\n"); return 1; }
        FILE* o = _wfopen(L"dump.out", L"wb");
        fwrite(d.data(), 1, d.size(), o);
        fclose(o);
        printf("dumped %zu bytes to dump.out\n", d.size());
    }
    return 0;
}
