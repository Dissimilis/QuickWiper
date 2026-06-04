// QuickWiper core: disk enumeration, the safety guard, RNG, pass planner, wipe
// engine, and reformatter. No UI here so the CLI and GUI share identical logic.
#pragma once
#define WINVER 0x0A00
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <set>
#include <functional>
#include <atomic>

namespace qw {

struct DiskInfo {
    int index = -1;
    std::string model = "(unknown)";   // ASCII from the device descriptor
    std::string serial;
    long long sizeBytes = 0;
    int sectorSize = 512;
    STORAGE_BUS_TYPE busType = BusTypeUnknown;
    bool removable = false;
    bool queryFailed = false;
    std::vector<wchar_t> letters;       // drive letters of volumes on this disk

    std::string sizeDisplay() const;
    std::string lettersDisplay() const;
    static std::string formatSize(long long bytes);
};

struct GuardResult {
    bool wipeable = false;
    std::string reason;
};

// The single decision point: provably-removable-USB only (or, in test mode, a
// file-backed VHD), and never a protected/system disk. Fail-closed.
GuardResult Evaluate(const DiskInfo& d, const std::set<int>& protectedDisks, bool allowVirtual);

// Enumerate all physical disks with the properties the guard needs.
std::vector<DiskInfo> Enumerate();

// Disk numbers that must never be wiped (Windows/system disk, pagefile disks).
std::set<int> GetProtectedDiskNumbers();

enum class Mode { Full, Quick };
enum class Outcome { Completed, Cancelled, TimedOut };
enum class FsChoice { None, ExFat, Ntfs };

struct WriteOp { std::string pass; long long offset; long long length; };
std::vector<WriteOp> Plan(long long total, Mode mode);

struct Progress {
    std::string pass;
    long long written = 0;
    long long total = 0;
    double mbPerSec = 0;
    double etaSeconds = 0;
    double fraction() const { return total <= 0 ? 0.0 : (double)written / (double)total; }
};

using ProgressFn = std::function<void(const Progress&)>;

// High-entropy fast PRNG (xorshift128+) seeded from the system CSPRNG and re-keyed
// periodically; defeats controller compression/dedup while staying faster than the disk.
class RandomSource {
public:
    RandomSource();
    void fill(unsigned char* buf, size_t len);
private:
    void rekey();
    unsigned long long s0_ = 0, s1_ = 0;
    long long sinceRekey_ = 0;
};

// Execute a plan against the disk. timeBoxMs < 0 means no time box.
Outcome RunWipe(const DiskInfo& disk, Mode mode, const ProgressFn& progress,
                std::atomic<bool>& cancel, long long timeBoxMs, std::string& err);

struct ReformatResult { bool success; std::string output; };
ReformatResult Reformat(int diskIndex, FsChoice fs, const std::string& label = "QUICKWIPER");

// Helpers shared with the UI.
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);
const char* BusName(STORAGE_BUS_TYPE t);

} // namespace qw
