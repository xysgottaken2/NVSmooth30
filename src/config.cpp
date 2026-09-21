#include "nvs30/config.hpp"

#include <cmath>
#include <cstdlib>
#include <cwchar>

namespace nvs30 {
namespace {
Config g_config;

bool env_bool(const wchar_t* name, bool fallback) {
    wchar_t value[32]{};
    const DWORD n = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    if (!n || n >= std::size(value)) return fallback;
    return _wcsicmp(value, L"1") == 0 || _wcsicmp(value, L"true") == 0 ||
           _wcsicmp(value, L"yes") == 0 || _wcsicmp(value, L"on") == 0;
}

float env_float(const wchar_t* name, float fallback) {
    wchar_t value[64]{};
    const DWORD n = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    if (!n || n >= std::size(value)) return fallback;
    wchar_t* end{};
    const float parsed = std::wcstof(value, &end);
    return end != value && std::isfinite(parsed) ? parsed : fallback;
}

std::wstring env_string(const wchar_t* name) {
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (!needed) return {};
    std::wstring out(needed, L'\0');
    const DWORD n = GetEnvironmentVariableW(name, out.data(), needed);
    if (!n || n >= needed) return {};
    out.resize(n);
    return out;
}
}

const Config& config() { return g_config; }

void load_config() {
    g_config.enable_osd = env_bool(L"SM86_ENABLE_OSD", false);
    g_config.enable_d3d11_bridge = env_bool(L"SM86_ENABLE_D3D11_BRIDGE", true);
    g_config.force_vsync = env_bool(L"SM86_FORCE_VSYNC", false);
    g_config.diagnostics = env_bool(L"SM86_DIAGNOSTICS", false);
    g_config.low_latency = env_bool(L"SM86_LOW_LATENCY", true);
    g_config.half_refresh_cap = env_bool(L"SM86_HALF_REFRESH_CAP", false);
    g_config.base_fps_cap = std::max(0.0f, env_float(L"SM86_BASE_FPS_CAP", 0.0f));
    g_config.bridge_linearize = env_bool(L"SM86_BRIDGE_LINEARIZE", false);
    g_config.nvpresent_path = env_string(L"SM86_NVPRESENT_PATH");

    // Turing/SM75 port switches.  SM75_* is the primary spelling; SM86_*
    // aliases keep existing launcher scripts working for Ampere users.
    g_config.sm75_force_cubin_rewrite =
        env_bool(L"SM75_FORCE_CUBIN_REWRITE",
                 env_bool(L"SM86_SM75_FORCE_CUBIN_REWRITE", false));
    {
        const std::wstring stamp = env_string(L"SM75_ELF_STAMP");
        if (_wcsicmp(stamp.c_str(), L"driver75") == 0 || _wcsicmp(stamp.c_str(), L"1") == 0)
            g_config.sm75_elf_stamp = 1;
        else if (_wcsicmp(stamp.c_str(), L"mirror86") == 0 || _wcsicmp(stamp.c_str(), L"2") == 0)
            g_config.sm75_elf_stamp = 2;
        else
            g_config.sm75_elf_stamp = 0;
    }
    g_config.allow_cuda_init = env_bool(L"NVS30_ALLOW_CUDA_INIT", true);
    g_config.force_cc = env_string(L"NVS30_FORCE_CC");
    const std::wstring device = env_string(L"NVS30_CUDA_DEVICE");
    if (!device.empty()) {
        wchar_t* end{};
        const long value = std::wcstol(device.c_str(), &end, 10);
        if (end != device.c_str() && value >= 0 && value <= 64)
            g_config.cuda_device = static_cast<int>(value);
    }
}
}
