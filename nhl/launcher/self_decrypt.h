// NPDRM SELF -> ELF (free-license NPDRM, as the game update's EBOOT.BIN is).
// Written from the format as documented by RPCS3's unself; the key values are
// the console's public SELF/NPDRM keys.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Decrypts a SELF image in memory into an ELF. Returns "" or an error message.
std::string self_to_elf(const std::vector<uint8_t>& self, std::vector<uint8_t>& elf);
