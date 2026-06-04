#include "core.h"
#include <bcrypt.h>
#include <cstdio>
#include <cstring>
#include <chrono>

#pragma comment(lib, "bcrypt.lib")

namespace qw {

// ---- small string helpers -------------------------------------------------

std::wstring Widen(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string a(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), a.data(), n, nullptr, nullptr);
    return a;
}

const char* BusName(STORAGE_BUS_TYPE t) {
    switch (t) {
        case BusTypeUsb: return "Usb";
        case BusTypeNvme: return "Nvme";
        case BusTypeSata: return "Sata";
        case BusTypeScsi: return "Scsi";
        case BusTypeAta: return "Ata";
        case BusTypeSas: return "Sas";
        case BusTypeRAID: return "Raid";
        case BusTypeSd: return "Sd";
        case BusTypeMmc: return "Mmc";
        case BusTypeVirtual: return "Virtual";
        case BusTypeFileBackedVirtual: return "FileBackedVirtual";
        default: return "Unknown";
    }
}

std::string DiskInfo::formatSize(long long bytes) {
    if (bytes <= 0) return "0 B";
    const char* u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes; int i = 0;
    while (v >= 1024 && i < 4) { v /= 1024; i++; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), (v < 10 ? "%.2f %s" : "%.1f %s"), v, u[i]);
    return buf;
}

std::string DiskInfo::sizeDisplay() const { return formatSize(sizeBytes); }

std::string DiskInfo::lettersDisplay() const {
    if (letters.empty()) return "-";
    std::string s;
    for (size_t i = 0; i < letters.size(); ++i) {
        if (i) s += ", ";
        s += (char)letters[i]; s += ':';
    }
    return s;
}

// ---- guard ----------------------------------------------------------------

GuardResult Evaluate(const DiskInfo& d, const std::set<int>& protectedDisks, bool allowVirtual) {
    if (d.queryFailed) return { false, "device could not be queried (rejected, fail-closed)" };
    if (protectedDisks.count(d.index)) return { false, "system / boot / pagefile disk - permanently protected" };
    if (d.busType == BusTypeUsb && d.removable) return { true, "removable USB device" };
    if (allowVirtual && d.busType == BusTypeFileBackedVirtual) return { true, "file-backed virtual disk (test mode)" };
    std::string r = "not a removable USB device (bus: ";
    r += BusName(d.busType); r += d.removable ? ", removable: yes)" : ", removable: no)";
    return { false, r };
}

// ---- low level disk access ------------------------------------------------

static HANDLE OpenDisk(int index, DWORD access) {
    wchar_t path[64];
    std::swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", index);
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, 0, nullptr);
}

static int DiskNumberForLetter(wchar_t letter) {
    wchar_t path[16];
    std::swprintf(path, 16, L"\\\\.\\%c:", letter);
    HANDLE h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    VOLUME_DISK_EXTENTS ext{};
    DWORD ret = 0;
    int disk = -1;
    if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                        &ext, sizeof(ext), &ret, nullptr) && ext.NumberOfDiskExtents >= 1)
        disk = (int)ext.Extents[0].DiskNumber;
    CloseHandle(h);
    return disk;
}

static std::vector<wchar_t> AllDriveLetters() {
    std::vector<wchar_t> v;
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i)) v.push_back((wchar_t)(L'A' + i));
    return v;
}

