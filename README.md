# QuickWiper

A tiny Windows tool that wipes an entire removable USB device and optionally reformats it to exFAT or NTFS. One native exe, no installer, runs elevated.

![QuickWiper](screenshot.png)

## Wipe modes

All modes write **high-entropy random data** (never zeros) and converge to a complete overwrite if left to finish. They differ in how the work is *ordered*, which is what matters when you cancel early.

- **Quick 2 (recommended)** — an *anytime* wipe you can **cancel at any time**. It destroys the filesystem map first, then overwrites the whole device in small, evenly-spread blocks visited in a bit-reversed order. Because the overwritten regions stay uniformly scattered at fine granularity, within seconds almost no file is left contiguously intact for a recovery tool to carve back — no matter where on the device it sat. The longer it runs, the denser the coverage, up to a full overwrite.
- **Quick** — the original anytime wipe. Same idea, but it overwrites in large 64 MB blocks. Fastest to *complete*, but until a given block is reached its whole 64 MB stays intact, so an early cancel can leave large recoverable regions. Prefer Quick 2 unless you are letting the wipe run all the way to completion.
- **Full** — one thorough sequential overwrite from the first sector to the last. Simplest and most predictable; no early-cancel benefit.

> Permanently destroys everything on the selected device — no undo. Only removable USB disks can be targeted; the system disk and fixed disks are always blocked.

## Download

Grab the latest `QuickWiper.exe` from the [Releases](https://github.com/Dissimilis/QuickWiper/releases) page.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File native\build.ps1
```
