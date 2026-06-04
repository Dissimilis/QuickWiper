#include <windows.h>

// One exe, two faces: args => headless CLI; no args => Win32 GUI.
// Built as a GUI-subsystem app (no console flash); for CLI we attach to the
// launching terminal unless stdout is already redirected (test harness / pipe).

int RunCli(int argc, char** argv);
int RunGui(HINSTANCE hInst);

int main(int argc, char** argv) {
    if (argc > 1) {
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == nullptr || out == INVALID_HANDLE_VALUE)
            AttachConsole(ATTACH_PARENT_PROCESS);
        return RunCli(argc, argv);
    }
    return RunGui(GetModuleHandleW(nullptr));
}