static std::string AsciiAt(const unsigned char* base, DWORD off, DWORD bufLen) {
    if (off == 0 || off >= bufLen) return "";
    const char* p = (const char*)base + off;
    std::string s;
    while (off < bufLen && *p) { s += *p++; off++; }
    // trim
    size_t a = s.find_first_not_of(" \t");
    size_t b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<DiskInfo> Enumerate() {
    // map letter -> disk
    std::vector<std::pair<wchar_t,int>> letterDisk;
    for (wchar_t c : AllDriveLetters()) {
        int dn = DiskNumberForLetter(c);
        if (dn >= 0) letterDisk.push_back({ c, dn });
    }

    std::vector<DiskInfo> disks;
    for (int i = 0; i < 64; ++i) {
        HANDLE h = OpenDisk(i, GENERIC_READ);
        if (h == INVALID_HANDLE_VALUE) continue;

        DiskInfo d;
        d.index = i;
        for (auto& ld : letterDisk) if (ld.second == i) d.letters.push_back(ld.first);

        // descriptor
        STORAGE_PROPERTY_QUERY q{};
        q.PropertyId = StorageDeviceProperty;
        q.QueryType = PropertyStandardQuery;
        unsigned char buf[1024]{};
        DWORD ret = 0;
        bool ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                                  buf, sizeof(buf), &ret, nullptr);
        if (ok) {
            auto* desc = (STORAGE_DEVICE_DESCRIPTOR*)buf;
            d.removable = desc->RemovableMedia != 0;
            d.busType = desc->BusType;
            std::string vendor = AsciiAt(buf, desc->VendorIdOffset, sizeof(buf));
            std::string product = AsciiAt(buf, desc->ProductIdOffset, sizeof(buf));
            d.serial = AsciiAt(buf, desc->SerialNumberOffset, sizeof(buf));
            std::string m = vendor.empty() ? product : (product.empty() ? vendor : vendor + " " + product);
            if (!m.empty()) d.model = m;
        }

        // length
        GET_LENGTH_INFORMATION li{};
        if (DeviceIoControl(h, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &li, sizeof(li), &ret, nullptr))
            d.sizeBytes = li.Length.QuadPart;
        else
            d.queryFailed = !ok ? true : d.queryFailed;
        if (!ok) d.queryFailed = true;

        // sector size
        DISK_GEOMETRY_EX geo{};
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geo, sizeof(geo), &ret, nullptr)) {
            int s = (int)geo.Geometry.BytesPerSector;
            if (s == 512 || s == 1024 || s == 2048 || s == 4096) d.sectorSize = s;
        }

        CloseHandle(h);
        disks.push_back(d);
    }
    return disks;
}

static int RootLetterDisk(const wchar_t* path) {
    if (!path || !path[0] || path[1] != L':') return -1;
    wchar_t c = towupper(path[0]);
    if (c < L'A' || c > L'Z') return -1;
    return DiskNumberForLetter(c);
}

std::set<int> GetProtectedDiskNumbers() {
    std::set<int> prot;
    auto add = [&](int dn) { if (dn >= 0) prot.insert(dn); };

    wchar_t win[MAX_PATH]; if (GetWindowsDirectoryW(win, MAX_PATH)) add(RootLetterDisk(win));
    wchar_t sys[MAX_PATH]; if (GetSystemDirectoryW(sys, MAX_PATH)) add(RootLetterDisk(sys));
    wchar_t exe[MAX_PATH]; if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) add(RootLetterDisk(exe));

    // pagefiles from the registry (REG_MULTI_SZ "PagingFiles")
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
            L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management",
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        DWORD type = 0, sz = 0;
        if (RegQueryValueExW(k, L"PagingFiles", nullptr, &type, nullptr, &sz) == ERROR_SUCCESS && sz > 0) {
            std::wstring data(sz / sizeof(wchar_t), L'\0');
            if (RegQueryValueExW(k, L"PagingFiles", nullptr, &type, (LPBYTE)data.data(), &sz) == ERROR_SUCCESS) {
                const wchar_t* p = data.c_str();
                while (*p) { add(RootLetterDisk(p)); p += wcslen(p) + 1; }
            }
        }
        RegCloseKey(k);
    }
    return prot;
}

// ---- RNG ------------------------------------------------------------------

RandomSource::RandomSource() { rekey(); }

