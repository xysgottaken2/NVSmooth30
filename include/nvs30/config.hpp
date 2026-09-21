#pragma once
#include "common.hpp"

namespace nvs30 {
struct Config {
    bool enable_osd = false;
    bool enable_d3d11_bridge = false;
    bool force_vsync = false;
    bool diagnostics = false;
    bool low_latency = true;
    bool half_refresh_cap = false;
    float base_fps_cap = 0.0f;
    bool bridge_linearize = false;
    std::wstring nvpresent_path;

    // --- SM75 / Turing port controls (all default to legacy behaviour) ---
    bool sm75_force_cubin_rewrite = false; // experimental cross-major retarget
    std::uint32_t sm75_elf_stamp = 0;      // 0 none, 1 generation75, 2 ampere-mirror
    bool allow_cuda_init = true;           // cuInit(0) query for device detection
    std::wstring force_cc;                 // e.g. L"7.5" pins the capability for tests
    int cuda_device = -1;                  // pin a CUDA device index on hybrid systems
};

const Config& config();
void load_config();
}

