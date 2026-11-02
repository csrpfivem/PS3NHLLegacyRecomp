// PS3 retail .pkg reader (the game update package from Sony). Written from the
// format as documented by RPCS3's unpkg; PKG_AES_KEY is the console's public
// package key. Files come out as stored: EBOOT.BIN stays an NPDRM SELF.
#pragma once
#include <windows.h>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct PkgEntry {
    std::string name;     // relative path inside the title dir, '/' separators
    uint64_t offset = 0;  // from the start of the encrypted data area
    uint64_t size = 0;
    uint32_t type = 0;    // low byte: 1 npdrm self, 3 regular, 4 folder, 9 sdat ...
    bool folder() const { return (type & 0xff) == 4; }
};

class PkgFile {
public:
    ~PkgFile() { close(); }
    bool open(const std::wstring& file);   // false -> error()
    void close();
    const std::string& content_id() const { return content_id_; }   // UP0006-BLUS31540_00-...
    std::string title_id() const { return content_id_.size() >= 16 ? content_id_.substr(7, 9) : ""; }
    const std::vector<PkgEntry>& entries() const { return entries_; }
    const PkgEntry* find(const char* name) const;
    // Decrypts len bytes of an entry at off. Returns bytes produced.
    size_t read(const PkgEntry& e, uint64_t off, void* buf, size_t len);
    bool read_all(const PkgEntry& e, std::vector<uint8_t>& out);
    // SHA-1 over the package against the digest in its tail. progress(0..1) may cancel by returning false.
    bool verify_digest(const std::function<bool(double)>& progress);
    const std::string& error() const { return err_; }

private:
    bool read_raw(uint64_t at, void* buf, size_t n);
    bool decrypt(uint64_t off, uint8_t* buf, size_t n);   // off: in the data area, 16-aligned

    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint64_t size_ = 0, data_off_ = 0, data_size_ = 0;
    uint8_t iv_[16] = {};
    std::string content_id_, err_;
    std::vector<PkgEntry> entries_;
};
