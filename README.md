# QuickWiper

A tiny Windows tool that wipes an entire removable USB device and optionally reformats it to exFAT or NTFS. One native exe, no installer, runs elevated.

![QuickWiper](screenshot.png)

## Quick mode — what makes it different

A best-effort wipe you can **cancel at any time**. It destroys data in order of how recoverable it is — the filesystem map first, then file headers across the whole device, then the remaining contents — so when you don't have time for a full overwrite you can stop whenever and still have wiped as much as possible in the time you had. Left running, it converges to the same result as a full wipe.

> Permanently destroys everything on the selected device — no undo. Only removable USB disks can be targeted; the system disk and fixed disks are always blocked.

## Download

Grab the latest `QuickWiper.exe` from the [Releases](https://github.com/Dissimilis/QuickWiper/releases) page.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File native\build.ps1
```
