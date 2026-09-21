#!/usr/bin/env python3
"""Monthly Turing PTX watchdog (CI-side, GitHub-hosted).

Scans NVIDIA's us.download.nvidia.com CDN for Game Ready driver versions
newer than the last known baseline, downloads the newest, extracts only
Display.Driver/NvPresent64.dll, and runs tools/inspect_nvp.py against it.

If the fatbins of that driver carry PTX (ptx_entries>0), the Smooth Motion
kernels become JIT-able on sm_75 and the NVSmooth30 Turing path flips from
fail-closed to functional without any code change - so the workflow opens an
issue.  If PTX is absent (every driver so far: 616.92 live + 595.71 offline
both measured ptx=0) the run just appends a line to the step summary.

Stdlib only; designed to never hard-fail on a schedule.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
from pathlib import Path

BASE = "https://us.download.nvidia.com/Windows/{ver}/{ver}-desktop-win10-win11-64bit-international-dch-whql.exe"
SEVENZIP_PATHS = [
    r"C:\Program Files\7-Zip\7z.exe",
    r"C:\Program Files (x86)\7-Zip\7z.exe",
]


def key(v: str) -> tuple[int, int]:
    m = re.fullmatch(r"(\d+)\.(\d+)", v)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def head(url: str, timeout: int = 15) -> bool:
    import urllib.request

    try:
        req = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return 200 <= resp.status < 400
    except Exception:
        return False


def download(url: str, dst: Path, timeout: int = 1200) -> bool:
    import urllib.request

    try:
        with urllib.request.urlopen(url, timeout=timeout) as resp, dst.open("wb") as out:
            while True:
                chunk = resp.read(1 << 20)
                if not chunk:
                    break
                out.write(chunk)
        return dst.stat().st_size > 100_000_000
    except Exception as exc:  # noqa: BLE001 - scheduled job must degrade quietly
        print(f"download failed: {exc}")
        return False


def sevenzip() -> str:
    for p in SEVENZIP_PATHS:
        if Path(p).exists():
            return p
    raise RuntimeError("7-Zip not found on runner")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", default="616.92", help="last driver already inspected (excluded)")
    ap.add_argument("--from-series", type=int, default=616)
    ap.add_argument("--to-series", type=int, default=624)
    ap.add_argument("--version", default="", help="explicit driver version to probe (skips scan)")
    ap.add_argument("--work", required=True, help="scratch dir")
    ap.add_argument("--state", required=True, help="dir for ptx-found.flag")
    args = ap.parse_args()

    work, state = Path(args.work), Path(args.state)
    work.mkdir(parents=True, exist_ok=True)
    state.mkdir(parents=True, exist_ok=True)
    flag = state / "ptx-found.flag"
    if flag.exists():  # one-shot until a human clears it
        print("flag already present; skipping")
        return 0

    summary = Path(os.environ.get("GITHUB_STEP_SUMMARY", str(state / "summary.md")))
    note = lambda line: summary.open("a", encoding="utf-8").write(line + "\n")

    if args.version:
        candidates = [args.version]
    else:
        urls = {
            v: BASE.format(ver=v)
            for series in range(args.from_series, args.to_series + 1)
            for v in (f"{series}.{minor}" for minor in range(0, 100))
            if key(v) > key(args.baseline)
        }
        with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
            hits = [v for v, ok in zip(urls, pool.map(lambda u: head(u), urls.values())) if ok]
        candidates = sorted(hits, key=key)
        note(f"### PTX watchdog probe\n- reachable versions above `{args.baseline}`: {', '.join(candidates) or 'none'}")
        if not candidates:
            note("- nothing newer published/reachable; baseline stands (this includes CDN refusing datacenter IPs)")
            return 0

    verdict = f"no newer driver candidate above {args.baseline}"
    for ver in reversed(candidates):
        setup = work / f"setup-{ver}.exe"
        if not download(BASE.format(ver=ver), setup):
            verdict = f"driver {ver}: download blocked/failed"
            continue
        dll = work / "NvPresent64.dll"
        sz = sevenzip()
        probe = subprocess.run([sz, "l", str(setup), "-y"], capture_output=True, text=True)
        names = [ln.split()[-1] for ln in probe.stdout.splitlines() if "nvpresent64.dll" in ln.lower()]
        if not names:
            verdict = f"driver {ver}: NvPresent64.dll not in package"
            break
        subprocess.run([sz, "e", str(setup), "-o" + str(work), names[-1], "-y"], check=True, capture_output=True)
        if not dll.exists():
            verdict = f"driver {ver}: extraction failed"
            break
        insp = subprocess.run(
            [sys.executable, str(Path(__file__).parent / "inspect_nvp.py"), str(dll)],
            capture_output=True, text=True,
        )
        out = insp.stdout
        ptx = int(re.search(r"ptx_entries=(\d+)", out).group(1)) if re.search(r"ptx_entries=(\d+)", out) else -1
        gates = int(re.search(r"gate_candidates=(\d+)", out).group(1)) if re.search(r"gate_candidates=(\d+)", out) else -1
        verdict = f"driver {ver}: ptx_entries={ptx} gate_candidates={gates}"
        note("- " + verdict)
        if ptx > 0:
            flag.write_text(f"{ver}\n{out}\n", encoding="utf-8")
            note(f"- **PTX FOUND in {ver}** - Turing SM unlock becomes JIT-viable; issue will be opened")
        break  # newest candidate only; older ones already known fail-closed
    if not args.version and verdict.startswith("driver"):
        pass
    print(verdict)
    if not args.version:
        # candidate loop above already noted per-version results when download worked
        pass
    if "download blocked" in verdict:
        note("- " + verdict + " (CDN may refuse this runner; retry from a residential line if this repeats)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
