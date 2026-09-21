# NVSmooth30 → RTX 20 / Turing / SM75 port (technical dossier)

Target first silicon: **GeForce RTX 2060, compute capability 7.5 (SM75)**.
Goal: run NVIDIA's *real* driver-side Smooth Motion path (the `NvPresent64.dll`
pipeline the Ampere rehost already drives) on Turing, not an alternative frame
generator.

Everything below was derived by auditing this repository's actual code plus
empirically inspecting CUDA container formats with `ptxas` 11.8 and 12.9
(the fixture header `tests/fixture_cubins.hpp` embeds real cubins so these
facts are regression-tested in CI).

---

## 1. Complete inventory of the unlock

| Stage | File(s) | What it does | SM86-specific? |
|---|---|---|---|
| Injection | `src/version_proxy.cpp/.def`, `src/dllmain.cpp` | `version.dll` proxy next to the game EXE; loads System32 `version.dll`, forwards all 17 exports; starts `bootstrap()` | No |
| GPU capability gate | `src/nvpresent.cpp::find_gate` | Dynamic scan of executable sections for `83 79 14 <2\|3>` (`cmp dword ptr [rcx+14h], 2/3`) followed by `SETGE SIL` (or its already-patched `MOV SIL,1` form) | **No** (see §2) |
| Config struct | `src/nvpresent.cpp::find_config` | Resolves the writable `0x12a6`-byte config blob via the `LEA rip-rel` prologue inside `NVP_Init_D3D`; forces `+0x4c=1`, `+0xe9=1` before and after `NVP_Init_D3D` | No |
| Gate patch | `src/nvpresent.cpp::initialize` | Rewrites the compare immediate to `2` **and** replaces `SETGE SIL` with `MOV SIL,1` → the capability result is unconditionally TRUE; transactional rollback on any failure | No |
| CUDA interception | `src/nvpresent.cpp::install_cuda_hook` | Hooks `cuModuleLoadData` as consumed by `NvPresent64`: named IAT slot → validated reference-resolver fallback (offsets guarded by exact image-wide structural checks) → delay-IAT → resolved-value IAT → data-slot scan → `GetProcAddress` IAT last resort | No |
| **Kernel binary retarget** | `src/fatbin.cpp` | Rewrites each loaded fatbin's **kind-2 cubin entries from arch 0x59 (sm_89) to 0x56 (sm_86)** and stamps ELF `e_flags=0x06005604`; sm_120 entries and anything unknown are left untouched (fail-closed) | **YES — the only genuinely SM86-hard-coded component** |
| Presentation | `src/dxgi_hooks.cpp` | Hooks shared DXGI vtable slots 8/22 (Present/Present1) on all swapchains, primary-swapchain selection, latency-1, pacer | No |
| D3D11 bridge | `src/d3d11_bridge.cpp` | D3D11→D3D12 two-buffer flip-discard shadow chain + NT-handle shared texture (+legacy SHARED fallback) + validated NvPresent wrapper activation (vtable slots 19/20) | No — Turing has D3D12 FL 12_1 |
| OSD / pacer / logs | `src/osd.cpp`, `src/pacer.cpp`, `src/log.cpp` | Diagnostics only | No |
| Offline tools | `tools/inspect_nvp.py`, `tools/validate_project.py`, `tools/make_fixtures.py` | Regression/inspection | No |

## 2. Classification of every "block" found (SM86 → SM75 diff)

Per the requested taxonomy:

1. **`cmp dword ptr [rcx+14h], 2/3` + `SETGE SIL` architecture gate**
   → Class **(1) artificial whitelist**. It is a *runtime* check inside
   `NvPresent64.dll`, and the project already forces it true for any GPU
   (`MOV SIL,1`). Nothing to relax for Turing — the gate passes identically on
   an RTX 2060, provided the same structural pattern exists in that driver
   build (it is found dynamically, not by version).
2. **Config blob forces (`+0x4c`, `+0xe9`)**
   → Class (1)/(2), architecture-agnostic feature enables inside the same
   DLL's data. Unchanged by this port.
3. **`NVP_Init_D3D` presence + wrapper vtables (Present slots 8/22, wrapper
   slots 19/20)**
   → Class (3) API-shape requirements. Driver-version-dependent, **not**
   GPU-architecture dependent. Unchanged.
