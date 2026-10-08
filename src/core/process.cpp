#include "process.h"
#include "process_finder.h"
#include "../shared/shared_memory.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <Psapi.h>
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "advapi32.lib")

// Enable SeDebugPrivilege — required to read protected processes
static bool EnableDebugPrivilege() {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        spdlog::warn("OpenProcessToken failed: {}", GetLastError());
        return false;
    }

    LUID luid = {};
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        spdlog::warn("LookupPrivilegeValue failed: {}", GetLastError());
        CloseHandle(hToken);
        return false;
    }

    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    if (!AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        spdlog::warn("AdjustTokenPrivileges failed: {}", GetLastError());
        CloseHandle(hToken);
        return false;
    }

    DWORD err = GetLastError();
    CloseHandle(hToken);

    if (err == ERROR_NOT_ALL_ASSIGNED) {
        spdlog::warn("SeDebugPrivilege not available (not running as admin?)");
        return false;
    }

    spdlog::info("SeDebugPrivilege enabled successfully");
    return true;
}

Process::~Process() {
    Detach();
}

bool Process::Attach(const std::wstring& processName) {
    DWORD pid = FindProcessId(processName);
    if (pid == 0) {
        // Convert wide string to narrow for logging
        char narrow[256] = {};
        size_t converted = 0;
        wcstombs_s(&converted, narrow, processName.c_str(), sizeof(narrow) - 1);
        spdlog::warn("Process '{}' not found", narrow);
        return false;
    }
    m_processName = processName;
    return Attach(pid);
}

bool Process::Attach(DWORD pid) {
    Detach();

    // Enable debug privilege before opening — critical for anti-cheat protected processes
    static bool debugPrivEnabled = false;
    if (!debugPrivEnabled) {
        debugPrivEnabled = EnableDebugPrivilege();
    }

    // Try PROCESS_ALL_ACCESS first (best chance against handle stripping)
    m_handle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    if (!m_handle) {
        spdlog::warn("PROCESS_ALL_ACCESS failed (error {}), trying limited access...", GetLastError());
        m_handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
    }
    if (!m_handle) {
        spdlog::error("Failed to open process PID {}. Error: {}", pid, GetLastError());
        return false;
    }

    // Verify we actually have VM_READ by doing a test read
    {
        uintptr_t testVal = 0;
        SIZE_T bytesRead = 0;
        // Try reading the PEB address via NtQueryInformationProcess to verify handle works
        typedef NTSTATUS(WINAPI* NtQueryFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        auto NtQuery = reinterpret_cast<NtQueryFn>(
            GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
        if (NtQuery) {
            struct { PVOID r1; PVOID PebBase; PVOID r2[2]; ULONG_PTR pid; PVOID r3; } pbi = {};
            ULONG len = 0;
            NTSTATUS st = NtQuery(m_handle, 0, &pbi, sizeof(pbi), &len);
            spdlog::info("Handle access test: NtQuery status=0x{:X}, PEB=0x{:X}",
                (unsigned long)st, reinterpret_cast<uintptr_t>(pbi.PebBase));

            if (st == 0 && pbi.PebBase) {
                // Try actual memory read
                if (ReadProcessMemory(m_handle, reinterpret_cast<LPCVOID>(
                    reinterpret_cast<uintptr_t>(pbi.PebBase)), &testVal, sizeof(testVal), &bytesRead)) {
                    spdlog::info("VM_READ verified: read {} bytes from PEB", bytesRead);
                } else {
                    spdlog::warn("VM_READ blocked by anti-cheat (error {}) — will try shared memory relay", GetLastError());
                }
            }
        }
    }

    m_pid = pid;
    if (!EnumerateModules()) {
        spdlog::warn("Failed to enumerate modules for PID {}", pid);
    }

    spdlog::info("Attached to PID {} with {} modules", pid, m_modules.size());
    return true;
}

void Process::Detach() {
    if (m_shm) {
        UnmapViewOfFile(m_shm);
        m_shm = nullptr;
    }
    if (m_shmHandle) {
        CloseHandle(m_shmHandle);
        m_shmHandle = nullptr;
    }
    m_useRelay = false;

    if (m_handle) {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
    m_pid = 0;
    m_modules.clear();
}

uintptr_t Process::GetModuleBase(const std::wstring& moduleName) const {
    // Case-insensitive lookup
    std::wstring lower = moduleName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

    for (const auto& [name, info] : m_modules) {
        std::wstring nameLower = name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
        if (nameLower == lower)
            return info.base;
    }
    return 0;
}

DWORD Process::GetModuleSize(const std::wstring& moduleName) const {
    std::wstring lower = moduleName;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);

    for (const auto& [name, info] : m_modules) {
        std::wstring nameLower = name;
        std::transform(nameLower.begin(), nameLower.end(), nameLower.begin(), ::towlower);
        if (nameLower == lower)
            return info.size;
    }
    return 0;
}

bool Process::ReadRaw(uintptr_t address, void* buffer, size_t size) const {
    if (!address || !buffer)
        return false;

    // If using shared memory relay, go through that instead of RPM
    if (m_useRelay) {
        return RelayRead(address, buffer, size);
    }

    if (!m_handle)
        return false;

    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(m_handle, reinterpret_cast<LPCVOID>(address), buffer, size, &bytesRead)) {
        return false;
    }
    return bytesRead == size;
}

bool Process::WriteRaw(uintptr_t address, const void* buffer, size_t size) const {
    if (!address || !buffer)
        return false;

    if (!m_handle)
        return false;

    SIZE_T bytesWritten = 0;
    if (!WriteProcessMemory(m_handle, reinterpret_cast<LPVOID>(address), buffer, size, &bytesWritten)) {
        return false;
    }
    return bytesWritten == size;
}

uintptr_t Process::ReadPointerChain(uintptr_t base, const std::vector<uintptr_t>& offsets) const {
    uintptr_t addr = base;
    for (size_t i = 0; i < offsets.size(); ++i) {
        addr = Read<uintptr_t>(addr);
        if (!addr) return 0;
        addr += offsets[i];
    }
    return addr;
}

bool Process::IsValid() const {
    if (!m_handle) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(m_handle, &exitCode))
        return false;
    return exitCode == STILL_ACTIVE;
}

