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
