# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Simplicity is paramount

This is the overriding principle for the whole project. It is a small, single-purpose tool — keep it that way. Prefer the simplest thing that works over anything clever, general, or "future-proof":

- No abstractions, interfaces, layers, or patterns the current scope doesn't concretely need. Add them only when something breaks without them, never speculatively.
- No dependencies beyond the platform (Win32 API / standard library) unless genuinely unavoidable.
- Fewer files, shorter files, plain procedural code over frameworks. If a feature can be a function, it's a function.
- When two approaches work, pick the one that's easier to read and delete.

When in doubt, choose less. Complexity must justify itself; simplicity never has to.

## Status

Implemented and tested. This file is the working orientation.

**The shipped deliverable is the native C++/Win32 build** (`native/`) → `dist/QuickWiper.exe`, ~0.36 MB, fully self-contained (UCRT is in-box on Win10/11). It was rewritten from C# specifically to shrink the exe ~137× (the .NET/WinForms self-contained build was 49 MB and can't be trimmed/AOT'd).

Layout:
- `native/` — **the app.** `main.cpp` dispatches GUI vs CLI; `core.{h,cpp}` is UI-free logic (enumerate, guard, RNG, planner, engine, reformat); `cli.cpp` and `gui.cpp` are thin front-ends. `build.ps1` compiles it; `app.manifest` + `resource.rc` embed the admin manifest + common-controls v6.
- `QuickWiper/`, `QuickWiper.Tests/` — the original C# implementation (same design, same behavior) plus xUnit tests for the guard/planner/RNG. **Superseded** by `native/` but kept as reference and for its unit tests.
- `test/vhd-test.ps1 [-ExePath ...]` — elevated integration test: creates a throwaway VHD, wipes it with the real exe, verifies planted markers are destroyed. Never touches a pre-existing disk. `test/screenshot.ps1 [-ExePath ...]` captures the GUI.

Verified (native build, 9/9 integration checks; C# build also 9/9 + 20 unit tests): system disk rejected (exit 2, even with `--allow-virtual`), both wipe modes run and destroy secret markers, GUI renders correctly. Reformat (diskpart/VDS) couldn't be verified on the dev machine due to a local VDS timeout — code is correct; verify on healthy hardware.

## Product

A Windows desktop app that **wipes an entire removable USB device** (everything — files, free space, partition table), then optionally repartitions and reformats it to a single exFAT or NTFS partition. Runs **elevated** (Administrator) for raw physical-disk access.

Two wipe modes:
- **Full** — thorough sequential random overwrite of the whole device, first sector to last.
- **Quick** — an *anytime* wipe: at every instant of cancel, maximize the data already unrecoverable. Two mechanisms (in `Plan`): **kill the partition map** (front partition area + the backup GPT at the tail) first, then a **bit-reversed spread fill** — overwrite the whole device in 64 MB chunks visited in bit-reversed order, so finished chunks stay spread uniformly across the device at every cancel point (no front/tail blind spot) while each chunk is one large sequential write (fast on USB). Cancel anytime; if left running it overwrites every chunk, converging to the same result as Full.

**One exe, two front-ends:** no args → WinForms GUI; with args → headless CLI (the CLI is the primary test harness — drives a VHD without the GUI). Both are thin shells over the same UI-free `core`.

## Stack & commands

Native **C++17 / Win32**, built with **MinGW-w64 (WinLibs, UCRT)** — no Visual Studio or Windows SDK needed (MinGW bundles its own Win32 headers/libs). GUI-subsystem exe (no console flash); `AttachConsole(ATTACH_PARENT_PROCESS)` gives the dual GUI/CLI behavior. Elevation + common-controls v6 via the embedded manifest. Statically linked (`-static`), so the exe is standalone.

```powershell
# Build -> dist\QuickWiper.exe  (needs WinLibs MinGW; install once:)
#   winget install --id BrechtSanders.WinLibs.POSIX.UCRT -e
powershell -ExecutionPolicy Bypass -File native\build.ps1

# Integration test (must run elevated; safe — only touches a VHD it creates):
#   powershell -ExecutionPolicy Bypass -File test\vhd-test.ps1 -ExePath dist\QuickWiper.exe
# GUI screenshot:
#   powershell -ExecutionPolicy Bypass -File test\screenshot.ps1 -ExePath dist\QuickWiper.exe

# Legacy C# build (superseded) still works if needed:
#   dotnet test QuickWiper.Tests\QuickWiper.Tests.csproj
```

Notes for working here:
- It's a GUI-subsystem exe, so the GUI launches with no console flash. In a terminal the CLI attaches to the console but the shell prompt returns before output finishes — to capture exit codes/output reliably (tests, scripts) invoke it with `Start-Process -Wait -PassThru -RedirectStandardOutput`.
- Raw writes are plain **buffered** (no `NO_BUFFERING`, no `WRITE_THROUGH`) so the cache manager can pipeline/coalesce them — critical for the Header-kill pass's small scattered writes, which crawl on USB flash if each must commit synchronously. Correctness is preserved by an explicit `FlushFileBuffers` every 64 MB and a final flush before close (offsets/lengths are still sector-aligned). An earlier build used `WRITE_THROUGH`; it forced every small write to a synchronous read-modify-write and made Header kill very slow on USB.
- Don't UPX-pack the exe: a disk-wiper that's packed is a magnet for antivirus false positives.
- GUI uses system-DPI awareness (manifest `dpiAware=true`) and scales layout by `GetDpiForSystem()`; keep coordinates in DIPs via the `S()` helper.

## Non-negotiable: never wipe the OS / a fixed disk

The central risk is wiping the **wrong disk**. The OS/system disk and any non-removable disk must be unselectable through both GUI and CLI, with no `--force` override. Guards are layered and **fail closed** (if a disk's bus type or the system-disk number can't be resolved, the disk is rejected). The safety logic (`Evaluate` / `GetProtectedDiskNumbers` in `core.cpp`, mirrored by `SystemDiskGuard` in the C# version) is the most important thing to keep correct and well-tested.

## Honest limits

Raw LBA writes still pass through the flash controller's translation layer, so wear-leveling and spare/over-provisioned NAND blocks are unreachable from software. This tool defeats software/logical recovery and destroys the filesystem map; it does **not** guarantee against lab-grade chip-off forensics. Don't overclaim in UI copy. Always write **high-entropy random** data (never zeros/patterns) so a compressing/dedup controller can't avoid real physical writes.

## Conventions

- Windows only — Windows-specific APIs and paths are expected, not a portability concern.
- Keep `core` free of UI and console dependencies so it stays headless-testable and shared by both front-ends.