bool Process::EnumerateModules() {
    m_modules.clear();

    // Method 1: CreateToolhelp32Snapshot (standard approach)
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, m_pid);
    if (snap != INVALID_HANDLE_VALUE) {
        MODULEENTRY32W me{};
        me.dwSize = sizeof(me);

        if (Module32FirstW(snap, &me)) {
            do {
                ModuleInfo info;
                info.base = reinterpret_cast<uintptr_t>(me.modBaseAddr);
                info.size = me.modBaseSize;
                info.name = me.szModule;
                m_modules[me.szModule] = info;
            } while (Module32NextW(snap, &me));
        }
        CloseHandle(snap);

        if (!m_modules.empty()) {
            spdlog::info("EnumerateModules: Toolhelp found {} modules", m_modules.size());
            return true;
        }
    } else {
        spdlog::warn("CreateToolhelp32Snapshot failed (error {}), trying fallbacks...", GetLastError());
    }

    // Method 2: EnumProcessModulesEx (PSAPI) — works when toolhelp is blocked
    {
        // Re-open with broader access for PSAPI
        HANDLE hProc = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, m_pid);
        if (hProc) {
            HMODULE hMods[1024];
            DWORD cbNeeded = 0;

            if (EnumProcessModulesEx(hProc, hMods, sizeof(hMods), &cbNeeded, LIST_MODULES_ALL)) {
                DWORD moduleCount = cbNeeded / sizeof(HMODULE);
                spdlog::info("EnumProcessModulesEx found {} modules", moduleCount);

                for (DWORD i = 0; i < moduleCount; ++i) {
                    wchar_t modName[MAX_PATH] = {};
                    MODULEINFO modInfo = {};

                    if (GetModuleFileNameExW(hProc, hMods[i], modName, MAX_PATH)) {
                        GetModuleInformation(hProc, hMods[i], &modInfo, sizeof(modInfo));

                        // Extract filename from full path
                        std::wstring fullPath(modName);
                        std::wstring fileName = fullPath;
                        auto lastSlash = fullPath.find_last_of(L"\\/");
                        if (lastSlash != std::wstring::npos)
                            fileName = fullPath.substr(lastSlash + 1);

                        ModuleInfo info;
                        info.base = reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);
                        info.size = modInfo.SizeOfImage;
                        info.name = fileName;
                        m_modules[fileName] = info;
                    }
                }
            } else {
                spdlog::warn("EnumProcessModulesEx failed (error {})", GetLastError());
            }
            CloseHandle(hProc);

            if (!m_modules.empty()) {
                return true;
            }
        }
    }

    // Method 3: PEB-based fallback for main module base address
    {
        spdlog::warn("All module enum methods failed; trying PEB fallback for main module...");

        typedef NTSTATUS(WINAPI* NtQueryInformationProcessFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (ntdll) {
            auto NtQueryInformationProcess = reinterpret_cast<NtQueryInformationProcessFn>(
                GetProcAddress(ntdll, "NtQueryInformationProcess"));

            if (NtQueryInformationProcess) {
                struct PROCESS_BASIC_INFORMATION {
                    PVOID Reserved1;
                    PVOID PebBaseAddress;
                    PVOID Reserved2[2];
                    ULONG_PTR UniqueProcessId;
                    PVOID Reserved3;
                };

                PROCESS_BASIC_INFORMATION pbi = {};
                ULONG len = 0;
                NTSTATUS status = NtQueryInformationProcess(m_handle, 0 /*ProcessBasicInformation*/, &pbi, sizeof(pbi), &len);

                spdlog::info("NtQueryInformationProcess: status=0x{:X}, PEB=0x{:X}",
                    (unsigned long)status, reinterpret_cast<uintptr_t>(pbi.PebBaseAddress));

                if (status == 0 && pbi.PebBaseAddress) {
                    uintptr_t pebAddr = reinterpret_cast<uintptr_t>(pbi.PebBaseAddress);
                    uintptr_t imageBase = 0;
                    bool readOk = ReadRaw(pebAddr + 0x10, &imageBase, sizeof(imageBase));
                    spdlog::info("PEB read: ok={}, ImageBaseAddress=0x{:X}", readOk, imageBase);

                    if (readOk && imageBase) {
                        // Get the actual process name for the module entry
                        wchar_t imagePath[MAX_PATH] = {};
                        DWORD pathSize = MAX_PATH;
                        QueryFullProcessImageNameW(m_handle, 0, imagePath, &pathSize);

                        std::wstring fullPath(imagePath);
                        std::wstring fileName = fullPath;
                        auto lastSlash = fullPath.find_last_of(L"\\/");
                        if (lastSlash != std::wstring::npos)
                            fileName = fullPath.substr(lastSlash + 1);

                        // Estimate module size from PE header
                        DWORD moduleSize = 0;
                        // Read DOS header -> e_lfanew -> NT header -> SizeOfImage
                        uint16_t dosMagic = 0;
                        ReadRaw(imageBase, &dosMagic, 2);
                        if (dosMagic == 0x5A4D) { // "MZ"
                            int32_t peOffset = 0;
                            ReadRaw(imageBase + 0x3C, &peOffset, 4);
                            // SizeOfImage is at NT_HEADER + 0x18 (OptionalHeader) + 0x38 (SizeOfImage) for 64-bit
                            ReadRaw(imageBase + peOffset + 0x50, &moduleSize, 4);
                        }
                        if (!moduleSize) moduleSize = 0x10000000; // 256MB fallback

                        ModuleInfo info;
                        info.base = imageBase;
                        info.size = moduleSize;
                        info.name = fileName;
                        m_modules[fileName] = info;

                        // Also add under the name the caller might use
                        if (fileName != m_processName && !m_processName.empty()) {
                            m_modules[m_processName] = info;
                        }

                        char narrowName[256] = {};
                        size_t cvt = 0;
                        wcstombs_s(&cvt, narrowName, fileName.c_str(), sizeof(narrowName) - 1);
                        spdlog::info("PEB fallback: {} at 0x{:X} (size 0x{:X})",
                            narrowName, imageBase, moduleSize);
                        return true;
                    }
                }
            }
        }
    }

    // Method 4: Shared memory relay from version.dll proxy
    if (ConnectSharedMemoryRelay()) {
        spdlog::info("Connected to shared memory relay! gameBase=0x{:X}, size=0x{:X}",
            m_shm->gameBase, m_shm->gameSize);

        // Get the actual process image name
        wchar_t imagePath[MAX_PATH] = {};
        DWORD pathSize = MAX_PATH;
        QueryFullProcessImageNameW(m_handle, 0, imagePath, &pathSize);
        std::wstring fullPath(imagePath);
        std::wstring fileName = fullPath;
        auto lastSlash = fullPath.find_last_of(L"\\/");
        if (lastSlash != std::wstring::npos)
            fileName = fullPath.substr(lastSlash + 1);

        ModuleInfo info;
        info.base = static_cast<uintptr_t>(m_shm->gameBase);
        info.size = static_cast<DWORD>(m_shm->gameSize);
        info.name = fileName;
        m_modules[fileName] = info;

        // Also register under common lookup names
        if (fileName != m_processName && !m_processName.empty()) {
            m_modules[m_processName] = info;
        }
        m_modules[L"GameProcess.exe"] = info;
        m_modules[L"GameProcess-Win64-Shipping.exe"] = info;

        m_useRelay = true;

        char narrowName[256] = {};
        size_t cvt = 0;
        wcstombs_s(&cvt, narrowName, fileName.c_str(), sizeof(narrowName) - 1);
        spdlog::info("Relay fallback: {} at 0x{:X} (size 0x{:X})",
            narrowName, info.base, info.size);
        return true;
    }

    spdlog::error("All module enumeration methods failed for PID {}", m_pid);
    return false;
}

