#pragma once
#include <windows.h>
constexpr DWORD TH32CS_SNAPPROCESS = 1;
struct PROCESSENTRY32W { DWORD dwSize, th32ParentProcessID, th32ProcessID; };
inline HANDLE CreateToolhelp32Snapshot(DWORD, DWORD) { ++Fake::snapshots; return 1000; }
inline BOOL Process32NextW(HANDLE, PROCESSENTRY32W* entry) {
    if (Fake::cursor >= Fake::processes.size()) return FALSE;
    auto [parent, pid] = Fake::processes[Fake::cursor++];
    entry->th32ParentProcessID = parent;
    entry->th32ProcessID = pid;
    return TRUE;
}
inline BOOL Process32FirstW(HANDLE h, PROCESSENTRY32W* entry) {
    Fake::cursor = 0;
    return Process32NextW(h, entry);
}