void RandomSource::rekey() {
    unsigned char seed[16];
    if (BCryptGenRandom(nullptr, seed, sizeof(seed), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // extremely unlikely; fall back to a time-derived seed
        long long t = (long long)GetTickCount64();
        std::memcpy(seed, &t, 8); std::memcpy(seed + 8, &t, 8);
    }
    std::memcpy(&s0_, seed, 8);
    std::memcpy(&s1_, seed + 8, 8);
    if (s0_ == 0 && s1_ == 0) s1_ = 0x9E3779B97F4A7C15ULL;
    sinceRekey_ = 0;
}

void RandomSource::fill(unsigned char* buf, size_t len) {
    const long long rekeyEvery = 256LL * 1024 * 1024;
    if (sinceRekey_ >= rekeyEvery) rekey();
    size_t i = 0;
    for (; i + 8 <= len; i += 8) {
        unsigned long long x = s0_, y = s1_;
        s0_ = y; x ^= x << 23;
        s1_ = x ^ y ^ (x >> 17) ^ (y >> 26);
        unsigned long long r = s1_ + y;
        std::memcpy(buf + i, &r, 8);
    }
    if (i < len) {
        unsigned long long x = s0_, y = s1_;
        s0_ = y; x ^= x << 23;
        s1_ = x ^ y ^ (x >> 17) ^ (y >> 26);
        unsigned long long r = s1_ + y;
        std::memcpy(buf + i, &r, len - i);
    }
    sinceRekey_ += (long long)len;
}

// ---- pass planner ---------------------------------------------------------

std::vector<WriteOp> Plan(long long total, Mode mode) {
    const long long NukeSize = 64LL * 1024 * 1024;
    const long long HeaderStride = 16LL * 1024 * 1024;
    const long long HeaderBlock = 256LL * 1024;
    const char* PassNuke = "Nuke filesystem map";
    const char* PassHeader = "Header kill";
    const char* PassFull = "Content fill";

    std::vector<WriteOp> ops;
    if (total <= 0) return ops;

    if (mode == Mode::Quick) {
        long long headLen = total < NukeSize ? total : NukeSize;
        ops.push_back({ PassNuke, 0, headLen });
        long long tailStart = (total - NukeSize) > headLen ? (total - NukeSize) : headLen;
        if (tailStart < total) ops.push_back({ PassNuke, tailStart, total - tailStart });
        for (long long off = headLen; off + HeaderBlock <= tailStart; off += HeaderStride)
            ops.push_back({ PassHeader, off, HeaderBlock });
    }
    ops.push_back({ PassFull, 0, total }); // present in both modes, last => Quick converges to Full
    return ops;
}

// ---- wipe engine ----------------------------------------------------------

static HANDLE OpenForWrite(const DiskInfo& disk, std::vector<HANDLE>& locks, std::string& err) {
    for (wchar_t letter : disk.letters) {
        wchar_t path[16]; std::swprintf(path, 16, L"\\\\.\\%c:", letter);
        HANDLE vol = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (vol == INVALID_HANDLE_VALUE) { err = "Could not open a volume on the disk."; return INVALID_HANDLE_VALUE; }
        locks.push_back(vol);
        DWORD ret = 0;
        if (!DeviceIoControl(vol, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &ret, nullptr)) {
            char b[160]; std::snprintf(b, sizeof(b),
                "Could not lock volume %c: - it is in use. Close programs using it and retry.", (char)letter);
            err = b; return INVALID_HANDLE_VALUE;
        }
        DeviceIoControl(vol, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &ret, nullptr);
    }
    wchar_t path[64]; std::swprintf(path, 64, L"\\\\.\\PhysicalDrive%d", disk.index);
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) err = "Could not open the physical drive for writing.";
    return h;
}

