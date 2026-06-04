#include "core.h"
#include <string>
#include <cstring>
#include <cstdio>
#include <atomic>

// Headless front-end. Honors every guard: system/fixed disks can never be wiped,
// and no flag overrides that. --allow-virtual only relaxes the removable-USB rule
// for file-backed VHDs (testing).

using namespace qw;

namespace {

// Exit codes (asserted by test scripts).
enum { OK = 0, USAGE = 1, GUARD = 2, NOTFOUND = 3, LOCKFAIL = 4, WRITEERR = 5, CANCELLED = 6 };

void W(HANDLE h, const std::string& s) { DWORD n; WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr); }
void Out(const std::string& s) { W(GetStdHandle(STD_OUTPUT_HANDLE), s); }
void Err(const std::string& s) { W(GetStdHandle(STD_ERROR_HANDLE), "ERROR: " + s + "\r\n"); }

struct Options {
    int disk = -1;
    Mode mode = Mode::Quick;
    long long seconds = -1;
    bool yes = false;
    bool allowVirtual = false;
    FsChoice fs = FsChoice::None;
};

std::string trunc(const std::string& s, size_t n) {
    return s.size() <= n ? s : s.substr(0, n - 1) + "~";
}
std::string pad(const std::string& s, size_t n) {
    return s.size() >= n ? s : s + std::string(n - s.size(), ' ');
}

int Usage() {
    Out(
"QuickWiper - wipe an entire removable USB device.\r\n\r\n"
"Usage:\r\n"
"  QuickWiper                                  Launch the GUI.\r\n"
"  QuickWiper list [--allow-virtual]           List disks and whether each may be wiped.\r\n"
"  QuickWiper wipe --disk N --mode full|quick [--seconds S] [--fs exfat|ntfs] --yes [--allow-virtual]\r\n"
"  QuickWiper format --disk N --fs exfat|ntfs --yes [--allow-virtual]\r\n\r\n"
"Safety:\r\n"
"  The system/boot/pagefile disk and any non-removable disk can NEVER be wiped.\r\n"
"  --allow-virtual only relaxes the removable-USB rule for file-backed VHDs (testing).\r\n\r\n"
"Exit codes: 0 ok, 1 usage, 2 guard-rejected, 3 not-found, 4 lock-failed, 5 write-error, 6 cancelled.\r\n");
    return OK;
}

bool ParseFs(const char* v, FsChoice& out) {
    if (!_stricmp(v, "exfat")) out = FsChoice::ExFat;
    else if (!_stricmp(v, "ntfs")) out = FsChoice::Ntfs;
    else if (!_stricmp(v, "none")) out = FsChoice::None;
    else return false;
    return true;
}

bool ParseOptions(int argc, char** argv, int start, Options& o) {
    for (int i = start; i < argc; ++i) {
        const char* a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (!_stricmp(a, "--disk")) { const char* v = next(); if (!v) return false; o.disk = atoi(v); }
        else if (!_stricmp(a, "--mode")) { const char* v = next(); if (!v) return false; o.mode = !_stricmp(v, "full") ? Mode::Full : Mode::Quick; }
        else if (!_stricmp(a, "--seconds")) { const char* v = next(); if (!v) return false; o.seconds = atoll(v); }
        else if (!_stricmp(a, "--yes")) o.yes = true;
        else if (!_stricmp(a, "--allow-virtual")) o.allowVirtual = true;
        else if (!_stricmp(a, "--fs") || !_stricmp(a, "--format")) { const char* v = next(); if (!v || !ParseFs(v, o.fs)) return false; }
        else { Err(std::string("Unknown option '") + a + "'."); return false; }
    }
    return true;
}

int CmdList(const Options& o) {
    auto prot = GetProtectedDiskNumbers();
    auto disks = Enumerate();
    Out("\r\n  # | Bus              | Removable | Size        | Drives | Model                | Status\r\n");
    Out("----+------------------+-----------+-------------+--------+----------------------+--------------------------------\r\n");
    char num[8];
    for (auto& d : disks) {
        GuardResult g = Evaluate(d, prot, o.allowVirtual);
        std::snprintf(num, sizeof(num), "%2d", d.index);
        std::string line = std::string(" ") + num + " | " + pad(BusName(d.busType), 16) + " | "
            + pad(d.removable ? "yes" : "no", 9) + " | " + pad(d.sizeDisplay(), 11) + " | "
            + pad(d.lettersDisplay(), 6) + " | " + pad(trunc(d.model, 20), 20) + " | "
            + (g.wipeable ? "WIPEABLE: " : "blocked: ") + g.reason + "\r\n";
        Out(line);
    }
    Out("\r\n");
    return OK;
}

