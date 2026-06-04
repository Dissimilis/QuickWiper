# QuickWiper

A tiny Windows tool that **wipes an entire removable USB device** and optionally reformats it. One native exe (~0.36 MB), no installer, no runtime dependency.

> **Warning:** QuickWiper permanently destroys everything on the selected device. There is no undo. It can only target **removable USB disks** — the system disk and any fixed/internal disk are always blocked.

## Features

- **Two wipe modes**
  - **Full** — thorough sequential random overwrite of the whole device, first sector to last.
  - **Quick** — a progressive, *cancel-anytime* wipe ordered by recovery-value-per-byte: destroy the filesystem map → kill file headers across the device → fill the rest. Stop whenever; if left running it converges to the same result as Full.
- **Optional reformat** to a single exFAT or NTFS partition after wiping.
- **GUI and CLI in one exe** — double-click for the GUI, or pass arguments for a headless/scriptable CLI.
- **High-entropy random data** so a flash controller can't compress or deduplicate the writes away.

## Safety

The only real danger is wiping the wrong disk, so the guard is strict and **fails closed**:

- Only disks that are **USB *and* removable** can be selected.
- The Windows/system disk, boot disk, and any pagefile disk are resolved and **permanently excluded** — through both the GUI and the CLI, with no override flag.
- A typed `ERASE` confirmation (GUI) or explicit `--yes` (CLI) is required, and the target is re-verified immediately before any write.

## Honest limits

QuickWiper writes through the OS to raw disk sectors. Those writes still pass through the flash controller's translation layer, so **wear-leveling and spare/over-provisioned NAND blocks are beyond the reach of any software**. It reliably defeats software/logical file-recovery tools and destroys the filesystem; it does **not** guarantee against lab-grade chip-off forensics.

## Usage

GUI: run `QuickWiper.exe` (it requests Administrator). Pick the device, choose a mode and optional reformat, type `ERASE` to confirm.

CLI:

```
QuickWiper list
QuickWiper wipe --disk N --mode full|quick [--seconds S] [--fs exfat|ntfs] --yes
QuickWiper format --disk N --fs exfat|ntfs --yes
```

Exit codes: `0` ok, `1` usage, `2` guard-rejected, `3` not-found, `4` lock-failed, `5` write-error, `6` cancelled.

## Build

Native C++17 / Win32, built with MinGW-w64 (no Visual Studio or Windows SDK needed):

```powershell
winget install --id BrechtSanders.WinLibs.POSIX.UCRT -e   # once
powershell -ExecutionPolicy Bypass -File native\build.ps1  # -> dist\QuickWiper.exe
```

## Testing

`test\vhd-test.ps1` runs an elevated integration test that creates a throwaway VHD, wipes it with the real exe, and verifies planted markers are destroyed — it never touches a pre-existing disk. Run from an elevated PowerShell:

```powershell
powershell -ExecutionPolicy Bypass -File test\vhd-test.ps1 -ExePath dist\QuickWiper.exe
```
