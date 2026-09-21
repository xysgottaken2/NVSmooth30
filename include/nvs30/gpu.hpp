#pragma once
#include "common.hpp"

namespace nvs30::gpu {
// What the loader should do for the GPU actually present in this process.
enum class Plan : std::uint32_t {
    AmpereRewrite = 0, // historical default: retarget driver fatbin SM89 cubins to SM86
    TuringPolicy = 1,  // RTX 20 / SM75: prefer PTX JIT pass-through; cubin retarget is opt-in
    PassthroughOnly = 2, // CUDA could not be interrogated; never retarget anything
};

struct Info {
    bool detected{};        // attributes were read successfully (or overridden)
    bool from_override{};   // NVS30_FORCE_CC was used
    int major{-1};
    int minor{-1};
    std::uint32_t selected_device{0xFFFFFFFFu};
    std::string name;
    std::string source;     // "cuda" | "override" | "nvcuda-missing" | "cuinit-failed" | "no-device" | "disabled"
    Plan plan{Plan::AmpereRewrite};
};

// Lazily queries and caches the compute capability of the GPU CUDA will use
// in this process.  Never throws, never logs on its own; safe from any thread.
const Info& query();

const char* plan_name(Plan plan) noexcept;
}
