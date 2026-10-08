#pragma once
#include "../../kernel_driver/ext_mem_uapi.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <spdlog/spdlog.h>

static_assert(sizeof(ext_mem_req) == 32);
static_assert(offsetof(ext_mem_req, addr) == 8);
static_assert(sizeof(ext_mem_info) == 8);

class KernelDriver {
public:
    KernelDriver() = default;
    ~KernelDriver() { Close(); }

    KernelDriver(const KernelDriver&) = delete;
    KernelDriver& operator=(const KernelDriver&) = delete;

    bool Open() {
        if (fd_ >= 0) return true;

        const char* devEnv = getenv(EXT_MEM_DEVICE_ENV);
        const int fd = open(devEnv ? devEnv : EXT_MEM_DEVICE, O_RDWR | O_CLOEXEC);
        if (fd < 0) return false;

        ext_mem_info info{};
        const int result = ioctl(fd, EXT_IOCTL_GET_INFO, &info);
        int error = 0;
        if (result < 0) {
            error = errno == ENOTTY ? EPROTONOSUPPORT : errno;
        } else if (result != 0 || !info.max_transfer ||
                   info.max_transfer > EXT_MEM_MAX_TRANSFER) {
            error = EPROTO;
        } else if (info.abi_version != EXT_MEM_ABI_VERSION) {
            error = EPROTONOSUPPORT;
        }

        if (error) {
            close(fd);
            errno = error;
            return false;
        }

        fd_ = fd;
        maxTransfer_ = info.max_transfer;
        nextWarningMs_.store(0, std::memory_order_relaxed);
        suppressedWarnings_.store(0, std::memory_order_relaxed);
        return true;
    }

    void Close() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
            maxTransfer_ = 0;
        }
    }

    bool IsOpen() const { return fd_ >= 0; }

    // A failed multi-chunk transfer may have completed earlier chunks. Callers
    // must check the result; writes must not be replayed through another backend.
    bool Read(uint32_t pid, uintptr_t addr, void* buf, size_t size) const {
        return Transfer(EXT_IOCTL_READ, pid, addr,
                        reinterpret_cast<uintptr_t>(buf), size);
    }

    bool Write(uint32_t pid, uintptr_t addr, const void* buf, size_t size) const {
        return Transfer(EXT_IOCTL_WRITE, pid, addr,
                        reinterpret_cast<uintptr_t>(buf), size);
    }

    template<typename T>
    bool Read(uint32_t pid, uintptr_t addr, T& value) const {
        return Read(pid, addr, &value, sizeof(T));
    }

    template<typename T>
    bool Write(uint32_t pid, uintptr_t addr, const T& value) const {
        return Write(pid, addr, &value, sizeof(T));
    }

private:
    bool Transfer(unsigned long cmd, uint32_t pid, uintptr_t addr,
                  uintptr_t buffer, size_t size) const {
        int error = 0;
        if (fd_ < 0) {
            error = EBADF;
        } else if (!pid || pid > static_cast<uint32_t>(std::numeric_limits<pid_t>::max()) ||
                   !addr || !buffer || !size) {
            error = EINVAL;
        } else if (size - 1 > std::numeric_limits<uintptr_t>::max() - addr ||
                   size - 1 > std::numeric_limits<uintptr_t>::max() - buffer) {
            error = EOVERFLOW;
        }

        if (error)
            return ReportFailure(cmd, pid, addr, size, 0, error);

        size_t completed = 0;
        while (completed < size) {
            const size_t chunk = (std::min)(size - completed, maxTransfer_);
            ext_mem_req req{};
            req.pid = pid;
            req.addr = addr + completed;
            req.size = chunk;
            req.user_buf = buffer + completed;

            const int result = ioctl(fd_, cmd, &req);
            if (result != 0) {
                error = result < 0 ? errno : EPROTO;
                return ReportFailure(cmd, pid, addr + completed, size,
                                     completed, error);
            }
            completed += chunk;
        }
        return true;
    }

    bool ReportFailure(unsigned long cmd, uint32_t pid, uintptr_t addr,
                       size_t size, size_t completed, int error) const {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        int64_t next = nextWarningMs_.load(std::memory_order_relaxed);
        if (now >= next && nextWarningMs_.compare_exchange_strong(
                next, now + 5000, std::memory_order_relaxed)) {
            const size_t suppressed = suppressedWarnings_.exchange(0, std::memory_order_relaxed);
            spdlog::warn("[KernelDriver] {} failed: pid={} addr=0x{:X} size={} "
                         "completed={} errno={} ({}) suppressed={}; no per-operation fallback",
                         cmd == EXT_IOCTL_READ ? "Read" : "Write", pid, addr,
                         size, completed, error, std::strerror(error), suppressed);
        } else {
            suppressedWarnings_.fetch_add(1, std::memory_order_relaxed);
        }
        errno = error;
        return false;
    }

    int fd_ = -1;
    size_t maxTransfer_ = 0;
    mutable std::atomic<int64_t> nextWarningMs_{0};
    mutable std::atomic<size_t> suppressedWarnings_{0};
};
