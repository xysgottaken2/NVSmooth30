# Blueprint: "Turing-FG" - an SM-*like* frame generator for RTX 20 as a standalone app

Status: design only. This is deliberately NOT part of the NVSmooth30 port (whose
charter forbids substituting algorithms); it is the separate project the owner
asked about ("e se criássemos nosso próprio app com DLSS 5 + frame gen?").
Keeping it here so the measured verdicts above flow into the next thing.

## What it must and must not be

Goal stack on RTX 2060 (2026-09), per owner: **DLSS 5 quality + frame generation
+ works in any game + anti-cheat tolerable**.

| Property | Real Smooth Motion | This app |
|---|---|---|
| Runs inside driver (pre-composition) | yes | no - independent process, capture+overlay |
| Needs game cooperation | never | never (same, by construction) |
| Invisible to anti-cheat | yes (driver module) | yes-ish: desktop duplication/WGC is what Discord/OBS use; zero game-process writes. Lose this if exclusive-fullscreen hooks are added |
| Interpolation quality | NVIDIA driver-side MV/depth magic | optical-flow class (LSFG-equivalent ceiling) |
| Added latency | ~sub-frame (async) | ~+1 displayed frame (queue for the "next" real frame) |
| Requires | RTX 40/50 | any GPU with OFA (or shader fallback) |
| Composes with DLSS 5 (Swapper/Feeder, game-side) | yes | yes - orthogonal layers: Feeder renders the game, we duplicate the result |

## Architecture (MVP)

```
 game window (borderless) ──► Windows.Graphics.Capture / DXGI duplication
                                     │  frame N-1, N (shared textures)
                                     ▼
                    NVIDIA OptiFlow via nvofapi64.dll  (CUDA path)
                     - Turing TU106: OFA gen-3 = SUPPORTED by nvofapi
                     - VK_NV_optical_flow = NOT listed for Turing (verified
                       2026-09; FFmpeg fruc_vulkan starts at Ampere) ->
                       do not use the Vulkan path for 20-series targets
                     - fallback: shader optical flow (compute) for non-OFA
                                     │  flow (N-1->N) + validity
                                     ▼
                    intermediate frame warp+blend + occlusion fill
                     (borrow: FFmpeg fruc_vulkan blending approach - LGPL,
                      referenceable; NvFRUC SDK lib itself: do NOT
                      redistribute - licensing is what killed vf_nvoffruc 2023)
                                     │
                     pacing engine: 2:1 (60 real -> 120 out), 3:1 later
                                     ▼
                    D3D11 fullscreen-borderless overlay swapchain
                     (Magpie-class compositor; Present at display rate)
```

## Component choices

- Capture: WGC (Windows 10+) with DXGI duplication fallback; sub-millisecond
  GPU-to-GPU copies via shared NT-handle textures. (Precedent: Magpie; our repo
  already has a D3D11->D3D12 shadow-chain bridge in `src/d3d11_bridge.cpp`
  demonstrating the same interop class of problems.)
- Flow: `nvofapi64.dll` ships with the NVIDIA display driver since R418-ish;
  Maxwell/Turing/Ada all enumerated by the old NV Optical Flow SDK. Call it via
  CUDA D3D11 interop - zero install-time dependencies beyond the driver.
- Occlusion/blend: own HLSL pass; quality knobs (blend strength, motion
  threshold, HUD protection rectangle) exposed in an F8-style overlay.
- Pacing: present(i), present(interp(i,i+1)) on a vsync-locked swapchain with
  queue depth 2; Reflex-style "anti-buffering" approximated by
  `IDXGIDevice2::SetMinimumSwapchainLatency`-family settings where available.
- Precedent code to read (license-clean): `lsfg-vk` (MIT) proves the whole
  present-level trick chain without engine cooperation; `Magpie` (MIT) proves
  the capture/overlay shell; FFmpeg `vf_fruc_vulkan` for warp/blend math.

## Milestones

1. **M0 - compositor shell** (~1 week): WGC capture -> overlay at 1:1, frame
   counters, HDR passthrough deferred (sRGB MVP).
2. **M1 - 2x flow interpolation** (~1-2 weeks): nvofapi flow on Turing,
   warp+blend, static-scene quality > SVP, motion > LSFG-v2-era.
3. **M2 - game UX**: per-game profiles (frame-rate target, HUD rectangle),
   latency metering (LDAT-style via capture timestamps), adaptive mode
   (skip interpolation when real fps >= display rate).
4. **M3 - 3:1 / 4:1** with a small occlusion-aware refinement; optional
   DLSS5-Feeder integration guide (stacking doc, not code).

## Honest limits (say them to users before they build or buy)

- Fast-motion ghosting will never match driver-resident SM: no engine motion
  vectors by design; only what OFA can recover from pixels.
- +~1 frame of latency: fine for 60->120 casual; competitive play is out.
- Exclusive-fullscreen games: either run borderless (stays AC-clean) or a
  hook is added (loses the clean story - do not hook online AC games).
- VRAM 6GB: capture+flow working set ~ +250-400MB at 1440p - budget it.
- This is not "Smooth Motion". It is an LSFG-class open implementation with
  OFA acceleration. The measured verdict of sections 5b/5c/5d about NVIDIA's
  own kernels on Turing stands and is untouched by this app.

## What would change this ranking

NVIDIA exposing driver-side MV/depth to a non-40 API, a PTX-carrying driver
(monitored by `.github/workflows/ptx-watchdog.yml` - issue auto-opens), or
AMD-style AFMF-for-GeForce (neither is announced as of 2026-09).


## Evidence anchors from the reference project (ReverieBizarre/Smooth-Motion-for-RTX30)

Published structure we would build against (their RTX 3080 validation ran on
driver 616.56 - same era as our measurements; their "unpatched Ampere ->
CUDA_ERROR_NO_BINARY_FOR_GPU (209)" result is independent confirmation that
the shipped containers rely solely on cubin relabeling: PTX was absent at the
feature's birth, else Ampere unlocking would never have needed binary
surgery, QMMA/FP8 kernel isolation, etc.):

- 37 embedded fatbins; each = {sm_89, sm_120} ELF pair.
- Master pointer table of 36 fatbin images at a fixed image VA (0x18022ead0
  in their build) - entries [0..18] are the 19 FP16 modules actually used on
  Ampere: conv1..8, conv_fused, conv_proj1/2, conv_out1/2/3, attn1/2,
  depth_to_space, downscale_kernel, warp_coarse_kernel, main_kernel.
- Execution: CUDA-graph ping-pong via cuGraphLaunch with a resolution guard;
  5-object COM swapchain hierarchy drives activation.
- FP8 variants are separate fatbins (Q/MMA instructions absent on Ampere) -
  the same instruction-class reasoning our ladders encode.

Consequences for a custom-kernel route (path "modder"):

1. The ABI surface to satisfy is named: ~19 module/function signatures +
   graph launch order + buffer ping-pong contract. First milestone should be
   a harness that replaces intercepted modules with pass-through kernels and
   LOGS every cuModuleGetFunction name and launch parameter block, rather
   than trying to statically reverse each structure.
2. Weight reuse is off the table (NVIDIA's trained FP16 model lives in the
   sm_89 cubin constant banks; extracting/repurposing it is model theft, not
   porting). The custom pass is flow+warp math of our own = LSFG-class
   quality, honestly labeled "Smooth-Motion-compatible", never "Smooth
   Motion".
3. Pointer-table VAs and gates are per-driver-build; the existing pattern
   scanners in this repo (not raw addresses) are the only durable way in.