4. **Fatbin retarget `sm_89 → sm_86`**
   → Class **(6) code compiled specifically for a newer architecture** and
   partially **(7)**: retargeting a cubin's *metadata* only works when the
   embedded SASS executes on the target GPU. sm_89 (Ada) and sm_86 (GA10x)
   share SASS **major 8** with identical encodings for the instruction subset
   the kernels use — hence the Ampere rehost works. Turing is SASS **major 7**:
   instruction words, control-word layout and opcode tables differ. A
   metadata-only `sm_89 → sm_75` relabel produces a binary the loader may
   accept but the GPU **cannot decode**. This is **the one real barrier**.
5. **Tensor cores / async copy (`cp.async`, HMMA variants, redux.sync …)**
   → Class (5)/(8): none of these appear in this repository (no PTX/SASS
   source is shipped here — the kernels are NVIDIA-embedded in the driver's
   DLL). They would only matter in the hypothetical case where the encoding
   barrier were crossed, which it cannot be by relabeling. So the Ampere-only
   *instruction* question is moot for the port: the blocker fires before it.
6. **Driver-level feature flags** (DisplayDriver/DRS "SmoothMotionAvailable",
   NVPI per-game enable)
   → Class (2): enforced by the *installed driver*, bypassed by this project's
   config+gate patches. If any additional Turing-specific denial exists inside
   `nvwmi`/GSP code paths, it is invisible to static analysis of this repo;
   the new logging (§4) pinpoints the first stage that refuses.
7. **VRAM (6 GB on RTX 2060)**
   → Class (5) but non-blocking: the bridge adds one shared full-res texture
   (~30 MB @ 1080p BGRA) plus a 2-buffer shadow chain. Documented, not a
   correctness gate.

## 3. What the port actually changes

* **`include/nvs30/fatbin.hpp` / `src/fatbin.cpp`** — generalized the engine:
  `Target{source_arch,target_arch,retarget_cubins,stamp_elf,elf_flags}` with
  three entry points: `ampere_target()` (byte-for-byte the historical
  behavior), `scan_target()` (pure structural analysis, writes nothing), and
  `turing_target(mode)`. Stats gained `ptx`, `sm89_cubins`, `sm75_cubins`,
  `sm89_to_sm75`. Core is now portable (no Windows headers) and runs real
  unit tests on Linux/CI.
* **`include/nvs30/gpu.hpp` + `src/gpu.cpp`** (new) — runtime capability
  detection: `cuInit` (no context) + `cuDeviceGetAttribute(75/76)` against
  the already-installed `nvcuda.dll`, device picked by max memory
  (hybrid-laptop safe), `NVS30_CUDA_DEVICE` pins explicitly,
  `NVS30_FORCE_CC=7.5|8.6` overrides for experiments, everything cached and
  logged. Failure to query ⇒ `PassthroughOnly` ⇒ nothing is ever edited.
* **`src/nvpresent.cpp`** — `hooked_cu_module_load_data` is policy-driven:
  `AmpereRewrite` (default, identical legacy path **and** identical log
  strings), `TuringPolicy` (new `load_fatbin_for_turing`), `PassthroughOnly`.
  `initialize()` now logs the detected GPU + plan before touching anything.
* **`src/config.cpp` / `include/nvs30/config.hpp`** — new opt-ins (all off by
  default, legacy env keys untouched): `SM75_FORCE_CUBIN_REWRITE=1`,
  `SM75_ELF_STAMP=entryonly|driver75|mirror86`, `NVS30_ALLOW_CUDA_INIT=0`,
  `NVS30_FORCE_CC`, `NVS30_CUDA_DEVICE`.
* **`tools/inspect_nvp.py`** — now counts `ptx_entries` / `sm75_entries`
  across the *installed* `NvPresent64.dll` and prints a machine-readable SM75
  verdict. This answers the one open empirical question that decides which
  sub-path a given driver build takes on Turing (§5).
* **`tools/probe/probe.cpp` + `examples/run_probe_rtx2060.bat`** — hardware
  smoke harness (D3D11 FLIP_DISCARD chain, N Presents, exit code), built by
  CI so testers need no local toolchain.
* **Tests/CI** — `tests/fatbin_tests.cpp` + `tests/fixture_cubins.hpp`
  (real ptxas cubins; 31 checks incl. Ampere byte-parity, PTX detection,
  fail-closed rejection, malformed-container rejection), `CMakeLists.txt`
  test wiring, `.github/workflows/build.yml`, `.github/workflows/gpu-validation.yml`.

## 4. SM75 decision flow (what runs on the RTX 2060)

