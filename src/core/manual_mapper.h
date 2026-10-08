#pragma once
// ============================================================================
//  Manual Mapping DLL Injector
//
//  Loads a DLL into a target process without calling LoadLibrary.
//  Uses direct syscalls to bypass user-mode anti-cheat hooks.
//
//  Steps: Parse PE → Allocate remote memory → Map sections →
//         Fix relocations → Resolve imports → Execute DllMain
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>
#include <vector>

namespace ManualMapper {

struct MapResult {
    bool success = false;
    std::string message;
    uintptr_t remoteBase = 0;   // Base address in target process
    DWORD lastError = 0;
};

// Manual map a DLL into a target process (uses direct syscalls)
MapResult Map(HANDLE hProcess, const std::wstring& dllPath);

// Manual map using a process handle you've already opened
// (for callers that already have the handle)
MapResult MapWithHandle(HANDLE hProcess, const std::wstring& dllPath);

} // namespace ManualMapper
