// Read-only ISO 9660 reader for decrypted PS3 disc images (Joliet names when the
// image has them, multi-extent files). Used by the installer to extract the disc.
#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct IsoFile {
    std::string path;                                  // "PS3_GAME/USRDIR/EBOOT.BIN"
    uint64_t size = 0;
    std::vector<std::pair<uint32_t, uint32_t>> extents;  // (lba, bytes)
};

class IsoImage {
public:
    ~IsoImage() { close(); }
    bool open(const std::wstring& file);   // false -> error()
    void close();
    const std::vector<IsoFile>& files() const { return files_; }
    const IsoFile* find(const char* path) const;   // case-insensitive, '/' separators
    // Reads n bytes at offset off of the file. Returns bytes read.
    size_t read(const IsoFile& f, uint64_t off, void* buf, size_t n);
    bool read_all(const IsoFile& f, std::vector<uint8_t>& out);
    const std::string& error() const { return err_; }
    uint64_t total_bytes() const;

private:
    bool read_sectors(uint32_t lba, uint32_t count, void* buf);
    bool walk(uint32_t lba, uint32_t size, const std::string& prefix, bool joliet, int depth);

    HANDLE h_ = INVALID_HANDLE_VALUE;
    std::vector<IsoFile> files_;
    std::string err_;
};