bool Resolve(int index, bool allowVirtual, DiskInfo& disk, GuardResult& guard, int& code) {
    auto prot = GetProtectedDiskNumbers();
    for (auto& d : Enumerate()) {
        if (d.index == index) { disk = d; guard = Evaluate(d, prot, allowVirtual); code = OK; return true; }
    }
    Err("no physical disk with index " + std::to_string(index) + ".");
    code = NOTFOUND;
    return false;
}

void PrintProgress(const Progress& p) {
    char buf[160];
    int es = (int)(p.etaSeconds + 0.5);
    std::snprintf(buf, sizeof(buf), "\r  [%-22s] %5.1f%%  %6.1f MB/s  ETA %02d:%02d:%02d      ",
        p.pass.c_str(), p.fraction() * 100.0, p.mbPerSec, es / 3600, (es / 60) % 60, es % 60);
    Out(buf);
}

std::atomic<bool>* g_cancel = nullptr;
BOOL WINAPI CtrlHandler(DWORD) { if (g_cancel) g_cancel->store(true); return TRUE; }

int CmdWipe(const Options& o) {
    if (o.disk < 0) { Err("wipe requires --disk N."); return USAGE; }
    DiskInfo disk; GuardResult guard; int code;
    if (!Resolve(o.disk, o.allowVirtual, disk, guard, code)) return code;
    if (!guard.wipeable) { Err("Disk " + std::to_string(o.disk) + " is not wipeable: " + guard.reason); return GUARD; }

    Out("Target: disk " + std::to_string(disk.index) + "  " + disk.model + "  " + disk.sizeDisplay()
        + "  [" + BusName(disk.busType) + "]  drives: " + disk.lettersDisplay() + "\r\n");
    Out(std::string("Mode:   ") + (o.mode == Mode::Full ? "Full" : "Quick")
        + (o.seconds >= 0 ? "  (time box: " + std::to_string(o.seconds) + "s)" : "") + "\r\n");
    Out("ALL DATA ON THIS DEVICE WILL BE DESTROYED.\r\n");
    if (!o.yes) { Err("Refusing without --yes (CLI wipe needs explicit --yes)."); return USAGE; }

    std::atomic<bool> cancel(false);
    g_cancel = &cancel;
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    std::string err;
    long long box = o.seconds >= 0 ? o.seconds * 1000 : -1;
    Outcome outcome = RunWipe(disk, o.mode, PrintProgress, cancel, box, err);
    Out("\r\n");
    if (!err.empty()) {
        Err(err);
        bool lock = err.find("lock") != std::string::npos;
        return lock ? LOCKFAIL : WRITEERR;
    }
    Out(std::string("Wipe ") + (outcome == Outcome::Completed ? "Completed" : outcome == Outcome::TimedOut ? "TimedOut" : "Cancelled") + ".\r\n");
    if (outcome == Outcome::Cancelled) return CANCELLED;

    if (o.fs != FsChoice::None) {
        Out(std::string("Reformatting as ") + (o.fs == FsChoice::ExFat ? "exFAT" : "NTFS") + "...\r\n");
        ReformatResult r = Reformat(disk.index, o.fs);
        Out(r.success ? "Reformat succeeded.\r\n" : "Reformat FAILED:\r\n" + r.output + "\r\n");
        if (!r.success) return WRITEERR;
    }
    return OK;
}

int CmdFormat(const Options& o) {
    if (o.disk < 0) { Err("format requires --disk N."); return USAGE; }
    if (o.fs == FsChoice::None) { Err("format requires --fs exfat|ntfs."); return USAGE; }
    DiskInfo disk; GuardResult guard; int code;
    if (!Resolve(o.disk, o.allowVirtual, disk, guard, code)) return code;
    if (!guard.wipeable) { Err("Disk " + std::to_string(o.disk) + " is not formattable: " + guard.reason); return GUARD; }
    if (!o.yes) { Err("Refusing without --yes."); return USAGE; }
    ReformatResult r = Reformat(disk.index, o.fs);
    Out(r.success ? "Reformat succeeded.\r\n" : "Reformat FAILED:\r\n" + r.output + "\r\n");
    return r.success ? OK : WRITEERR;
}

} // namespace

int RunCli(int argc, char** argv) {
    std::string cmd = argv[1];
    for (auto& c : cmd) c = (char)tolower(c);
    Options o;
    if (cmd == "list") { if (!ParseOptions(argc, argv, 2, o)) return USAGE; return CmdList(o); }
    if (cmd == "wipe") { if (!ParseOptions(argc, argv, 2, o)) return USAGE; return CmdWipe(o); }
    if (cmd == "format") { if (!ParseOptions(argc, argv, 2, o)) return USAGE; return CmdFormat(o); }
    if (cmd == "help" || cmd == "--help" || cmd == "-h") return Usage();
    Err("Unknown command '" + cmd + "'.");
    return USAGE;
}
