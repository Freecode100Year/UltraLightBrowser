#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <utility>
using DWORD = unsigned long;
using UINT32 = unsigned int;
using BOOL = int;
using HANDLE = std::intptr_t;
using SIZE_T = std::size_t;
using HRESULT = long;
constexpr BOOL FALSE = 0, TRUE = 1;
constexpr HRESULT S_OK = 0, E_FAIL = -1;
constexpr HANDLE INVALID_HANDLE_VALUE = -1;
constexpr DWORD PROCESS_SET_INFORMATION = 1, PROCESS_QUERY_LIMITED_INFORMATION = 2;
constexpr DWORD ABOVE_NORMAL_PRIORITY_CLASS = 3, NORMAL_PRIORITY_CLASS = 4;
constexpr DWORD PROCESS_POWER_THROTTLING_CURRENT_VERSION = 1, PROCESS_POWER_THROTTLING_EXECUTION_SPEED = 1;
constexpr int ProcessPowerThrottling = 0;
#define SUCCEEDED(hr) ((hr) >= 0)
#define FAILED(hr) ((hr) < 0)
#define IID_PPV_ARGS(pp) (pp)
struct PROCESS_POWER_THROTTLING_STATE { DWORD Version, ControlMask, StateMask; };
namespace Fake {
inline int snapshots = 0, trims = 0;
inline std::vector<std::pair<DWORD, DWORD>> processes;
inline std::vector<std::pair<HANDLE, bool>> throttling;
inline std::size_t cursor = 0;
}
inline HANDLE GetCurrentProcess() { return 999; }
inline BOOL SetProcessInformation(HANDLE h, int, void* state, std::size_t) {
    Fake::throttling.emplace_back(h, static_cast<PROCESS_POWER_THROTTLING_STATE*>(state)->StateMask != 0);
    return TRUE;
}
inline BOOL SetPriorityClass(HANDLE, DWORD) { return TRUE; }
inline HANDLE OpenProcess(DWORD, BOOL, DWORD pid) { return static_cast<HANDLE>(pid); }
inline BOOL CloseHandle(HANDLE) { return TRUE; }
inline BOOL SetProcessWorkingSetSize(HANDLE, SIZE_T, SIZE_T) { ++Fake::trims; return TRUE; }
