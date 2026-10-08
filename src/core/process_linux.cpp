#ifndef _WIN32

#include "process.h"
#include "process_finder.h"
#include "../shared/shared_memory.h"

#include <algorithm>
#include <cerrno>
#include <cwctype>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <sys/uio.h>
#include <unistd.h>

#include <spdlog/spdlog.h>

namespace {

std::wstring ToWide(const std::string& value) {
    return std::wstring(value.begin(), value.end());
}

std::wstring BaseName(const std::wstring& value) {
    const auto slash = value.find_last_of(L"/\\");
    return slash == std::wstring::npos ? value : value.substr(slash + 1);
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring WithoutExe(std::wstring value) {
    value = Lower(BaseName(value));
    if (value.size() > 4 && value.substr(value.size() - 4) == L".exe")
        value.resize(value.size() - 4);
    return value;
}

bool ParseMapLine(const std::string& line, uintptr_t& start, uintptr_t& end,
                  std::string& path) {
    std::istringstream stream(line);
    std::string range;
    std::string permissions;
    std::string offset;
    std::string device;
    std::string inode;
    if (!(stream >> range >> permissions >> offset >> device >> inode))
        return false;

    const auto dash = range.find('-');
    if (dash == std::string::npos)
        return false;

    try {
        start = std::stoull(range.substr(0, dash), nullptr, 16);
        end = std::stoull(range.substr(dash + 1), nullptr, 16);
    } catch (...) {
        return false;
    }

    std::getline(stream, path);
    while (!path.empty() && path.front() == ' ')
        path.erase(path.begin());
    return true;
}

} // namespace

Process::~Process() {
    Detach();
}

bool Process::Attach(const std::wstring& processName) {
    const DWORD pid = FindProcessId(processName);
    if (!pid) {
        spdlog::warn("Process not found");
        return false;
    }
    m_processName = processName;
    return Attach(pid);
}

bool Process::Attach(DWORD pid) {
    Detach();

    if (!pid || pid > static_cast<DWORD>(std::numeric_limits<pid_t>::max())) {
        spdlog::error("Invalid process ID {}", pid);
        return false;
    }

    if (kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM) {
        spdlog::error("Process {} is not accessible: {}", pid, std::strerror(errno));
        return false;
    }

    m_pid = pid;
    m_handle = static_cast<HANDLE>(pid);

    // Kernel driver is mandatory — no process_vm_readv fallback.
    if (!m_kernelDriver.Open()) {
        const int error = errno;
        if (error == EPROTONOSUPPORT) {
            spdlog::error("Kernel driver is outdated; rebuild and reload ext_mem.ko");
        } else if (error == EACCES || error == EPERM) {
            const char* dev = getenv(EXT_MEM_DEVICE_ENV);
            if (!dev) dev = EXT_MEM_DEVICE;
            spdlog::error("Access denied opening {} (UID={}); run via run-linux.sh",
                          dev, geteuid());
        } else if (error == ENOENT) {
            spdlog::error("Kernel device missing; run via run-linux.sh to load module");
        } else {
            spdlog::error("Kernel driver unavailable: errno={} ({})",
                          error, std::strerror(error));
        }
        return false;
    }
    spdlog::info("Using kernel driver ABI {} for memory access (64 KiB chunks)",
                 EXT_MEM_ABI_VERSION);

    if (!EnumerateModules()) {
        spdlog::error("Failed to enumerate modules for PID {}", pid);
        Detach();
        return false;
    }

    spdlog::info("Attached to PID {} with {} modules", pid, m_modules.size());
    return true;
}

void Process::Detach() {
    m_kernelDriver.Close();
    m_handle = -1;
    m_pid = 0;
    m_modules.clear();
    m_useRelay = false;
}

uintptr_t Process::GetModuleBase(const std::wstring& moduleName) const {
    const std::wstring requested = WithoutExe(moduleName);
    for (const auto& [name, info] : m_modules) {
        if (WithoutExe(name) == requested)
            return info.base;
    }
    return 0;
}

DWORD Process::GetModuleSize(const std::wstring& moduleName) const {
    const std::wstring requested = WithoutExe(moduleName);
    for (const auto& [name, info] : m_modules) {
        if (WithoutExe(name) == requested)
            return info.size;
    }
    return 0;
}

bool Process::ReadRaw(uintptr_t address, void* buffer, size_t size) const {
    if (!m_pid || !address || !buffer || !size)
        return false;
    return m_kernelDriver.Read(m_pid, address, buffer, size);
}

bool Process::WriteRaw(uintptr_t address, const void* buffer, size_t size) const {
    if (!m_pid || !address || !buffer || !size)
        return false;
    return m_kernelDriver.Write(m_pid, address, buffer, size);
}

uintptr_t Process::ReadPointerChain(uintptr_t base,
                                     const std::vector<uintptr_t>& offsets) const {
    uintptr_t address = base;
    for (const uintptr_t offset : offsets) {
        address = Read<uintptr_t>(address);
        if (!address)
            return 0;
        address += offset;
    }
    return address;
}

bool Process::IsValid() const {
    return m_pid != 0 &&
        (kill(static_cast<pid_t>(m_pid), 0) == 0 || errno == EPERM);
}

bool Process::EnumerateModules() {
    m_modules.clear();

    // Module discovery is independent of the selected memory backend.
    std::ifstream maps("/proc/" + std::to_string(m_pid) + "/maps");
    if (!maps)
        return false;

    std::unordered_map<std::wstring, ModuleInfo> byPath;
    std::string line;
    while (std::getline(maps, line)) {
        uintptr_t start = 0;
        uintptr_t end = 0;
        std::string path;
        if (!ParseMapLine(line, start, end, path) || path.empty() || path[0] != '/')
            continue;

        const std::wstring fullPath = ToWide(path);
        const std::wstring name = BaseName(fullPath);
        if (name.empty())
            continue;

        auto& info = byPath[fullPath];
        if (!info.base || start < info.base)
            info.base = start;
        const uintptr_t mappedSize = end > info.base ? end - info.base : 0;
        if (mappedSize > info.size)
            info.size = static_cast<DWORD>((std::min)(mappedSize,
                                                       static_cast<uintptr_t>(std::numeric_limits<DWORD>::max())));
        info.name = name;
    }

    for (const auto& [path, info] : byPath)
        m_modules[info.name] = info;

    return !m_modules.empty();
}

bool Process::HasProxyModule(uintptr_t* outBase, DWORD* outSize) const {
    if (m_proxyConnected && m_proxyBase != 0) {
        if (outBase) *outBase = m_proxyBase;
        if (outSize) *outSize = m_proxySize;
        return true;
    }
    return false;
}

void Process::SetManualProxyBase(uintptr_t base) {
    if (m_manualProxyBase == base && !m_forceProxyRescan) {
        return;
    }
    spdlog::info("Setting manual proxy base address: 0x{:X}", base);
    m_manualProxyBase = base;
    m_forceProxyRescan = true;
    m_manualBaseFailed = false;

    // Invalidate existing connection to force immediate rediscovery
    m_remoteShmAddr = 0;
    m_proxyConnected = false;
    m_shm = nullptr;

    if (m_pid != 0) {
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

bool Process::ConnectSharedMemoryRelay() {
    if (!m_pid) {
        if (m_manualProxyBase != 0) {
            m_manualBaseFailed = true;
        }
        return false;
    }

    // 1. If we already found the remote SHM address and it's valid, verify
    if (!m_forceProxyRescan && m_remoteShmAddr != 0) {
        if (ReadRaw(m_remoteShmAddr, &m_localShm, sizeof(SharedGameData))) {
            if (m_localShm.magic == EXT_SHM_MAGIC && m_localShm.version == EXT_SHM_VERSION) {
                m_shm = &m_localShm;
                m_proxyConnected = true;
                m_manualBaseFailed = false;
                return true;
            }
        }
        m_remoteShmAddr = 0;
        m_proxyConnected = false;
        m_shm = nullptr;
    }
    m_forceProxyRescan = false;

    // 2. If a manual base address was specified by the user / menu
    if (m_manualProxyBase != 0) {
        // Direct match: address points directly to SharedGameData
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

        // Scan memory segments in /proc/<pid>/maps belonging to the module at m_manualProxyBase (up to 32MB)
        std::ifstream maps("/proc/" + std::to_string(m_pid) + "/maps");
        bool scannedAny = false;
        if (maps) {
            std::string line;
            std::vector<std::pair<uintptr_t, uintptr_t>> priorityAnon;
            std::vector<std::pair<uintptr_t, uintptr_t>> otherSegments;
            while (std::getline(maps, line)) {
                std::istringstream stream(line);
                std::string range, permissions, offset, device, inode, path;
                if (!(stream >> range >> permissions >> offset >> device >> inode))
                    continue;
                if (permissions.empty() || permissions[0] != 'r')
                    continue;

                auto dash = range.find('-');
                if (dash == std::string::npos) continue;

                uintptr_t segStart = 0, segEnd = 0;
                try {
                    segStart = std::stoull(range.substr(0, dash), nullptr, 16);
                    segEnd = std::stoull(range.substr(dash + 1), nullptr, 16);
                } catch (...) {
                    continue;
                }

                if (segEnd > m_manualProxyBase && segStart < (m_manualProxyBase + 0x2000000)) {
                    std::getline(stream, path);
                    while (!path.empty() && path.front() == ' ') path.erase(path.begin());
                    bool isAnon = path.empty() || path == "[anon]" || path == "[heap]";
                    bool isWritable = permissions.size() >= 2 && permissions[1] == 'w';
                    if (isAnon && isWritable) {
                        priorityAnon.push_back({segStart, segEnd});
                    } else {
                        otherSegments.push_back({segStart, segEnd});
                    }
                }
            }

            for (const auto& [segStart, segEnd] : priorityAnon) {
                scannedAny = true;
                if (ScanRegionForShm(segStart, segEnd, m_manualProxyBase)) {
                    m_manualBaseFailed = false;
                    return true;
                }
            }
            for (const auto& [segStart, segEnd] : otherSegments) {
                scannedAny = true;
                if (ScanRegionForShm(segStart, segEnd, m_manualProxyBase)) {
                    m_manualBaseFailed = false;
                    return true;
                }
            }
        }

        if (!scannedAny) {
            // Direct scan range without /proc/maps
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

    // 3. Auto-scan (m_manualProxyBase == 0):
    m_manualBaseFailed = false;
    std::ifstream maps("/proc/" + std::to_string(m_pid) + "/maps");
    if (!maps) return false;

    struct MemSegment {
        uintptr_t start = 0;
        uintptr_t end = 0;
        bool isAnonymous = false;
        bool isWritable = false;
        std::string path;
    };

    std::vector<MemSegment> priorityWritableAnon;
    std::vector<MemSegment> priorityOtherAnon;
    std::vector<MemSegment> fallbackSegments;

    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream stream(line);
        std::string range, permissions, offset, device, inode, path;
        if (!(stream >> range >> permissions >> offset >> device >> inode))
            continue;

        std::getline(stream, path);
        while (!path.empty() && path.front() == ' ')
            path.erase(path.begin());

        // Skip any non-readable segment
        if (permissions.empty() || permissions[0] != 'r')
            continue;

        // Only scan private ('p') or shared ('s') memory
        if (permissions.size() >= 4 && permissions[3] != 'p' && permissions[3] != 's')
            continue;

        // Skip Linux system libraries and drivers that cannot hold our Windows payload
        if (!path.empty()) {
            if (path.rfind("/usr/", 0) == 0 ||
                path.rfind("/lib/", 0) == 0 ||
                path.rfind("/lib64/", 0) == 0 ||
                path.rfind("/dev/", 0) == 0 ||
                path == "[vsyscall]" || path == "[vdso]" || path == "[vvar]") {
                continue;
            }
            // Skip Wine's built-in system libraries (e.g. Proton's internal version.dll)
            if (path.find("/files/lib/wine/") != std::string::npos ||
                path.find("/files/lib64/wine/") != std::string::npos) {
                continue;
            }
        }

        auto dash = range.find('-');
        if (dash == std::string::npos) continue;

        uintptr_t segStart = 0, segEnd = 0;
        try {
            segStart = std::stoull(range.substr(0, dash), nullptr, 16);
            segEnd = std::stoull(range.substr(dash + 1), nullptr, 16);
        } catch (...) {
            continue;
        }

        if (segEnd <= segStart || (segEnd - segStart) > 128 * 1024 * 1024)
            continue;

        MemSegment seg;
        seg.start = segStart;
        seg.end = segEnd;
        seg.isAnonymous = path.empty() || path == "[anon]" || path == "[heap]";
        seg.isWritable = (permissions.size() >= 2 && permissions[1] == 'w');
        seg.path = path;

        if (seg.isAnonymous && seg.isWritable) {
            priorityWritableAnon.push_back(seg);
        } else if (seg.isAnonymous) {
            priorityOtherAnon.push_back(seg);
        } else {
            fallbackSegments.push_back(seg);
        }
    }

    // 1. First scan anonymous writable segments (fastest: exact location of g_shmBuffer in .data)
    for (const auto& seg : priorityWritableAnon) {
        if (ScanRegionForShm(seg.start, seg.end, seg.start)) return true;
    }

    // 2. Next scan other anonymous segments
    for (const auto& seg : priorityOtherAnon) {
        if (ScanRegionForShm(seg.start, seg.end, seg.start)) return true;
    }

    // 3. Next scan remaining candidates (e.g. file-backed custom DLLs)
    for (const auto& seg : fallbackSegments) {
        if (ScanRegionForShm(seg.start, seg.end, seg.start)) return true;
    }

    m_proxyConnected = false;
    return false;
}

void Process::UpdateProxySync() {
    // Render-thread fast path: just validate the already-found SHM address.
    // ConnectSharedMemoryRelay() (expensive) is called by TryConnectProxy()
    // from the background scan thread so the render thread is never blocked.
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
    }
    m_proxyConnected = false;
}

void Process::TryConnectProxy() {
    // Background scan thread only. Throttled to avoid burning CPU on the scan.
    if (m_remoteShmAddr != 0 && !m_forceProxyRescan) return;

    static auto s_lastScan = std::chrono::steady_clock::time_point{};
    bool force = m_forceProxyRescan;
    if (!force) {
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_lastScan).count() < 3000) return;
        s_lastScan = now;
    }

    ConnectSharedMemoryRelay();
}

void Process::SyncProxyTarget(const SilentHelperIPC& targetConfig) {
    if (!m_proxyConnected || m_remoteShmAddr == 0) return;

    // Write only input parameters up to isRedirecting, leaving proxy feedback & analytics intact
    constexpr size_t INPUT_SIZE = offsetof(SilentHelperIPC, isRedirecting);
    uintptr_t targetOffset = m_remoteShmAddr + offsetof(SharedGameData, silentHelper);
    WriteRaw(targetOffset, &targetConfig, INPUT_SIZE);
}

bool Process::RelayRead(uintptr_t, void*, size_t) const {
    return false;
}

DWORD Process::FindProcessId(const std::wstring& processName) {
    return static_cast<DWORD>(FindProcessIdByName(processName));
}

#endif
