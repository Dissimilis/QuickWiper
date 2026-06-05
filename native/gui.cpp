#include "core.h"
#include <commctrl.h>
#include <string>
#include <vector>
#include <atomic>

#pragma comment(lib, "comctl32.lib")

using namespace qw;

namespace {

enum {
    ID_GRID = 1001, ID_REFRESH, ID_FULL, ID_QUICK, ID_FORMAT, ID_WIPE, ID_CANCEL, ID_PROGRESS, ID_STATUS
};
const UINT WM_APP_PROGRESS = WM_APP + 1;
const UINT WM_APP_DONE = WM_APP + 2;
const UINT WM_APP_REFORMAT = WM_APP + 3;

int g_dpi = 96;
int S(int dip) { return MulDiv(dip, g_dpi, 96); }   // DIP -> px

HFONT g_font = nullptr;
HFONT g_mono = nullptr;
HWND hMain, hHeader, hGrid, hRefresh, hModeBox, hFull, hQuick, hFmtBox, hFormat,
     hWipe, hCancel, hProgress, hStatus, hSpeed, hLimits;

std::vector<DiskInfo> g_disks;
std::vector<GuardResult> g_guards;
bool g_suppressSel = false;

// running job
struct Job {
    DiskInfo disk; Mode mode; FsChoice fs;
    std::atomic<bool> cancel{ false };
    Outcome outcome = Outcome::Completed;
    std::string err;
    bool reformatTried = false;
    ReformatResult reformat{ true, "" };
    HANDLE thread = nullptr;
};
Job* g_job = nullptr;
CRITICAL_SECTION g_cs;
Progress g_latest;

void SetFontAll(HWND parent) {
    EnumChildWindows(parent, [](HWND h, LPARAM f) -> BOOL {
        SendMessageW(h, WM_SETFONT, (WPARAM)f, TRUE); return TRUE;
    }, (LPARAM)g_font);
}

std::wstring WideOf(const std::string& s) { return Widen(s); }

void AddColumn(HWND lv, int i, const wchar_t* text, int width) {
    LVCOLUMNW c{}; c.mask = LVCF_TEXT | LVCF_WIDTH; c.pszText = (LPWSTR)text; c.cx = S(width);
    ListView_InsertColumn(lv, i, &c);
}

FsChoice SelectedFs() {
    int i = (int)SendMessageW(hFormat, CB_GETCURSEL, 0, 0);
    return i == 1 ? FsChoice::ExFat : i == 2 ? FsChoice::Ntfs : FsChoice::None;
}
Mode SelectedMode() {
    return SendMessageW(hFull, BM_GETCHECK, 0, 0) == BST_CHECKED ? Mode::Full : Mode::Quick;
}
int SelectedRow() {
    return (int)SendMessageW(hGrid, LVM_GETNEXTITEM, (WPARAM)-1, LVNI_SELECTED);
}

void UpdateWipeButton() {
    int row = SelectedRow();
    bool ok = row >= 0 && row < (int)g_guards.size() && g_guards[row].wipeable && g_job == nullptr;
    EnableWindow(hWipe, ok);
}

void RefreshList() {
    ListView_DeleteAllItems(hGrid);
    g_disks = Enumerate();
    auto prot = GetProtectedDiskNumbers();
    g_guards.clear();
    for (size_t i = 0; i < g_disks.size(); ++i) {
        const DiskInfo& d = g_disks[i];
        GuardResult g = Evaluate(d, prot, false);
        g_guards.push_back(g);
        LVITEMW it{}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = (int)i; it.lParam = (LPARAM)i;
        std::wstring c0 = std::to_wstring(d.index);
        it.pszText = (LPWSTR)c0.c_str();
        int row = ListView_InsertItem(hGrid, &it);
        std::wstring c1 = WideOf(d.model), c2 = WideOf(d.sizeDisplay()), c3 = WideOf(BusName(d.busType)),
                     c4 = WideOf(d.lettersDisplay()),
                     c5 = WideOf((g.wipeable ? "WIPEABLE - " : "blocked - ") + g.reason);
        ListView_SetItemText(hGrid, row, 1, (LPWSTR)c1.c_str());
        ListView_SetItemText(hGrid, row, 2, (LPWSTR)c2.c_str());
        ListView_SetItemText(hGrid, row, 3, (LPWSTR)c3.c_str());
        ListView_SetItemText(hGrid, row, 4, (LPWSTR)c4.c_str());
        ListView_SetItemText(hGrid, row, 5, (LPWSTR)c5.c_str());
    }
    UpdateWipeButton();
    SetWindowTextW(hStatus, L"Ready.");
}

void SetBusy(bool busy) {
    EnableWindow(hGrid, !busy);
    EnableWindow(hRefresh, !busy);
    EnableWindow(hFull, !busy);
    EnableWindow(hQuick, !busy);
    EnableWindow(hFormat, !busy);
    EnableWindow(hCancel, busy);
    EnableWindow(hWipe, FALSE);
    SetWindowTextW(hSpeed, busy ? L"Measuring..." : L"");
    if (!busy) { SendMessageW(hProgress, PBM_SETPOS, 0, 0); UpdateWipeButton(); }
}

// ---- confirm dialog (typed ERASE) ----------------------------------------

struct ConfirmState { std::wstring details; bool result = false; HWND edit = nullptr, ok = nullptr; bool done = false; };

LRESULT CALLBACK ConfirmProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto* st = (ConfirmState*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (m) {
        case WM_COMMAND:
            if (LOWORD(w) == IDOK) { st->result = true; st->done = true; DestroyWindow(h); return 0; }
            if (LOWORD(w) == IDCANCEL) { st->result = false; st->done = true; DestroyWindow(h); return 0; }
            if (HIWORD(w) == EN_CHANGE && (HWND)l == st->edit) {
                wchar_t b[32]{}; GetWindowTextW(st->edit, b, 32);
                EnableWindow(st->ok, lstrcmpW(b, L"ERASE") == 0);
                return 0;
            }
            break;
        case WM_CLOSE: st->result = false; st->done = true; DestroyWindow(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool ShowConfirm(HWND parent, const DiskInfo& d, Mode mode, FsChoice fs) {
    static bool reg = false;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!reg) {
        WNDCLASSW wc{}; wc.lpfnWndProc = ConfirmProc; wc.hInstance = hi;
        wc.lpszClassName = L"QWConfirm"; wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        RegisterClassW(&wc); reg = true;
    }
    std::string then = fs == FsChoice::None ? "leave raw" : std::string("reformat as ") + (fs == FsChoice::ExFat ? "exFAT" : "NTFS");
    std::string txt = "You are about to PERMANENTLY DESTROY all data on:\r\n\r\n"
        "   Disk " + std::to_string(d.index) + ":  " + d.model + "\r\n"
        "   Size:   " + d.sizeDisplay() + "\r\n"
        "   Bus:    " + BusName(d.busType) + "    Drives: " + d.lettersDisplay() + "\r\n"
        "   Serial: " + (d.serial.empty() ? "-" : d.serial) + "\r\n\r\n"
        "   Mode:   " + (mode == Mode::Full ? "Full" : "Quick") + "\r\n"
        "   Then:   " + then + "\r\n"
        + (d.letters.empty() ? "" :
           "\r\nMounted volumes (" + d.lettersDisplay() + ") will be dismounted automatically.\r\n")
        + "\r\nThis cannot be undone.";

    ConfirmState st; st.details = WideOf(txt);

    RECT pr; GetWindowRect(parent, &pr);
    int w = S(440), h = S(320);
    int x = pr.left + ((pr.right - pr.left) - w) / 2, y = pr.top + ((pr.bottom - pr.top) - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, L"QWConfirm", L"Confirm wipe",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, h, parent, nullptr, hi, nullptr);
    SetWindowLongPtrW(dlg, GWLP_USERDATA, (LONG_PTR)&st);

    CreateWindowExW(0, L"STATIC", st.details.c_str(), WS_CHILD | WS_VISIBLE,
        S(16), S(12), S(404), S(180), dlg, nullptr, hi, nullptr);
    CreateWindowExW(0, L"STATIC", L"Type ERASE to confirm:", WS_CHILD | WS_VISIBLE,
        S(16), S(196), S(404), S(20), dlg, nullptr, hi, nullptr);
    st.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        S(16), S(218), S(404), S(26), dlg, nullptr, hi, nullptr);
    st.ok = CreateWindowExW(0, L"BUTTON", L"Wipe now", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        S(232), S(260), S(110), S(34), dlg, (HMENU)IDOK, hi, nullptr);
    EnableWindow(st.ok, FALSE);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        S(350), S(260), S(70), S(34), dlg, (HMENU)IDCANCEL, hi, nullptr);
    SetFontAll(dlg);

    EnableWindow(parent, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(st.edit);

    MSG msg;
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return st.result;
}

// ---- wipe worker ----------------------------------------------------------

DWORD WINAPI WorkerProc(LPVOID) {
    Job* job = g_job;
    auto cb = [](const Progress& p) {
        EnterCriticalSection(&g_cs); g_latest = p; LeaveCriticalSection(&g_cs);
        PostMessageW(hMain, WM_APP_PROGRESS, 0, 0);
    };
    job->outcome = RunWipe(job->disk, job->mode, cb, job->cancel, -1, job->err);
    // Reformat even after a cancel: by the time you can cancel, the partition table
    // is already gone, so still give back the formatted drive that was requested.
    if (job->err.empty() && job->fs != FsChoice::None) {
        job->reformatTried = true;
        PostMessageW(hMain, WM_APP_REFORMAT, 0, 0); // tell the UI we've moved on to formatting
        job->reformat = Reformat(job->disk.index, job->fs);
    }
    PostMessageW(hMain, WM_APP_DONE, 0, 0);
    return 0;
}

void StartWipe() {
    int row = SelectedRow();
    if (row < 0 || row >= (int)g_guards.size() || !g_guards[row].wipeable) return;
    DiskInfo disk = g_disks[row];

    // Defense in depth: re-enumerate + re-verify just before writing.
    auto prot = GetProtectedDiskNumbers();
    bool ok = false; DiskInfo fresh;
    for (auto& d : Enumerate())
        if (d.index == disk.index && d.serial == disk.serial && d.sizeBytes == disk.sizeBytes
            && Evaluate(d, prot, false).wipeable) { fresh = d; ok = true; break; }
    if (!ok) {
        MessageBoxW(hMain, L"The device changed since it was listed. Refreshing - please re-select.",
            L"QuickWiper", MB_OK | MB_ICONWARNING);
        RefreshList();
        return;
    }

    Mode mode = SelectedMode(); FsChoice fs = SelectedFs();
    if (!ShowConfirm(hMain, fresh, mode, fs)) return;

    g_job = new Job();
    g_job->disk = fresh; g_job->mode = mode; g_job->fs = fs;
    SetBusy(true);
    g_job->thread = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
}

void OnDone() {
    Job* job = g_job;
    std::string status;
    if (!job->err.empty()) {
        status = "Failed.";
        MessageBoxW(hMain, WideOf(job->err).c_str(), L"Wipe failed", MB_OK | MB_ICONERROR);
    } else {
        std::string base =
            job->outcome == Outcome::Cancelled ? "Wipe cancelled" :
            job->outcome == Outcome::TimedOut  ? "Wipe stopped (time box)" : "Wipe completed";
        if (job->reformatTried && !job->reformat.success) {
            status = base + ", but reformat failed.";
            MessageBoxW(hMain, WideOf(job->reformat.output).c_str(), L"Reformat failed", MB_OK | MB_ICONWARNING);
        } else if (job->reformatTried) {
            status = base + "; formatted as " + (job->fs == FsChoice::ExFat ? "exFAT" : "NTFS") + ".";
        } else {
            status = base + ".";
        }
    }
    if (job->thread) { WaitForSingleObject(job->thread, INFINITE); CloseHandle(job->thread); }
    delete job; g_job = nullptr;
    SetBusy(false);
    RefreshList();
    SetWindowTextW(hStatus, WideOf(status).c_str());
}

// ---- layout & main window -------------------------------------------------

void Layout(int cw, int ch) {
    int m = S(16), gap = S(16);
    int x = m, y = m, w = cw - 2 * m;
    int btnH = S(34), boxH = S(96), barH = S(26), lblH = S(40), speedH = S(58);

    int refreshW = S(96);
    MoveWindow(hHeader, x, y, w - refreshW - gap, S(40), TRUE);
    MoveWindow(hRefresh, x + w - refreshW, y, refreshW, btnH, TRUE);
    y += S(48);

    int gridBottom = ch - m - lblH - gap - speedH - gap - S(24) - gap - barH - gap - btnH - gap - boxH - gap;
    int gridH = gridBottom - y;
    if (gridH < S(120)) gridH = S(120);
    MoveWindow(hGrid, x, y, w, gridH, TRUE);
    y += gridH + gap;

    int modeW = S(330), fmtW = S(260);
    MoveWindow(hModeBox, x, y, modeW, boxH, TRUE);
    MoveWindow(hFull, x + S(16), y + S(28), S(290), S(24), TRUE);
    MoveWindow(hQuick, x + S(16), y + S(58), S(290), S(24), TRUE);
    MoveWindow(hFmtBox, x + modeW + gap, y, fmtW, boxH, TRUE);
    MoveWindow(hFormat, x + modeW + gap + S(16), y + S(34), S(200), S(200), TRUE);
    y += boxH + gap;

    MoveWindow(hWipe, x, y, S(210), btnH, TRUE);
    MoveWindow(hCancel, x + S(226), y, S(110), btnH, TRUE);
    y += btnH + gap;

    MoveWindow(hProgress, x, y, w, barH, TRUE);
    y += barH + gap;
    MoveWindow(hStatus, x, y, w, S(24), TRUE);
    y += S(24) + gap;
    MoveWindow(hSpeed, x, y, w, speedH, TRUE);
    y += speedH + gap;
    MoveWindow(hLimits, x, y, w, lblH, TRUE);
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_CREATE: {
            HINSTANCE hi = ((CREATESTRUCT*)l)->hInstance;
            hHeader = CreateWindowExW(0, L"STATIC",
                L"Select a USB device to wipe.\nThe system disk and internal SATA/NVMe disks cannot be selected.",
                WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, nullptr, hi, nullptr);
            hRefresh = CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 0, 0, h, (HMENU)ID_REFRESH, hi, nullptr);
            hGrid = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                0, 0, 0, 0, h, (HMENU)ID_GRID, hi, nullptr);
            ListView_SetExtendedListViewStyle(hGrid, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            AddColumn(hGrid, 0, L"Disk", 50);
            AddColumn(hGrid, 1, L"Model", 230);
            AddColumn(hGrid, 2, L"Size", 90);
            AddColumn(hGrid, 3, L"Bus", 130);
            AddColumn(hGrid, 4, L"Drives", 80);
            AddColumn(hGrid, 5, L"Status", 260);

            hModeBox = CreateWindowExW(0, L"BUTTON", L"Mode", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, h, nullptr, hi, nullptr);
            hFull = CreateWindowExW(0, L"BUTTON", L"Full (thorough overwrite)",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | WS_GROUP, 0, 0, 0, 0, h, (HMENU)ID_FULL, hi, nullptr);
            hQuick = CreateWindowExW(0, L"BUTTON", L"Quick (progressive, cancel anytime)",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON, 0, 0, 0, 0, h, (HMENU)ID_QUICK, hi, nullptr);
            SendMessageW(hQuick, BM_SETCHECK, BST_CHECKED, 0);

            hFmtBox = CreateWindowExW(0, L"BUTTON", L"After wipe, reformat as", WS_CHILD | WS_VISIBLE | BS_GROUPBOX, 0, 0, 0, 0, h, nullptr, hi, nullptr);
            hFormat = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                0, 0, 0, 0, h, (HMENU)ID_FORMAT, hi, nullptr);
            SendMessageW(hFormat, CB_ADDSTRING, 0, (LPARAM)L"Leave raw");
            SendMessageW(hFormat, CB_ADDSTRING, 0, (LPARAM)L"exFAT");
            SendMessageW(hFormat, CB_ADDSTRING, 0, (LPARAM)L"NTFS");
            SendMessageW(hFormat, CB_SETCURSEL, 1, 0);

            hWipe = CreateWindowExW(0, L"BUTTON", L"Wipe selected device", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 0, 0, 0, 0, h, (HMENU)ID_WIPE, hi, nullptr);
            EnableWindow(hWipe, FALSE);
            hCancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 0, 0, h, (HMENU)ID_CANCEL, hi, nullptr);
            EnableWindow(hCancel, FALSE);
            hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)ID_PROGRESS, hi, nullptr);
            SendMessageW(hProgress, PBM_SETRANGE32, 0, 1000);
            hStatus = CreateWindowExW(0, L"STATIC", L"Ready.", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, (HMENU)ID_STATUS, hi, nullptr);
            hSpeed = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, nullptr, hi, nullptr);
            hLimits = CreateWindowExW(0, L"STATIC",
                L"Note: this destroys the filesystem and defeats software file-recovery tools. It cannot "
                L"guarantee against lab-grade chip-off forensics; flash wear-leveling keeps remapped copies "
                L"beyond any software's reach.",
                WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, h, nullptr, hi, nullptr);

            // Tooltips explaining the two modes (hover over the radio buttons).
            HWND hTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0, h, nullptr, hi, nullptr);
            SendMessageW(hTip, TTM_SETMAXTIPWIDTH, 0, (LPARAM)S(340));
            SendMessageW(hTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, (LPARAM)MAKELONG(20000, 0));
            auto addTip = [&](HWND ctrl, const wchar_t* text) {
                TTTOOLINFOW ti{}; ti.cbSize = sizeof(ti);
                ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                ti.hwnd = h; ti.uId = (UINT_PTR)ctrl; ti.lpszText = (LPWSTR)text;
                SendMessageW(hTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
            };
            addTip(hFull,
                L"Full: writes random data over the entire device, from the first sector "
                L"to the last. Most thorough; takes as long as it takes to write the whole "
                L"device once.");
            addTip(hQuick,
                L"Quick: a progressive wipe you can cancel at any time. It works in order of "
                L"impact - first it destroys the filesystem map (the index of what is where), "
                L"then overwrites file headers across the whole device, then fills the rest with "
                L"random data. Stop after a few seconds for a fast, partial wipe, or let it run "
                L"to completion for the same result as Full.");

            SetFontAll(h);
            if (g_mono) SendMessageW(hSpeed, WM_SETFONT, (WPARAM)g_mono, TRUE); // align the speed columns
            RefreshList();
            return 0;
        }
        case WM_SIZE: Layout(LOWORD(l), HIWORD(l)); return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = (MINMAXINFO*)l;
            mmi->ptMinTrackSize.x = S(780); mmi->ptMinTrackSize.y = S(560);
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(w)) {
                case ID_REFRESH: RefreshList(); return 0;
                case ID_WIPE: StartWipe(); return 0;
                case ID_CANCEL:
                    if (g_job && !g_job->cancel.load()) {
                        g_job->cancel.store(true);
                        EnableWindow(hCancel, FALSE);
                        SetWindowTextW(hStatus, L"Cancelling - finishing the current block...");
                    }
                    return 0;
            }
            return 0;
        case WM_NOTIFY: {
            auto* nh = (LPNMHDR)l;
            if (nh->idFrom == ID_GRID) {
                if (nh->code == LVN_ITEMCHANGED) {
                    auto* nv = (LPNMLISTVIEW)l;
                    if (!g_suppressSel && (nv->uChanged & LVIF_STATE) && (nv->uNewState & LVIS_SELECTED)) {
                        int row = nv->iItem;
                        if (row >= 0 && row < (int)g_guards.size() && !g_guards[row].wipeable) {
                            g_suppressSel = true;
                            ListView_SetItemState(hGrid, row, 0, LVIS_SELECTED);
                            g_suppressSel = false;
                        }
                    }
                    UpdateWipeButton();
                } else if (nh->code == NM_CUSTOMDRAW) {
                    auto* cd = (LPNMLVCUSTOMDRAW)l;
                    if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                    if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        int row = (int)cd->nmcd.dwItemSpec;
                        if (row >= 0 && row < (int)g_guards.size() && !g_guards[row].wipeable)
                            cd->clrText = GetSysColor(COLOR_GRAYTEXT);
                        return CDRF_DODEFAULT;
                    }
                }
            }
            return 0;
        }
        case WM_APP_PROGRESS: {
            if (g_job && g_job->cancel.load()) return 0; // keep the "Cancelling..." message
            EnterCriticalSection(&g_cs); Progress p = g_latest; LeaveCriticalSection(&g_cs);
            SendMessageW(hProgress, PBM_SETPOS, (WPARAM)(p.fraction() * 1000), 0);
            int es = (int)(p.etaSeconds + 0.5);
            wchar_t b[160];
            std::swprintf(b, 160, L"%ls - %.1f%%   ETA %02d:%02d:%02d",
                WideOf(p.pass).c_str(), p.fraction() * 100.0, es / 3600, (es / 60) % 60, es % 60);
            SetWindowTextW(hStatus, b);
            wchar_t s[320];
            std::swprintf(s, 320,
                L"Current   %7.1f MB/s\n"
                L"Average   10s %7.1f   1 min %7.1f   total %7.1f MB/s\n"
                L"Min %7.1f   Max %7.1f MB/s",
                p.curMbPerSec, p.avg10sMbPerSec, p.avg60sMbPerSec, p.avgMbPerSec, p.minMbPerSec, p.maxMbPerSec);
            SetWindowTextW(hSpeed, s);
            return 0;
        }
        case WM_APP_REFORMAT: {
            EnableWindow(hCancel, FALSE); // a format can't be cancelled
            const wchar_t* fsName = (g_job && g_job->fs == FsChoice::ExFat) ? L"exFAT" : L"NTFS";
            std::wstring m = (g_job && g_job->outcome == Outcome::Cancelled)
                ? std::wstring(L"Wipe cancelled - reformatting as ") + fsName + L"..."
                : std::wstring(L"Reformatting as ") + fsName + L"...";
            SetWindowTextW(hStatus, m.c_str());
            return 0;
        }
        case WM_APP_DONE: OnDone(); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int RunGui(HINSTANCE hInst) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc),
        ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES };
    InitCommonControlsEx(&icc);
    InitializeCriticalSection(&g_cs);

    g_dpi = (int)GetDpiForSystem();
    g_font = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_mono = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, L"Consolas");

    WNDCLASSW wc{};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"QuickWiperMain";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    hMain = CreateWindowExW(0, L"QuickWiperMain", L"QuickWiper",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(920), S(720),
        nullptr, nullptr, hInst, nullptr);
    ShowWindow(hMain, SW_SHOW);
    UpdateWindow(hMain);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hMain, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return 0;
}