// ============================================================================
//  Shared Memory Relay — connects to the version.dll proxy's named section
// ============================================================================
void Process::SetManualProxyBase(uintptr_t base) {
    if (m_manualProxyBase == base && !m_forceProxyRescan) {
        return;
    }
    spdlog::info("Setting manual proxy base address: 0x{:X}", base);
    m_manualProxyBase = base;
    m_forceProxyRescan = true;
    m_manualBaseFailed = false;

    m_remoteShmAddr = 0;
    m_proxyConnected = false;
    m_shm = nullptr;

    if (m_handle) {
        ConnectSharedMemoryRelay();
    }
}

bool Process::ScanRegionForShm(uintptr_t start, uintptr_t end, uintptr_t reportedBase) {
    if (start >= end) return false;
    constexpr size_t CHUNK_SIZE = 0x20000;
    constexpr size_t HEADER_SIZE = 8;
    std::vector<uint8_t> buffer(CHUNK_SIZE);

    for (uintptr_t addr = start; addr + HEADER_SIZE <= end; addr += (CHUNK_SIZE - HEADER_SIZE)) {
        size_t toRead = (std::min)(CHUNK_SIZE, static_cast<size_t>(end - addr));
        if (toRead < HEADER_SIZE) break;

        if (ReadRaw(addr, buffer.data(), toRead)) {
            for (size_t i = 0; i + HEADER_SIZE <= toRead; i += 4) {
                uint32_t magic = *reinterpret_cast<const uint32_t*>(&buffer[i]);
                if (magic == EXT_SHM_MAGIC) {
                    uint32_t ver = *reinterpret_cast<const uint32_t*>(&buffer[i + 4]);
                    if (ver == EXT_SHM_VERSION) {
                        uintptr_t foundAddr = addr + i;
                        if (ReadRaw(foundAddr, &m_localShm, sizeof(SharedGameData))) {
                            if (m_localShm.magic == EXT_SHM_MAGIC && m_localShm.version == EXT_SHM_VERSION) {
                                m_remoteShmAddr = foundAddr;
                                m_proxyBase = reportedBase ? reportedBase : start;
                                m_proxySize = static_cast<DWORD>(end - start);
                                m_shm = &m_localShm;
                                m_proxyConnected = true;
                                m_manualBaseFailed = false;
                                spdlog::info("Discovered payload SharedGameData at 0x{:X} (base 0x{:X}, size 0x{:X})",
                                             m_remoteShmAddr, m_proxyBase, m_proxySize);
                                return true;
                            }
                        }
                    }
                }
            }
        }
    }
    return false;
}

