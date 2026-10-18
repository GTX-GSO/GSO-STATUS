#!/usr/bin/env python3
"""Checks, without compiling firmware, that the Windows simulator and the ESP32-P4 firmware share one code base.

Rules:
  * every shared/*.c is listed in main/CMakeLists.txt;
  * every simulator/*.c is either listed there (shared with the firmware) or named in SIMULATOR_ONLY below
    (a Windows platform adapter with a firmware counterpart in main/);
  * the firmware compiles the same UI entry (simulator/ui.c) the simulator is built from;
  * the simulator build compiles every shared/*.c (scripts/build.ps1 globs shared/*.c);
  * a new file that is in neither list fails, forcing an explicit decision instead of silent divergence.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Windows-only platform adapters and the firmware file that replaces each one.
SIMULATOR_ONLY = {
    "main.c": "main/main.c (entry, board init)",
    "data_source.c": "main/gss_data.c (GSN polling; both use shared/gso_link.c)",
    "home_client.c": "main/gss_data.c (HOME commands)",
    "disk_data.c": "firmware has no local disks",
    "display_brightness.c": "main/gss_preferences.c (preferences, brightness output)",
    "wifi_preview.c": "main/gss_wifi_radio.c, gss_wifi_storage.c",
    "system_preview.c": "main/gss_system.c (chip statistics and accelerometer)",
}


def main() -> int:
    cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")
    listed = {pathlib.PurePosixPath(m).as_posix() for m in re.findall(r'"([^"]+\.c)"', cmake)}
    problems = []

    for path in sorted((ROOT / "shared").glob("*.c")):
        if f"../shared/{path.name}" not in listed:
            problems.append(f"shared/{path.name} is not compiled into the firmware (main/CMakeLists.txt)")

    for path in sorted((ROOT / "simulator").glob("*.c")):
        in_firmware = f"../simulator/{path.name}" in listed
        if in_firmware and path.name in SIMULATOR_ONLY:
            problems.append(f"simulator/{path.name} is listed as simulator-only but the firmware also compiles it")
        if not in_firmware and path.name not in SIMULATOR_ONLY:
            problems.append(
                f"simulator/{path.name} is in neither the firmware sources nor SIMULATOR_ONLY; "
                "add it to main/CMakeLists.txt or declare it a platform adapter in scripts/verify-shared-code.py")

    for name in sorted(SIMULATOR_ONLY):
        if not (ROOT / "simulator" / name).exists():
            problems.append(f"SIMULATOR_ONLY names simulator/{name} which no longer exists")

    if "../simulator/ui.c" not in listed:
        problems.append("the firmware does not compile simulator/ui.c, the shared UI entry")

    build = (ROOT / "scripts" / "build.ps1").read_text(encoding="utf-8")
    if "'shared'" not in build or "Filter '*.c'" not in build:
        problems.append("scripts/build.ps1 no longer compiles shared/*.c into the simulator")

    for line in problems:
        print("FAIL:", line)
    if problems:
        return 1
    shared = len(list((ROOT / "shared").glob("*.c")))
    common = len([p for p in (ROOT / "simulator").glob("*.c") if p.name not in SIMULATOR_ONLY])
    print(f"PASS: simulator and firmware share {shared} shared/*.c and {common} simulator/*.c files; "
          f"{len(SIMULATOR_ONLY)} Windows adapters are declared")
    return 0


if __name__ == "__main__":
    sys.exit(main())
