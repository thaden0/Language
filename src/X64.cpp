#include "X64.hpp"
#include <cstring>

// Minimal static ELF64. Layout: [ELF header | program header | code | data],
// the whole file mapped once (R+W+X) at BASE. entry = BASE + fileOffsetOfCode +
// entryOffset. Data is placed right after code; its virtual address is returned
// so codegen can reference string/constant bytes.
std::string makeElf(const std::vector<uint8_t>& code, size_t entryOffset,
                    const std::vector<uint8_t>& data, uint64_t& dataVAddrOut) {
    const uint64_t BASE = 0x400000;
    const uint64_t EHSIZE = 64, PHSIZE = 56;
    const uint64_t codeOff = EHSIZE + PHSIZE;       // file offset of code
    const uint64_t dataOff = codeOff + code.size();
    const uint64_t fileSize = dataOff + data.size();
    dataVAddrOut = BASE + dataOff;
    const uint64_t entry = BASE + codeOff + entryOffset;

    std::string out;
    out.resize(fileSize, 0);
    auto put = [&](uint64_t off, const void* p, size_t n) { std::memcpy(&out[off], p, n); };
    auto put16 = [&](uint64_t off, uint16_t v) { put(off, &v, 2); };
    auto put32 = [&](uint64_t off, uint32_t v) { put(off, &v, 4); };
    auto put64 = [&](uint64_t off, uint64_t v) { put(off, &v, 8); };

    // --- ELF header ---
    out[0] = 0x7f; out[1] = 'E'; out[2] = 'L'; out[3] = 'F';
    out[4] = 2;            // ELFCLASS64
    out[5] = 1;            // ELFDATA2LSB
    out[6] = 1;            // EV_CURRENT
    put16(16, 2);          // ET_EXEC
    put16(18, 0x3e);       // EM_X86_64
    put32(20, 1);          // version
    put64(24, entry);      // e_entry
    put64(32, EHSIZE);     // e_phoff
    put64(40, 0);          // e_shoff
    put32(48, 0);          // e_flags
    put16(52, EHSIZE);     // e_ehsize
    put16(54, PHSIZE);     // e_phentsize
    put16(56, 1);          // e_phnum
    put16(58, 0);          // e_shentsize
    put16(60, 0);          // e_shnum
    put16(62, 0);          // e_shstrndx

    // --- program header (one PT_LOAD, R+W+X) ---
    put32(EHSIZE + 0, 1);          // p_type = PT_LOAD
    put32(EHSIZE + 4, 7);          // p_flags = R|W|X
    put64(EHSIZE + 8, 0);          // p_offset
    put64(EHSIZE + 16, BASE);      // p_vaddr
    put64(EHSIZE + 24, BASE);      // p_paddr
    put64(EHSIZE + 32, fileSize);  // p_filesz
    put64(EHSIZE + 40, fileSize);  // p_memsz
    put64(EHSIZE + 48, 0x1000);    // p_align

    if (!code.empty()) put(codeOff, code.data(), code.size());
    if (!data.empty()) put(dataOff, data.data(), data.size());
    return out;
}