// ============================================================================
//  Shared Memory Relay — connects to the version.dll proxy's named section
// ============================================================================
bool Process::ConnectSharedMemoryRelay() {
    if (m_manualProxyBase != 0) {
        // Direct match: address points directly to SharedGameData struct
        SharedGameData directCandidate = {};
        if (ReadRaw(m_manualProxyBase, &directCandidate, sizeof(SharedGameData))) {
            if (directCandidate.magic == EXT_SHM_MAGIC && directCandidate.version == EXT_SHM_VERSION) {
                m_remoteShmAddr = m_manualProxyBase;
                m_proxyBase = directCandidate.proxyBase ? static_cast<uintptr_t>(directCandidate.proxyBase) : m_manualProxyBase;
                m_proxySize = sizeof(SharedGameData);
                m_localShm = directCandidate;
                m_shm = &m_localShm;
                m_proxyConnected = true;
                m_manualBaseFailed = false;
                spdlog::info("Manual base 0x{:X} directly matched SharedGameData", m_manualProxyBase);
                return true;
            }
        }

        // VirtualQueryEx scan across module memory regions starting at m_manualProxyBase (up to 32MB)
        uintptr_t current = m_manualProxyBase;
        uintptr_t maxAddr = m_manualProxyBase + 0x2000000;
        MEMORY_BASIC_INFORMATION mbi = {};
        bool scannedAny = false;
        while (current < maxAddr && VirtualQueryEx(m_handle, reinterpret_cast<LPCVOID>(current), &mbi, sizeof(mbi))) {
            if (mbi.State == MEM_COMMIT &&
                (mbi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)) &&
                !(mbi.Protect & PAGE_GUARD)) {
                scannedAny = true;
                uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
                if (ScanRegionForShm(reinterpret_cast<uintptr_t>(mbi.BaseAddress), regionEnd, m_manualProxyBase)) {
                    m_manualBaseFailed = false;
                    return true;
                }
            }
            current = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (mbi.RegionSize == 0) break;
        }

        if (!scannedAny) {
            if (ScanRegionForShm(m_manualProxyBase, m_manualProxyBase + 0x1000000, m_manualProxyBase)) {
                m_manualBaseFailed = false;
                return true;
            }
        }

        m_manualBaseFailed = true;
        m_proxyConnected = false;
        m_shm = nullptr;
        m_remoteShmAddr = 0;
        spdlog::warn("Manual proxy base 0x{:X} specified, but no valid SharedGameData found in that region", m_manualProxyBase);
        return false;
    }

    // Auto-scan / Named file mapping (when m_manualProxyBase == 0):
    m_manualBaseFailed = false;
    m_shmHandle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, EXT_SHM_NAME);
    if (!m_shmHandle) {
        // Fallback: Check if version.dll or any module holds SharedGameData
        uintptr_t vBase = GetModuleBase(L"version.dll");
        DWORD vSize = GetModuleSize(L"version.dll");
        if (!vBase) {
            vBase = GetModuleBase(L"version");
            vSize = GetModuleSize(L"version");
        }
        if (vBase && vSize > 0) {
            if (ScanRegionForShm(vBase, vBase + vSize, vBase)) {
                return true;
            }
        }
        spdlog::warn("Shared memory relay not available (proxy not loaded?) error={}", GetLastError());
        return false;
    }

    m_shm = static_cast<SharedGameData*>(
        MapViewOfFile(m_shmHandle, FILE_MAP_ALL_ACCESS, 0, 0, EXT_SHM_SIZE));

    if (!m_shm) {
        spdlog::warn("Failed to map shared memory view, error={}", GetLastError());
        CloseHandle(m_shmHandle);
        m_shmHandle = nullptr;
        return false;
    }

    // Verify magic and version
    if (m_shm->magic != EXT_SHM_MAGIC) {
        spdlog::warn("Shared memory magic mismatch: 0x{:X} (expected 0x{:X})",
            m_shm->magic, EXT_SHM_MAGIC);
        UnmapViewOfFile(m_shm);
        m_shm = nullptr;
        CloseHandle(m_shmHandle);
        m_shmHandle = nullptr;
        return false;
    }

    if (m_shm->version != EXT_SHM_VERSION) {
        spdlog::warn("Shared memory version mismatch: {} (expected {})",
            m_shm->version, EXT_SHM_VERSION);
    }

    // Verify PID matches
    if (m_shm->pid != m_pid) {
        spdlog::warn("Shared memory PID mismatch: {} (expected {})", m_shm->pid, m_pid);
    }

    m_proxyConnected = true;
    m_proxyBase = static_cast<uintptr_t>(m_shm->proxyBase);
    spdlog::info("Shared memory relay connected! heartbeat={}", m_shm->heartbeat);
    return m_shm->gameBase != 0;
}

