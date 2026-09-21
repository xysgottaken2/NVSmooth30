#include "nvs30/gpu.hpp"

#include "nvs30/config.hpp"

#include <cstdlib>
#include <cstring>
#include <cwchar>

// Same calling convention the CUDA driver API uses on Windows; on x64 this
// collapses to the single native convention.  We avoid pulling in <cuda.h>
// because the project ships no CUDA SDK dependency.
#ifndef CUDAAPI
#define CUDAAPI __stdcall
#endif

namespace nvs30::gpu {
namespace {
using CuInit = int (CUDAAPI*)();
using CuDeviceGetCount = int (CUDAAPI*)(int*);
using CuDeviceGet = int (CUDAAPI*)(int*, int);
using CuDeviceGetName = int (CUDAAPI*)(char*, int, int);
using CuDeviceGetAttribute = int (CUDAAPI*)(int*, int, int);
using CuDeviceTotalMem = int (CUDAAPI*)(std::size_t*, int);

constexpr int kAttrComputeCapabilityMajor = 75;
constexpr int kAttrComputeCapabilityMinor = 76;

// The CUDA driver API numbers are part of the stable ABI; the game's own
// usage of nvcuda.dll already requires these exports.  We only query
// attributes; we never create a context and never call into the driver after
// nvpresent::install_cuda_hook redirects loader entry points.
struct Loader {
    HMODULE nvcuda{};
    CuInit init{};
    CuDeviceGetCount get_count{};
    CuDeviceGet get{};
    CuDeviceGetName get_name{};
    CuDeviceGetAttribute get_attr{};
    CuDeviceTotalMem total_mem{};
};

bool parse_cc(const std::wstring& text, int& major, int& minor) {
    if (text.empty()) return false;
    wchar_t* end{};
    const long m = std::wcstol(text.c_str(), &end, 10);
    if (end == text.c_str() || m < 1 || m > 20) return false;
    if (*end == L'.') {
        const wchar_t* rest = end + 1;
        const long n = std::wcstol(rest, &end, 10);
        if (end != rest && n >= 0 && n <= 9) {
            if (*end == L'x' || *end == L'X') ++end;
            major = static_cast<int>(m);
            minor = static_cast<int>(n);
            return *end == L'\0';
        }
        return false;
    }
    // Allow the "75"/"86" spelling too.
    if (*end == L'\0' && m >= 10 && m <= 209) {
        major = static_cast<int>(m / 10);
        minor = static_cast<int>(m % 10);
        return true;
    }
    return false;
}

Plan plan_for(int major, int minor) {
    // 7.5 (Turing RTX 20) gets the new policy.  Everything else, including
    // undetected GPUs, keeps the historical Ampere behaviour of the project
    // so RTX 30 users see no change whatsoever.
    if (major == 7 && minor == 5) return Plan::TuringPolicy;
    return Plan::AmpereRewrite;
}

Info probe() {
    Info info;
    int major = -1;
    int minor = -1;
    if (!config().force_cc.empty() && parse_cc(config().force_cc, major, minor)) {
        info.detected = true;
        info.from_override = true;
        info.major = major;
        info.minor = minor;
        info.name = "override";
        info.source = "override";
        info.plan = plan_for(major, minor);
        return info;
    }
    if (!config().allow_cuda_init) {
        info.source = "disabled";
        info.plan = Plan::PassthroughOnly;
        return info;
    }

    Loader ld;
    ld.nvcuda = GetModuleHandleW(L"nvcuda.dll");
    if (!ld.nvcuda) ld.nvcuda = LoadLibraryW(L"nvcuda.dll");
    if (!ld.nvcuda) {
        info.source = "nvcuda-missing";
        info.plan = Plan::PassthroughOnly;
        return info;
    }
    ld.init = reinterpret_cast<CuInit>(GetProcAddress(ld.nvcuda, "cuInit"));
    ld.get_count = reinterpret_cast<CuDeviceGetCount>(GetProcAddress(ld.nvcuda, "cuDeviceGetCount"));
    ld.get = reinterpret_cast<CuDeviceGet>(GetProcAddress(ld.nvcuda, "cuDeviceGet"));
    ld.get_name = reinterpret_cast<CuDeviceGetName>(GetProcAddress(ld.nvcuda, "cuDeviceGetName"));
    ld.get_attr = reinterpret_cast<CuDeviceGetAttribute>(
        GetProcAddress(ld.nvcuda, "cuDeviceGetAttribute"));
    ld.total_mem = reinterpret_cast<CuDeviceTotalMem>(
        GetProcAddress(ld.nvcuda, "cuDeviceTotalMem_v2"));
    if (!ld.init || !ld.get_count || !ld.get || !ld.get_attr) {
        info.source = "nvcuda-missing";
        info.plan = Plan::PassthroughOnly;
        return info;
    }
    if (ld.init(0) != 0) {
        info.source = "cuinit-failed";
        info.plan = Plan::PassthroughOnly;
        return info;
    }
    int count = 0;
    if (ld.get_count(&count) != 0 || count <= 0) {
        info.source = "no-device";
        info.plan = Plan::PassthroughOnly;
        return info;
    }

    // Pick the device NVSmooth30 should reason about.  On hybrid systems the
    // discrete GPU is the CUDA device with the most memory; NVS30_CUDA_DEVICE
    // pins an explicit index for exotic rigs.  Every device is logged by the
    // caller-visible enumeration below.
    int selected = -1;
    std::size_t selected_mem = 0;
    for (int i = 0; i < count; ++i) {
        if (config().cuda_device >= 0 && i != config().cuda_device) continue;
        int device = -1;
        if (ld.get(&device, i) != 0) continue;
        std::size_t bytes = 0;
        if (ld.total_mem) ld.total_mem(&bytes, device);
        if (bytes >= selected_mem) {
            selected = device;
            selected_mem = bytes;
        }
    }
    if (selected < 0) {
        info.source = "no-device";
        info.plan = Plan::PassthroughOnly;
        return info;
    }
    int m = -1;
    int n = -1;
    if (ld.get_attr(&m, kAttrComputeCapabilityMajor, selected) != 0 ||
        ld.get_attr(&n, kAttrComputeCapabilityMinor, selected) != 0) {
        info.source = "no-device";
        info.plan = Plan::PassthroughOnly;
        return info;
    }
    info.detected = true;
    info.selected_device = static_cast<std::uint32_t>(selected);
    info.major = m;
    info.minor = n;
    info.source = "cuda";
    info.plan = plan_for(m, n);
    if (ld.get_name) {
        char name[128]{};
        if (ld.get_name(name, static_cast<int>(sizeof(name)) - 1, selected) == 0)
            info.name = name;
    }
    return info;
}

Info g_info;
std::once_flag g_once;

const char* names[] = {"AmpereRewrite", "TuringPolicy", "PassthroughOnly"};
}

const Info& query() {
    std::call_once(g_once, [] { g_info = probe(); });
    return g_info;
}

const char* plan_name(Plan plan) noexcept {
    const auto index = static_cast<std::uint32_t>(plan);
    return index < 3 ? names[index] : "Unknown";
}
}
