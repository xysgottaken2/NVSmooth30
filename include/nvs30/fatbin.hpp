#pragma once
// Portable fatbin inspection/retarget engine.  This header deliberately does
// not include <windows.h> so the same core logic can be unit-tested on
// non-Windows hosts.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nvs30::fatbin {
// Fatbin "arch" field values are the plain decimal SM numbers used by the
// CUDA driver to select a cubin for the current device.
inline constexpr std::uint32_t kArchSm75 = 75;    // 0x4B - Turing (RTX 20)
inline constexpr std::uint32_t kArchSm86 = 86;    // 0x56 - Ampere  (RTX 30)
inline constexpr std::uint32_t kArchSm89 = 89;    // 0x59 - Ada     (RTX 40, shipped by the driver)
inline constexpr std::uint32_t kArchSm120 = 120;  // 0x78 - Blackwell (never retargeted)

// Driver-era ELF e_flags stamp the original project validated for sm_86 on
// RTX 30 hardware.  Public ptxas emits 0x00560556 for sm_86; the NVIDIA
// driver's internal toolchain stamps 0x06005604-style words instead.  Keep
// this exact constant: the Ampere path depends on it.
inline constexpr std::uint32_t kElfSm86Flags = 0x06005604;

// Candidate sm_75 stamps for the opt-in Turing cubin retarget experiment.
// None of these can be validated without an RTX 20-series device, so the
// retarget is disabled by default and the mode is selectable.
inline constexpr std::uint32_t kElfSm75GenerationFlags = 0x05004B04;  // generation byte = 5 for major 7
inline constexpr std::uint32_t kElfSm75AmpereMirrorFlags = 0x06004B04; // keep the sm_86-era generation byte

enum class ElfStampMode : std::uint32_t {
    None = 0,        // only the fatbin entry arch field is retargeted (safest, default for SM75)
    Generation75,   // stamp 0x05004B04 into the ELF header
    AmpereMirror,   // stamp 0x06004B04 into the ELF header (mirror of the verified SM86 word)
};

struct Target {
    std::uint32_t source_arch = kArchSm89;   // cubin entries to rewrite
    std::uint32_t target_arch = kArchSm86;   // arch value written to the fatbin entry
    bool retarget_cubins = true;             // false => pure scan, no bytes are modified
    bool stamp_elf = false;                  // write an e_flags word into ELF64 headers
    std::uint32_t elf_flags = kElfSm86Flags; // stamp used when stamp_elf is set
};

inline Target ampere_target() {
    // Byte-for-byte the historical rewrite_sm89_to_sm86 behaviour.
    Target t{};
    t.source_arch = kArchSm89;
    t.target_arch = kArchSm86;
    t.retarget_cubins = true;
    t.stamp_elf = true;
    t.elf_flags = kElfSm86Flags;
    return t;
}

inline Target turing_target(ElfStampMode mode) {
    Target t{};
    t.source_arch = kArchSm89;
    t.target_arch = kArchSm75;
    t.retarget_cubins = true;
    t.stamp_elf = mode != ElfStampMode::None;
    t.elf_flags = mode == ElfStampMode::AmpereMirror ? kElfSm75AmpereMirrorFlags
                                                      : kElfSm75GenerationFlags;
    return t;
}

inline Target scan_target() {
    Target t{};
    t.retarget_cubins = false;
    t.stamp_elf = false;
    return t;
}

struct RewriteStats {
    std::uint32_t entries{};      // fatbin entries walked
    std::uint32_t cubins{};       // entries of kind 2 (cubin/ELF)
    std::uint32_t ptx{};          // entries of kind 1 (PTX text, JIT-able)
    std::uint32_t sm89_cubins{};  // kind-2 entries declaring SM89 (source set)
    std::uint32_t sm89_to_sm86{}; // retargeted Ampere cubins (historical counter)
    std::uint32_t sm89_to_sm75{}; // retargeted Turing-experiment cubins
    std::uint32_t sm75_cubins{};  // cubins already natively targeting SM75
    std::uint32_t sm120_left{};   // Blackwell cubins deliberately untouched
    std::uint32_t elf_headers{};  // ELF e_flags words written
};

struct RewrittenImage {
    std::vector<std::byte> bytes;
    RewriteStats stats;
    bool valid{}; // true when the whole container parsed against the reference layout
};

// Parse (and optionally rewrite) a fatbin image of exactly `size` bytes.
// `result.bytes` always contains the parsed image; callers must only feed it
// back to cuModuleLoadData when they requested retargeting and stats confirm
// at least one rewritten entry.  On any structural surprise the result is
// invalid and nothing is trusted (reference fail-closed policy).
RewrittenImage rewrite_image(const void* image, std::size_t size, const Target& target);

#ifdef _WIN32
// Windows convenience entry point: derives the scan bound from the committed
// region of `image`, matching the original implementation exactly.
RewrittenImage rewrite_image(const void* image, const Target& target);
RewrittenImage rewrite_sm89_to_sm86(const void* image);
#endif
}
