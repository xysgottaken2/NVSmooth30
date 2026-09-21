#include "nvs30/fatbin.hpp"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace nvs30::fatbin {
namespace {
constexpr std::size_t align_up(std::size_t value, std::size_t alignment) noexcept {
    return (value + alignment - 1) & ~(alignment - 1);
}

constexpr std::uint32_t kFatbinMagic = 0xBA55ED50;
constexpr std::uint32_t kSm86 = kArchSm86;
constexpr std::uint32_t kSm89 = kArchSm89;
constexpr std::uint32_t kSm120 = kArchSm120;
constexpr std::uint32_t kSm75 = kArchSm75;
constexpr std::size_t kMaximumFatbin = 64u * 1024u * 1024u;

template <class T>
T read(const std::byte* p) {
    T value{};
    std::memcpy(&value, p, sizeof(value));
    return value;
}

template <class T>
void write(std::byte* p, T value) {
    std::memcpy(p, &value, sizeof(value));
}

bool is_elf64(const std::byte* p, std::size_t size) {
    return size >= 0x34 && p[0] == std::byte{0x7f} && p[1] == std::byte{0x45} &&
           p[2] == std::byte{0x4c} && p[3] == std::byte{0x46};
}
}

RewrittenImage rewrite_image(const void* image, std::size_t size, const Target& target) {
    RewrittenImage result;
    if (!image || size < 0x10) return result;
    const auto* source = static_cast<const std::byte*>(image);
    if (read<std::uint32_t>(source) != kFatbinMagic) return result;

    const std::uint16_t header_size = read<std::uint16_t>(source + 6);
    const std::uint64_t payload_size = read<std::uint64_t>(source + 8);
    if (header_size < 0x10 || header_size > 0x100 || payload_size > kMaximumFatbin ||
        payload_size > size - header_size) return result;

    const std::size_t total = header_size + static_cast<std::size_t>(payload_size);
    result.bytes.assign(source, source + total);
    auto* base = result.bytes.data();
    std::size_t cursor = header_size;

    while (cursor < total) {
        if (total - cursor < 0x20) return {};
        auto* entry = base + cursor;
        const std::uint16_t kind = read<std::uint16_t>(entry);
        const std::uint32_t entry_header = read<std::uint32_t>(entry + 4);
        const std::uint32_t data_size = read<std::uint32_t>(entry + 8);
        const std::uint32_t arch = read<std::uint32_t>(entry + 0x1c);
        if (entry_header < 0x20 || entry_header > 0x400 || entry_header > total - cursor)
            return {};
        const std::size_t data_offset = cursor + entry_header;
        if (data_size > total - data_offset) return {};

        ++result.stats.entries;
        if (kind == 2) ++result.stats.cubins;
        if (kind == 1) ++result.stats.ptx;
        if (arch == kSm120) ++result.stats.sm120_left;
        if (kind == 2 && arch == kSm75) ++result.stats.sm75_cubins;
        if (kind == 2 && arch == kSm89) ++result.stats.sm89_cubins;

        auto* payload = base + data_offset;
        if (kind == 2 && arch == target.source_arch && target.retarget_cubins) {
            write<std::uint32_t>(entry + 0x1c, target.target_arch);
            if (target.target_arch == kSm86) ++result.stats.sm89_to_sm86;
            if (target.target_arch == kSm75) ++result.stats.sm89_to_sm75;
            if (target.stamp_elf && is_elf64(payload, data_size)) {
                write<std::uint32_t>(payload + 0x30, target.elf_flags);
                ++result.stats.elf_headers;
            }
        }

        const std::size_t next = align_up(data_offset + data_size, std::size_t{8});
        if (next <= cursor || next > total) return {};
        cursor = next;
    }

    result.valid = cursor == total && result.stats.entries != 0;
    return result;
}

#ifdef _WIN32
RewrittenImage rewrite_image(const void* image, const Target& target) {
    if (!image) return {};
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(image, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return {};
    const auto available = static_cast<std::size_t>(
        static_cast<const std::byte*>(mbi.BaseAddress) + mbi.RegionSize -
        static_cast<const std::byte*>(image));
    return rewrite_image(image, available, target);
}

RewrittenImage rewrite_sm89_to_sm86(const void* image) {
    return rewrite_image(image, ampere_target());
}
#endif
}