Outcome RunWipe(const DiskInfo& disk, Mode mode, const ProgressFn& progress,
                std::atomic<bool>& cancel, long long timeBoxMs, std::string& err) {
    std::vector<HANDLE> locks;
    HANDLE h = OpenForWrite(disk, locks, err);
    if (h == INVALID_HANDLE_VALUE) {
        for (HANDLE l : locks) CloseHandle(l);
        return Outcome::Cancelled; // caller distinguishes via non-empty err
    }

    const int BlockSize = 4 * 1024 * 1024;
    std::vector<unsigned char> buffer(BlockSize);
    RandomSource rng;

    auto plan = Plan(disk.sizeBytes, mode);
    long long totalPlanned = 0; for (auto& op : plan) totalPlanned += op.length;
    long long writtenTotal = 0;

    Outcome result = Outcome::Completed;
    auto start = std::chrono::steady_clock::now();

    for (auto& op : plan) {
        long long pos = op.offset, end = op.offset + op.length;
        while (pos < end) {
            double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            if (cancel.load()) { result = Outcome::Cancelled; goto done; }
            if (timeBoxMs >= 0 && elapsed * 1000.0 >= (double)timeBoxMs) { result = Outcome::TimedOut; goto done; }

            int count = (int)((end - pos) < BlockSize ? (end - pos) : BlockSize);
            rng.fill(buffer.data(), count);

            LARGE_INTEGER li; li.QuadPart = pos;
            if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) { err = "Seek failed."; result = Outcome::Cancelled; goto done; }
            DWORD written = 0;
            if (!WriteFile(h, buffer.data(), (DWORD)count, &written, nullptr) || (int)written != count) {
                err = "Write failed (device removed or write-protected?)."; result = Outcome::Cancelled; goto done;
            }

            pos += count; writtenTotal += count;
            if (progress) {
                Progress p;
                p.pass = op.pass; p.written = writtenTotal; p.total = totalPlanned;
                p.mbPerSec = elapsed > 0 ? (writtenTotal / 1048576.0) / elapsed : 0;
                p.etaSeconds = p.mbPerSec > 0 ? ((totalPlanned - writtenTotal) / 1048576.0) / p.mbPerSec : 0;
                progress(p);
            }
        }
        FlushFileBuffers(h);
    }
done:
    FlushFileBuffers(h);
    CloseHandle(h);
    for (HANDLE l : locks) CloseHandle(l);
    return result;
}

// ---- reformat (diskpart) --------------------------------------------------

ReformatResult Reformat(int diskIndex, FsChoice fs, const std::string& label) {
    if (fs == FsChoice::None) return { true, "No reformat requested." };
    const char* fsName = fs == FsChoice::ExFat ? "exfat" : "ntfs";

    char tmpDir[MAX_PATH]; GetTempPathA(MAX_PATH, tmpDir);
    char script[MAX_PATH]; std::snprintf(script, sizeof(script), "%sqw-%d-%lu.txt", tmpDir, diskIndex, GetTickCount());

    {
        FILE* f = std::fopen(script, "w");
        if (!f) return { false, "Could not write diskpart script." };
        std::fprintf(f,
            "select disk %d\r\nclean\r\nconvert mbr\r\ncreate partition primary\r\n"
            "format fs=%s quick label=%s\r\nassign\r\nexit\r\n",
            diskIndex, fsName, label.c_str());
        std::fclose(f);
    }

    // Run diskpart with redirected stdout via a pipe.
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    std::string cmd = "diskpart.exe /s \""; cmd += script; cmd += "\"";
    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = wr; si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string output;
    bool ok = false;

    std::vector<char> cmdline(cmd.begin(), cmd.end()); cmdline.push_back('\0');
    if (CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                       nullptr, nullptr, &si, &pi)) {
        CloseHandle(wr); wr = nullptr;
        char buf[512]; DWORD n = 0;
        while (ReadFile(rd, buf, sizeof(buf), &n, nullptr) && n > 0) output.append(buf, n);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
        ok = (code == 0);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    } else {
        output = "Could not start diskpart.";
    }
    if (wr) CloseHandle(wr);
    CloseHandle(rd);
    DeleteFileA(script);
    return { ok, output };
}

} // namespace qw