bool Process::RelayRead(uintptr_t address, void* buffer, size_t size) const {
    if (!m_shm || size == 0) return false;

    // For large reads, split into chunks
    size_t offset = 0;
    while (offset < size) {
        size_t chunkSize = (std::min)(size - offset, (size_t)EXT_RELAY_MAX_SIZE);

        // Pick a relay slot (round-robin)
        int slotIdx = m_nextRelaySlot;
        m_nextRelaySlot = (m_nextRelaySlot + 1) % EXT_RELAY_SLOTS;
        auto& slot = m_shm->relay[slotIdx];

        // Wait for slot to be idle (with timeout)
        int waitCount = 0;
        while (slot.state != EXT_RELAY_IDLE && slot.state != EXT_RELAY_READY && slot.state != EXT_RELAY_ERROR) {
            if (++waitCount > 10000) {
                return false; // Timeout — proxy may be stuck
            }
            YieldProcessor();
        }

        // Post the read request
        slot.address = address + offset;
        slot.size = static_cast<uint32_t>(chunkSize);
        MemoryBarrier();
        slot.state = EXT_RELAY_REQUEST;

        // Wait for response (with timeout)
        waitCount = 0;
        while (slot.state == EXT_RELAY_REQUEST) {
            if (++waitCount > 1000000) {
                slot.state = EXT_RELAY_IDLE; // Reset and fail
                return false;
            }
            YieldProcessor();
        }

        if (slot.state == EXT_RELAY_ERROR) {
            slot.state = EXT_RELAY_IDLE;
            return false;
        }

        // Copy the result
        memcpy(static_cast<uint8_t*>(buffer) + offset, slot.data, chunkSize);
        slot.state = EXT_RELAY_IDLE;

        offset += chunkSize;
    }

    return true;
}