```
DllMain → bootstrap → nvpresent::initialize()
  gpu::query(): cuInit(0); CC = 7.5 → plan = TuringPolicy      [logged]
  locate NvPresent64.dll (DriverStore, newest)                  [logged]
  find_gate: cmp [rcx+14h],2/3 + SETGE                          [logged]
  find_config → install_cuda_hook → patch gate/config → NVP_Init_D3D
  every cuModuleLoadData from NvPresent:
    rewrite_image(image, scan_target)
      ├─ container unknown            → pass unmodified            [logged]
      ├─ PTX or native sm_75 present  → pass unmodified; the CUDA
      │                                 driver JITs/selects for
      │                                 sm_75 itself → kernels REAL,
      │                                 Smooth Motion LIVE on Turing
      ├─ cubin-only, default          → REFUSE (fail-closed), log
      │                                 "no PTX … major 8 not decodable"
      └─ SM75_FORCE_CUBIN_REWRITE=1   → EXPERIMENT: relabel entry arch
                                        to 0x4B (+optional ELF stamp);
                                        loader rc and any later CUDA
                                        error are logged; a TDR here is
                                        the documented outcome, games
                                        keep running (Present passthrough)
```

Gate/config/DXGI/D3D11-bridge stages are **architecture-agnostic** and
already proven by the Ampere path; on Turing the only stage whose outcome can
differ is fatbin selection, and the log says exactly which branch fired
(`grep '\[nvs30' nvsmooth30.log`).

## 5. Open empirical questions (must be answered on the RTX 2060)

1. **Does the user's driver `NvPresent64.dll` ship PTX in its Smooth Motion
   fatbins?** Run `py tools\inspect_nvp.py C:\Windows\System32\DriverStore\FileRepository\nvami*\NvPresent64.dll`.
   `ptx_entries>0` ⇒ the JIT branch makes Smooth Motion genuinely loadable on
   Turing today, with zero unsafe edits. `ptx_entries=0` (expected for recent
   drivers — the reference build logs `entries==cubins`) ⇒ branch 3/4.
2. **Does `libcuda` accept an entry-relabeled sm_75 cubin and where does it
   fail** (`SM75_FORCE_CUBIN_REWRITE=1`): clean `rc!=0` at load (driver
   validates) vs `rc==0` then illegal-instruction/TDR (driver does not
   validate). Both outcomes are informative; the log line distinguishes them.
3. **Any Turing-specific denial outside `NvPresent64`** (GSP/DRS): detectable
   by the new staged logs (`gate located` … `NVP_Init_D3D=TRUE` … wrapper
   activation … `[nvs30] Smooth Motion activated`).

## 5b. Confirmed by the first RTX 2060 field logs (2026-09-21)

