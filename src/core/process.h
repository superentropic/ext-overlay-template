#pragma once
#ifdef _WIN32
#include <Windows.h>
#include <TlHelp32.h>
#else
#include "kernel_driver.h"
#include <sys/types.h>
#include <cstdint>
using DWORD = uint32_t;
using HANDLE = int;
#endif
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>
#include "../shared/shared_memory.h"

struct ModuleInfo {
    uintptr_t base = 0;
    DWORD size = 0;
    std::wstring name;
};

class Process {
public:
    Process() = default;
    ~Process();

    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    bool Attach(const std::wstring& processName);
    bool Attach(DWORD pid);
    void Detach();

    uintptr_t GetModuleBase(const std::wstring& moduleName) const;
    DWORD GetModuleSize(const std::wstring& moduleName) const;

    template<typename T>
    T Read(uintptr_t address) const {
        T value{};
        if (!ReadRaw(address, &value, sizeof(T)))
            return T{};
        return value;
    }

    bool ReadRaw(uintptr_t address, void* buffer, size_t size) const;

    // Write memory
    template<typename T>
    bool Write(uintptr_t address, const T& value) const {
        return WriteRaw(address, &value, sizeof(T));
    }

    bool WriteRaw(uintptr_t address, const void* buffer, size_t size) const;

    // Read a chain of pointers: base -> [off1] -> [off2] -> ... -> final
    uintptr_t ReadPointerChain(uintptr_t base, const std::vector<uintptr_t>& offsets) const;

    HANDLE GetHandle() const { return m_handle; }
    DWORD GetPid() const { return m_pid; }
    bool IsValid() const;

    const std::wstring& GetProcessName() const { return m_processName; }
    const std::unordered_map<std::wstring, ModuleInfo>& GetModules() const { return m_modules; }

    // Shared memory relay & proxy status
    bool IsUsingRelay() const { return m_useRelay; }
    SharedGameData* GetSharedData() const { return m_shm; }
#ifndef _WIN32
    bool IsUsingKernelDriver() const { return m_kernelDriver.IsOpen(); }
#endif

    // Proxy telemetry & verification
    bool HasProxyModule(uintptr_t* outBase = nullptr, DWORD* outSize = nullptr) const;
    void UpdateProxySync();
    void TryConnectProxy();   // call from background thread only — runs the expensive SHM scan
    void SyncProxyTarget(const SilentHelperIPC& targetConfig);
    uintptr_t GetProxyBase() const { return m_proxyBase; }
    DWORD GetProxySize() const { return m_proxySize; }
    uint32_t GetProxyHeartbeat() const { return m_shm ? m_shm->heartbeat : 0; }
    bool IsProxyConnected() const { return m_proxyConnected; }
    uintptr_t GetRemoteShmAddress() const { return m_remoteShmAddr; }
    const SharedGameData& GetLocalSharedData() const { return m_localShm; }

    // Manual payload base (e.g. address printed by the injector). 0 = auto-scan.
    // Applying a new value drops the current connection and forces an immediate retry.
    void SetManualProxyBase(uintptr_t base);
    uintptr_t GetManualProxyBase() const { return m_manualProxyBase; }
    // True when a manual base is set but no SharedGameData was found in that region.
    bool ManualProxyBaseFailed() const { return m_manualBaseFailed; }

    // Public static helper for process discovery
    static DWORD FindProcessId(const std::wstring& processName);

private:
    bool EnumerateModules();
    bool ConnectSharedMemoryRelay();
    bool RelayRead(uintptr_t address, void* buffer, size_t size) const;

#ifdef _WIN32
    HANDLE m_handle = nullptr;
#else
    HANDLE m_handle = -1;
    KernelDriver m_kernelDriver;
#endif
    DWORD m_pid = 0;
    std::wstring m_processName;
    std::unordered_map<std::wstring, ModuleInfo> m_modules;

    // Shared memory relay (fallback when RPM is blocked by anti-cheat)
#ifdef _WIN32
    HANDLE m_shmHandle = nullptr;
#else
    HANDLE m_shmHandle = -1;
#endif
    SharedGameData* m_shm = nullptr;
    bool m_useRelay = false;
    mutable int m_nextRelaySlot = 0;  // Round-robin slot selection

    // Proxy live synchronization
    uintptr_t m_proxyBase = 0;
    DWORD m_proxySize = 0;
    uintptr_t m_remoteShmAddr = 0;
    bool m_proxyConnected = false;
    mutable SharedGameData m_localShm = {};

    uintptr_t m_manualProxyBase = 0;
    bool m_manualBaseFailed = false;
    bool m_forceProxyRescan = false;
    bool ScanRegionForShm(uintptr_t start, uintptr_t end, uintptr_t reportedBase);
};