bool Process::HasProxyModule(uintptr_t* outBase, DWORD* outSize) const {
    if (m_proxyConnected && m_proxyBase != 0) {
        if (outBase) *outBase = m_proxyBase;
        if (outSize) *outSize = m_proxySize;
        return true;
    }
    uintptr_t base = GetModuleBase(L"version.dll");
    DWORD size = GetModuleSize(L"version.dll");
    if (!base) {
        base = GetModuleBase(L"version");
        size = GetModuleSize(L"version");
    }
    if (base) {
        if (outBase) *outBase = base;
        if (outSize) *outSize = size;
        return true;
    }
    return false;
}

void Process::UpdateProxySync() {
    if (m_forceProxyRescan) {
        m_forceProxyRescan = false;
        ConnectSharedMemoryRelay();
    }

    if (m_remoteShmAddr != 0) {
        if (ReadRaw(m_remoteShmAddr, &m_localShm, sizeof(SharedGameData))) {
            if (m_localShm.magic == EXT_SHM_MAGIC && m_localShm.version == EXT_SHM_VERSION) {
                m_shm = &m_localShm;
                m_proxyConnected = true;
                return;
            }
        }
        m_remoteShmAddr = 0;
        m_shm = nullptr;
        m_proxyConnected = false;
    } else if (m_shm && m_shmHandle) {
        if (m_shm->magic == EXT_SHM_MAGIC && m_shm->version == EXT_SHM_VERSION) {
            m_proxyConnected = true;
            return;
        }
        m_proxyConnected = false;
    } else {
        static int retryCounter = 0;
        if (retryCounter++ % 30 == 0) {
            ConnectSharedMemoryRelay();
        }
    }
}

void Process::SyncProxyTarget(const SilentHelperIPC& targetConfig) {
    if (!m_proxyConnected) return;

    constexpr size_t INPUT_SIZE = offsetof(SilentHelperIPC, isRedirecting);
    if (m_remoteShmAddr != 0) {
        uintptr_t targetOffset = m_remoteShmAddr + offsetof(SharedGameData, silentHelper);
        WriteRaw(targetOffset, &targetConfig, INPUT_SIZE);
    } else if (m_shm) {
        std::memcpy(const_cast<SilentHelperIPC*>(&m_shm->silentHelper), &targetConfig, INPUT_SIZE);
    }
}

DWORD Process::FindProcessId(const std::wstring& processName) {
    return static_cast<DWORD>(FindProcessIdByName(processName));
}
