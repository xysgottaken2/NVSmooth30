# NVSmooth30

NVSmooth30 is a clean-room, source-available compatibility experiment for
running NVIDIA's `NvPresent64.dll` Smooth Motion path on SM86/RTX 30-series
hardware, with an added **SM75 / RTX 20-series (Turing) compatibility layer**:
GPU-aware fatbin policy, PTX-JIT pass-through for the driver's kernels, full
staged diagnostics, and a hardware smoke probe. The Ampere path is unchanged
byte-for-byte; see [docs/SM75_PORT.md](docs/SM75_PORT.md) for the complete
audit of which checks are artificial (already bypassed on Turing) and which
are real architectural barriers (SASS major 7 vs 8).

It is not NVIDIA software and contains no NVIDIA binaries or copied
source code.

##Download Release##

https://www.nexusmods.com/site/mods/2344

1. Extract the version.dll into the game folder with the executable (Rename the version.dll into a .asi if needed, use Ultimate ASI Loader only if using ASI).
2. Download Nvidia Profile Inspector.
3. Open up Nvidia Profile Inspector and enable Smooth motion for your game and hit "Apply Changes" (You will need to do this everytime your drivers are updated)
4. Launch your game.

## Status and safety

This is an **alpha interoperability project**. It was designed against the
observed behavior of one reference `NvPresent64.dll` build, SHA-256:

`cd395d58f41c6e393c31a9898f2be3da83f7bc109a2228935c23e8c89b944c15`

It does not hard-code that build's internal RVAs. Runtime scanners require the validated compare/capability structure used by
the reference path and abort without patching when that structure is unknown. New NVIDIA versions can still change semantics, kernel formats, or
presentation behavior.

The CUDA step is conservative metadata retargeting, not a general SASS binary
translator. It is suitable only while the shipped SM89 kernels use instructions
that are valid on SM86. Unknown fatbin layouts are rejected instead of edited.
On SM75/Turing the retarget is additionally restricted by the SASS major-version
barrier: the loader only ever receives pass-through images unless you enable the
explicit `SM75_FORCE_CUBIN_REWRITE` experiment, and PTX-carrying fatbins are
- `SM75_ABI_PROBE=1` - path-3 contract survey: replaces intercepted modules with JIT'd sm_75 no-op stubs and logs every NvPresent kernel contract under `[nvs30-abi]`; never for gameplay (docs/SM75_PORT.md section 5f).
handed to the driver's own JIT (the supported Turing route, when available).

Hardware field status (measured on RTX 2060, driver 616.92): every artificial
gate, enable and the D3D11->D3D12 bridge pass on Turing; the forced cubin
relabel is accepted by the driver loader but the major-8 kernels cannot
execute on major-7 hardware, so SM75 stays fail-closed by default (details in
`docs/SM75_PORT.md` sections 5b-5d). On RTX 30 series the sm_89->sm_86 route is
field-confirmed on 61x drivers; 616.92 is the recommended 61x build (it carries
all current Smooth Motion fixes). Games launched **through the Steam client**
may crash at start with any proxy-injected SM unlock - launching the game `.exe`
directly works (see docs section 5d).

Do not use this in competitive or anti-cheat-protected games. Keep a backup of
every replaced file. A driver reset, game crash, corrupted frame, or black
screen remains possible.

## Building

Requirements:

- 64-bit Windows 10 or 11;
- Visual Studio 2022 with **Desktop development with C++**;
- Windows 10/11 SDK;
- CMake 3.24 or newer.

Run:

```bat
build_release.bat
```

The result is `build\Release\version.dll`. Copy it beside the game's main
executable. Do not copy `NvPresent64.dll`; NVSmooth30 loads the installed driver
copy from DriverStore.

### Continuous builds (no local toolchain required)

The `CI build` GitHub Actions workflow (`windows-latest`) compiles
`version.dll` + `nvs30_probe.exe`, runs the portable fatbin unit tests and the
repository invariant checks, and uploads a ready-to-run zip as a build
artifact. A self-hosted workflow (`GPU validation (self-hosted RTX)`) is
provided as a template for validating on real RTX hardware.

