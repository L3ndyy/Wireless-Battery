#pragma once
// Self-update from the project's GitHub releases.
#include <windows.h>

#include <string>

namespace update {

// Checks the latest release and, when it is newer, asks the user and installs it.
// Blocking (network); call from a worker thread. `manual` shows the "up to date" and error boxes.
void CheckAndPrompt(HWND owner, bool manual);

// `--apply-update "<path to WirelessBattery.exe>"`: run by the downloaded copy with administrator
// rights when the installed folder is not writable. Returns 0 on success.
int ApplyFromCommandLine(const wchar_t* cmdLine);

// Deletes "WirelessBattery.exe.old" left by a previous update (the old process has exited by now).
void CleanupOldVersion();

}  // namespace update
