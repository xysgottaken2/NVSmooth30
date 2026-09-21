// Portable unit tests for the fatbin retarget engine.  These run on any host
// (CI executes them on Linux and Windows without an NVIDIA GPU) and use REAL
// cubins produced by ptxas as fixtures:
//   sm_75 -> e_flags 0x004B054B, sm_89 -> 0x00590559, sm_120 -> 0x06007802.
//
// Failures return the number of failed assertions; the CTest contract is
// exit 0 == pass.

#include "nvs30/fatbin.hpp"
#include "fixture_cubins.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace nvs30;

namespace {
int failures = 0;

void check(bool cond, const char* what) {
    if (cond) {
        std::printf("  ok: %s\n", what);
    } else {
        std::printf("  FAIL: %s\n", what);
        ++failures;
    }
}

template <class T>
void put(std::vector<std::byte>& out, T value) {
    const auto* p = reinterpret_cast<const std::byte*>(&value);
    out.insert(out.end(), p, p + sizeof(value));
}

void pad8(std::vector<std::byte>& out) {
    while (out.size() % 8) out.push_back(std::byte{0});
}

struct EntrySpec {
    std::uint16_t kind;
    std::uint32_t arch;
    std::vector<std::byte> data;
};

std::vector<std::byte> build_fatbin(const std::vector<EntrySpec>& entries) {
    std::vector<std::byte> payload;
    for (const auto& e : entries) {
        const std::uint32_t header_size = 0x20;
        const std::uint32_t data_size = static_cast<std::uint32_t>(e.data.size());
        std::vector<std::byte> data = e.data;
        while (data.size() % 8) data.push_back(std::byte{0});
        put<std::uint16_t>(payload, e.kind);
        put<std::uint16_t>(payload, 0);                         // variant
        put<std::uint32_t>(payload, header_size);               // +0x04 entry header size
        put<std::uint32_t>(payload, static_cast<std::uint32_t>(data.size()));  // +0x08 padded size
        put<std::uint32_t>(payload, 0x0100);                    // +0x0c container version
        put<std::uint64_t>(payload, data_size);                 // +0x10 unpadded data size
        put<std::uint32_t>(payload, 0);                         // +0x18 segment/flags
        put<std::uint32_t>(payload, e.arch);                    // +0x1c arch
        payload.insert(payload.end(), data.begin(), data.end());
    }
    std::vector<std::byte> out;
    put<std::uint32_t>(out, 0xBA55ED50u);  // fatbin magic
    put<std::uint16_t>(out, 1);            // container version
    put<std::uint16_t>(out, 0x10);         // container header size
    put<std::uint64_t>(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::byte* entry_at(std::vector<std::byte>& image, std::size_t index) {
    // All fixtures use 0x20-byte entry headers and the 0x10 container header;
    // the u32 at +8 is the padded segment size (as produced by build_fatbin).
    std::size_t cursor = 0x10;
    for (std::size_t i = 0; i < index; ++i) {
        std::uint32_t padded;
        std::memcpy(&padded, image.data() + cursor + 8, 4);
        cursor += 0x20 + padded;
    }
    return image.data() + cursor;
}

std::uint32_t arch_at(const std::vector<std::byte>& image, std::size_t index) {
    const auto* e = entry_at(const_cast<std::vector<std::byte>&>(image), index);
    std::uint32_t arch = 0;
    std::memcpy(&arch, e + 0x1c, 4);
    return arch;
}

std::uint32_t elf_flags_at(const std::vector<std::byte>& image, std::size_t index) {
    const auto* e = entry_at(const_cast<std::vector<std::byte>&>(image), index);
    std::uint32_t flags = 0;
    std::memcpy(&flags, e + 0x20 + 0x30, 4);  // ELF e_flags right after the entry header
    return flags;
}

std::vector<std::byte> bytes_of(const unsigned char* p, std::size_t n) {
    return std::vector<std::byte>(reinterpret_cast<const std::byte*>(p),
                                  reinterpret_cast<const std::byte*>(p) + n);
}
}

int main() {
    std::printf("[fatbin_tests] fixture sizes: sm75=%zu sm89=%zu sm120=%zu\n",
                fixture::kCubin_sm75_size, fixture::kCubin_sm89_size, fixture::kCubin_sm120_size);

    // --- 1. Ampere path parity: sm89 -> sm86 metadata retarget -------------
    std::printf("Ampere parity\n");
    {
        auto image = build_fatbin({
            {2, 89, bytes_of(fixture::kCubin_sm89, fixture::kCubin_sm89_size)},
            {2, 120, bytes_of(fixture::kCubin_sm120, fixture::kCubin_sm120_size)},
        });
        auto res = fatbin::rewrite_image(image.data(), image.size(), fatbin::ampere_target());
        check(res.valid, "container accepted");
        check(res.stats.entries == 2 && res.stats.cubins == 2, "two cubin entries counted");
        check(res.stats.sm89_cubins == 1, "one sm89 source");
        check(res.stats.sm89_to_sm86 == 1, "one retarget");
        check(res.stats.sm120_left == 1, "sm120 left untouched");
        check(res.stats.elf_headers == 1, "ELF e_flags stamped");
        check(arch_at(res.bytes, 0) == 0x56, "entry0 arch = sm_86");
        check(arch_at(res.bytes, 1) == 0x78, "entry1 arch stays sm_120");
        std::uint32_t stamped = 0;
        std::memcpy(&stamped, res.bytes.data() + 0x10 + 0x20 + 0x30, 4);
        check(stamped == 0x06005604u, "ELF stamp equals the verified SM86 word");
        // Payload of the untouched sm120 entry must be byte-identical.
        const std::size_t off1 = 0x10 + 0x20 +
            ((fixture::kCubin_sm89_size + 7) & ~std::size_t{7}) + 0x20;
        check(std::memcmp(res.bytes.data() + off1, fixture::kCubin_sm120,
                         fixture::kCubin_sm120_size) == 0,
              "sm120 payload unchanged");
    }

    // --- 2. Turing scan: nothing may be modified by default ----------------
    std::printf("Turing fail-closed scan\n");
    {
        auto image = build_fatbin({
            {2, 89, bytes_of(fixture::kCubin_sm89, fixture::kCubin_sm89_size)},
            {2, 120, bytes_of(fixture::kCubin_sm120, fixture::kCubin_sm120_size)},
        });
        auto res = fatbin::rewrite_image(image.data(), image.size(), fatbin::scan_target());
        check(res.valid, "scan still validates the container");
        check(res.stats.sm89_to_sm86 == 0 && res.stats.sm89_to_sm75 == 0, "scan rewrites nothing");
        check(res.bytes == image, "scan output byte-identical to input");
        check(res.stats.ptx == 0, "no PTX entries in the reference-shape blob");
    }

    // --- 3. PTX presence is detected (the JIT route for SM75) --------------
    std::printf("PTX pass-through detection\n");
    {
        const char* ptx = "//\n.version 8.3\n.target sm_75\n.address_size 64\n// end\n";
        auto image = build_fatbin({
            {1, 0x0100, bytes_of(reinterpret_cast<const unsigned char*>(ptx), std::strlen(ptx))},
            {2, 89, bytes_of(fixture::kCubin_sm89, fixture::kCubin_sm89_size)},
        });
        auto res = fatbin::rewrite_image(image.data(), image.size(), fatbin::scan_target());
        check(res.valid && res.stats.ptx == 1, "kind-1 entry counted as PTX");
        check(res.stats.entries == 2, "walk crosses the padded PTX entry");
        auto amp = fatbin::rewrite_image(image.data(), image.size(), fatbin::ampere_target());
        check(amp.valid && amp.stats.sm89_to_sm86 == 1, "Ampere retarget unaffected by PTX entry");
        const std::size_t ptx_off = 0x10 + 0x20;
        check(std::memcmp(amp.bytes.data() + ptx_off, ptx, std::strlen(ptx)) == 0,
              "PTX payload left byte-identical");
    }

    // --- 4. Native sm_75 cubins are recognised, never rewritten ------------
    std::printf("Native sm_75 cubin handling\n");
    {
        auto image = build_fatbin({
            {2, 75, bytes_of(fixture::kCubin_sm75, fixture::kCubin_sm75_size)},
        });
        auto res = fatbin::rewrite_image(image.data(), image.size(), fatbin::scan_target());
        check(res.stats.sm75_cubins == 1, "native sm_75 cubin counted");
        auto amp = fatbin::rewrite_image(image.data(), image.size(), fatbin::ampere_target());
        check(amp.stats.sm89_to_sm86 == 0, "sm_75 cubin is never retargeted to sm_86");
        check(arch_at(amp.bytes, 0) == 0x4B, "sm_75 arch value is 0x4B (verified against ptxas 12.9)");
    }

    // --- 5. Opt-in Turing experiment retargets entry arch + selectable stamp
    std::printf("Opt-in SM75 retarget experiment\n");
    {
        auto image = build_fatbin({
            {2, 89, bytes_of(fixture::kCubin_sm89, fixture::kCubin_sm89_size)},
        });
        auto a = fatbin::rewrite_image(image.data(), image.size(),
                                       fatbin::turing_target(fatbin::ElfStampMode::None));
        check(arch_at(a.bytes, 0) == 0x4B && a.stats.sm89_to_sm75 == 1, "entry-only retarget");
        check(a.stats.elf_headers == 0, "entry-only mode leaves the ELF word alone");
        check(elf_flags_at(a.bytes, 0) == 0x00590559u, "source e_flags preserved in entry-only mode");

        auto b = fatbin::rewrite_image(image.data(), image.size(),
                                       fatbin::turing_target(fatbin::ElfStampMode::Generation75));
        check(elf_flags_at(b.bytes, 0) == 0x05004B04u, "driver75 stamp word");

        auto c = fatbin::rewrite_image(image.data(), image.size(),
                                       fatbin::turing_target(fatbin::ElfStampMode::AmpereMirror));
        check(elf_flags_at(c.bytes, 0) == 0x06004B04u, "mirror86 stamp word");
    }

    // --- 6. Malformed containers are rejected (reference fail-closed) ------
    std::printf("Malformed input rejection\n");
    {
        check(!fatbin::rewrite_image(nullptr, 0, fatbin::ampere_target()).valid, "null image");
        std::vector<std::byte> garbage(0x100, std::byte{0x41});
        check(!fatbin::rewrite_image(garbage.data(), garbage.size(), fatbin::ampere_target()).valid,
              "no magic");
        auto image = build_fatbin({
            {3, 89, bytes_of(fixture::kCubin_sm89, fixture::kCubin_sm89_size)},
        });
        auto res = fatbin::rewrite_image(image.data(), image.size(), fatbin::ampere_target());
        check(res.valid && res.stats.sm89_to_sm86 == 0, "unknown kind 3 accepted as walk, never rewritten");

        auto two = build_fatbin({
            {2, 89, bytes_of(fixture::kCubin_sm89, fixture::kCubin_sm89_size)},
            {2, 120, bytes_of(fixture::kCubin_sm120, fixture::kCubin_sm120_size)},
        });
        // Cut the declared payload in half: the second entry now dangles -> reject.
        std::uint64_t half = two.size() - 0x10;
        half /= 2;
        std::memcpy(two.data() + 8, &half, 8);
        check(!fatbin::rewrite_image(two.data(), two.size(), fatbin::ampere_target()).valid,
              "truncated payload rejected");
    }

    std::printf(failures ? "FAILED: %d checks\n" : "ALL CHECKS PASSED (%d)\n", failures);
    return failures ? 1 : 0;
}