## Configuration

Set environment variables before launching the game:

| Variable | Default | Purpose |
|---|---:|---|
| `SM86_ENABLE_OSD` | `0` | Draw the lightweight status OSD; F11 toggles it. |
| `SM86_ENABLE_D3D11_BRIDGE` | `1` | Enable the experimental D3D11-to-D3D12 bridge; set to `0` to disable it. |
| `SM86_FORCE_VSYNC` | `0` | Force swapchain sync interval 1. |
| `SM86_DIAGNOSTICS` | `0` | Reserve verbose diagnostic behavior. |
| `SM86_LOW_LATENCY` | `1` | Request maximum frame latency 1 when supported. |
| `SM86_HALF_REFRESH_CAP` | `0` | Pace base frames at half the active refresh rate. |
| `SM86_BASE_FPS_CAP` | `0` | Explicit base-frame cap; zero disables it. |
| `SM86_BRIDGE_LINEARIZE` | `0` | Map sRGB source formats to linear for the bridge; default keeps source format. |
| `SM86_NVPRESENT_PATH` | auto | Override the full path to `NvPresent64.dll`. |
| `SM75_FORCE_CUBIN_REWRITE` | `0` | **Experimental**: on SM75 only, relabel sm_89 fatbin entries to sm_75 (may TDR; see docs). |
| `SM75_ELF_STAMP` | `entryonly` | ELF word for the experiment: `entryonly`, `driver75` (0x05004B04), `mirror86` (0x06004B04). |
| `NVS30_ALLOW_CUDA_INIT` | `1` | Allow the one-shot `cuInit` capability query used for GPU detection. |
| `NVS30_FORCE_CC` | – | Pin the compute capability (`7.5`, `8.6`) for A/B testing. |
| `NVS30_CUDA_DEVICE` | auto | CUDA device index to interrogate on hybrid systems. |

Example launcher:

```bat
@echo off
set SM86_ENABLE_D3D11_BRIDGE=1
set SM86_ENABLE_OSD=1
start "" "Game.exe"
```

Diagnostics are written to `nvsmooth30.log` beside the game executable.

## Offline driver inspection

The regression inspector does not load or modify the DLL:

```bat
py tools\inspect_nvp.py "C:\path\to\NvPresent64.dll"
```

On RTX 20 series, the added `ptx_entries=` / `sm75:` verdict lines tell you
whether your driver build carries JIT-able PTX for the Smooth Motion kernels
(the supported Turing path) or only SASS-major-8 cubins (fail-closed; see
docs/SM75_PORT.md).

For the reference build it should find the validated `cmp [rcx+14h], 2/3` +
`SETGE SIL` capability structure, `NVP_Init_D3D`, and one writable configuration
target. Unknown layouts are intentionally unsupported until reviewed.

## Architecture

1. The version proxy loads the real Windows `version.dll` from System32.
2. The NvPresent module is located and inspected as a normal PE image.
3. The CUDA load path is intercepted through a named import or NvPresent's
   standard `GetProcAddress` import.
4. Only valid SM89 cubin entries are copied and retargeted to SM86; SM120 and
   unknown entries remain untouched.
5. Gate/config changes are applied as a transaction and restored on failure.
6. Shared DXGI vtables expose newly created game swapchains to the presentation
   path. D3D11 can optionally copy through a two-buffer D3D12 shadow swapchain.
7. The D3D11 bridge mirrors the reference shared-resource path: NT-handle sharing
   with a legacy SHARED fallback, D3D11 event-query synchronization, and validated
   NvPresent wrapper activation when the private wrapper object is discovered.

## Clean-room note

The project is organized from observed inputs, outputs, public PE/DXGI/D3D/CUDA
interfaces, and independently written code. It is not a variable-renamed
decompilation. The external binary that motivated the experiment is not
included and its authorship is not claimed.

## License

The independently written source in this archive is released under the MIT
License. NVIDIA, CUDA, DirectX, and Windows are trademarks of their respective
owners. No NVIDIA or third-party binaries are redistributed.