* Injection, GPU detection (`cc=7.5 -> TuringPolicy`), dynamic gate/config
  discovery (`+0xc41f/+0xc437/+0x7f0cd0/+0x7fb628` — byte-identical to the
  reference fingerprint on the user's driver), `NVP_Init_D3D=TRUE` on Turing,
  `0x12a5` self-enabling, CUDA interception (2 fatbin intercepts) and the
  **D3D11→D3D12 bridge** (after the DISCARD-chain fix) are all verified
  working on real SM75 hardware.
* Both observed fatbins are `sm89=1 sm120=1 ptx=0` for this driver build:
  no PTX route → the fail-closed refusal is the correct default; the Stage-B
  `loader-rc` measurement remains the only outstanding datapoint.
* **Stage B measured on hardware (entryonly stamp):** both fatbins returned
  `loader-rc=300` (`CUDA_ERROR_INVALID_SOURCE` - libcuda rejects the module at
  `cuModuleLoadData`). Rejection was clean: no TDR, probe continued running the
  bridge end-to-end.  Entryonly leaves the cubin ELF declaring sm_89 while the
  container entry claims sm_75, so the Stage-C stamp sweep (`driver75` /
  `mirror86` self-consistent words) distinguishes an inconsistency rejection
  from a genuine ISA validation.
* `wrapper detected=0` in the probe is expected: NvPresent only attaches the
  swapchain wrapper when the application's driver profile enables Smooth
  Motion (NVIDIA Profile Inspector). Attaching NVPI to `nvs30_probe.exe` is
  therefore the additional "pipeline reaches kernels" experiment.
* Stage B must be launched via `run_probe_rtx2060_stageB.bat` (double-click):
  Explorer cannot pass `--force-experiment`, and a run whose log says
  `forced-cubin-rewrite=0` did NOT exercise the experiment.

## 5c. Stage C stamp sweep - FINAL measured result on the RTX 2060 (2026-09-21)

Probe exit codes per stamp mode (build `f45cf15`):

| Mode | ELF word | libcuda at `cuModuleLoadData` | Process outcome |
|---|---|---|---|
| 0 entryonly | unchanged (still sm_89) | `rc=300` `CUDA_ERROR_INVALID_SOURCE` | clean, probe exits 0, bridge stable |
| 1 driver75 | `0x05004B04` | **`rc=0` - module LOADED** (fatbin #19 logged) | `0xC0000005` STATUS_ACCESS_VIOLATION |
| 2 mirror86 | `0x06004B04` | same | same |

Conclusions, now with hardware evidence at every stage:

1. libcuda's earlier rejection was a **metadata self-consistency check**, not ISA
   validation: with entry arch and ELF e_flags both claiming sm_75, the driver
   loader accepts the module on a Turing device.
2. Acceptance is as far as metadata can carry the port: the first kernel
   invocation faults the host process, because the embedded instruction stream
   is SASS major 8 and the Turing front end cannot decode it. This is the
   **terminal barrier**; no flag, stamp, offset or profile value can cross it.
3. The only remaining theoretically-working Turing routes are (a) a driver
   build whose Smooth Motion fatbins ship PTX (check older/newer
   `NvPresent64.dll` with `tools\inspect_nvp.py`; NVSmooth30 auto-JITs those -
   already implemented and unit-tested), or (b) a general SASS recompiler,
   which this project's charter (and the port brief) explicitly excludes.
4. The default product behavior stays fail-closed (mode-0-equivalent refusal,
   game stable); the relabel modes remain probe-only opt-ins.

## 5d. Driver 616.92 compatibility matrix (researched 2026-09-21)

One `NvPresent64.dll` ships per Game Ready package for the whole GeForce
stack, so the live RTX 2060 field data in section 5/5c IS this driver
build's fingerprint: gate +0xc41f/+0xc437 matched, config enables
self-activated, fatbins `sm89+sm120, no PTX`, loader `rc=300/0` behavior
sweep. Release-note corroboration (616.92, 2026-09-09): Smooth Motion is
alive and actively fixed - DX11 `SM+G-SYNC` frame pacing improved
[6300603], DX11 jitter/ghosting [5937897] and SM launch crashes [5466398]
fixed (both carried from hotfix 610.52). 616.92 is the best SM driver of
the 61x branch; nothing about it changes the section-5c verdict for Turing.

| Driver (61x era) | RTX 30xx + NVSmooth30 | RTX 2060 |
|---|---|---|
| 610.47-610.88 | SM works in the field (community: RTX 3080 + 610.88, "Smooth Motion Version 1", 2026-09-20: works on Game Pass/Epic/standalone DX11 titles + RPCS3/DuckStation/Yuzu/Eden; known Steam-client-launch crash caveat below) | gates pass, kernels cannot execute (measured) |
| 616.92 | expected same-or-better: all 610.52/616.92 SM fixes present | unchanged: `ptx=0` measured on-package |
| older (546-596) | dll-swap community practice (596.49 `NvPresent64.dll` preferred for pacing by some) | **measured 2026-09-21, offline inspection of 595.71**: `fatbins=37 sm89_entries=37 sm120_entries=37 ptx_entries=0 sm75_entries=0` - no driver-side route to working Turing kernels in this branch. NVIDIA's download page offers no older Turing package and public archives (TechPowerUp driver DB) carry none for the 20-series either. **PTX hunt CLOSED 2026-09-21**: no obtainable driver for this GPU ships PTX for the Smooth Motion modules. |

PTX-hunt winning condition (both must hold on the same DLL):
`ptx_entries>0` **and** `gate_candidates>0` - PTX gives Turing-native
kernels via the driver's JIT (auto-used by the implemented pass-through),
while a resolvable gate lets the patcher arm the policy at all.

Status: hunt closed negative (595.71 + 616.92 both fail the `ptx_entries`
condition). The auto-JIT route stays dormant in the codebase - armed,
unit-tested, and ready without source changes should a PTX-carrying driver
ever become obtainable for Turing.

Steam caveat (from the field report, reproducer for ALL proxy-injection SM
unlocks incl. this project's `version.dll`): games launched **through the
Steam client** may crash at start; launching the game `.exe` directly works.
Mitigations: direct-exe launch, disable Steam overlay for the title, or add
the game to Steam as a non-Steam shortcut. Artifact note: interpolated
output on Ampere (no FP8/late-gen tensor path) shows more ghosting than
Ada/Blackwell at low base FPS - upstream is aware; not fixable by the port.

## 5e. FAQ: why not translation / an FSR-style replacement (the DLSSG mod analogy)?

Reader question: modders made "DLSS Frame Gen -> FSR 3 Frame Gen" work on
unsupported GPUs - why not do the same for Smooth Motion? Three distinct
routes, each closed for a different reason:

1. **API-shim replacement (the actual DLSSG->FSR3 trick).** That mod is
   possible because DLSS FG has a public, documented, versioned boundary:
   the game calls `nvngx_dlssg`'s COM-style API (D3D11/D3D12 resources +
   engine motion vectors go in, interpolated frame comes out). Swapping the
   implementation only requires honoring that contract. Smooth Motion has no
   such boundary: it is private machinery inside `NvPresent64.dll`, between
   the D3D11 runtime and the driver's own D3D12 shadow chain, with kernel
   arguments in undocumented internal structures and no stable ABI. There is
   no seam to shim from the outside; reproducing the slot means replacing the
   whole pipeline, which by definition is no longer "Smooth Motion on
   Turing" but a new frame-generation engine (out of this port's charter,
   which prohibits substituting algorithms).
2. **SASS translation (a "Rosetta" for cubins).** `nvdisasm` can read
   sm_89, but NVIDIA never published an assembler for sm_75 control words,
   scheduling/barrier semantics, or memory-model encodings; control bits are
   opaque and lossy even when round-tripped to PTX. A working major 8 ->
   major 7 translator is a standalone reverse-engineering project (years,
   per-driver-fragile) and the exact capability NVIDIA removed from the
   public toolchain. No PTX source exists in any obtainable Turing driver
   to sidestep this (measured, sections 5c/5d).
3. **The user-side equivalent that already exists.** For games with native
   DLSS 3 or FSR integration, FSR3-style FG mods run on RTX 20 today (AMD's
   SDK is open and has non-tensor DP4a profiles). That is a *different
   feature* with different coverage: it needs the game to expose engine
   motion vectors through a vendor SDK - the whole point of Smooth Motion is
   that it needs nothing from the game, which is why it is driver-resident,
   and why its driver-resident kernels are the hard wall this port hit.
4. **Game-side DLSS 5 (2026): DLSS5-Swapper ecosystem.** NVIDIA has no
   per-game DLSS 5 switch; modders filled the gap (github.com/rakanki911/
   DLSS5-Swapper): patched `nvngx_dlssnr.dll` + a "DLSS5-Feeder" route that
   adds Neural Rendering to games with no DLSS at all (DX8/9/11/12/VK/GL,
   emulators; estimated motion vectors). It claims RTX 20-50 compatibility
   and carries per-game/per-card community validation. Out of scope here:
   it is game-file injection (AC-visible), not driver patching, and does not
   provide frame generation - Smooth Motion's niche (and its wall) stand.

## 6. Known limitations

* **Barrier #1 (resolved, artificial):** the `[rcx+14h]>=2/3` whitelist — already forced
  by the existing transaction; works on Turing exactly as on Ampere.
* **Barrier #2 (structural, real):** SASS major 8 vs 7 — *no* metadata trick
  can make driver-shipped sm_89 cubins decode on Turing. Therefore on SM75:
  (a) PTX JIT pass-through is the supported route when the driver ships PTX;
  (b) cubin-only drivers can only be probed via the explicit opt-in
  experiment (loads may be accepted then fault on the GPU); a full "Ampere
  cubins run on Turing" claim would be false and is not made.
* Retarget decisions never modify files on disk; all edits are in-memory
  transactions with rollback.
* The forced-rewrite experiment can TDR the display driver — run the probe,
  not a ranked game, until the result is known.
* Hybrid laptops: device selection is max-memory + `NVS30_CUDA_DEVICE` pin;
  the D3D12 shadow chain follows the game's GPU preference, same as Ampere.
* `NvPresent64.dll` build-to-build drift (new gate layouts, bound-IAT
  variants) applies to Turing exactly as it does to RTX 30 — the rollback
  policy means "Smooth Motion stays off", never "game breaks".

## 7. Why RTX 30 support cannot regress

* `plan_for()` returns `TuringPolicy` **only** for CC exactly 7.5 (or
  `NVS30_FORCE_CC=7.5`); every other outcome, including all detection
  failures, keeps `AmpereRewrite`, which calls `rewrite_sm89_to_sm86()` —
  the historical function, same constants (`kElfSm86Flags=0x06005604`), same
  log lines, same transaction.
* The gate/config/DXGI/bridge code is untouched by the port.
* `tools/validate_project.py` fails the build if the SM86 constants or the
  fail-closed invariants disappear; `tests/fatbin_tests.cpp` asserts byte-level
  parity of the Ampere retarget against real cubins, in CI, every push.
