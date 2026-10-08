#include "core/process.h"

#include <algorithm>
#include <cstdarg>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <sys/uio.h>
#include <spdlog/sinks/ostream_sink.h>

#define CHECK(condition) do { \
    if (!(condition)) throw std::runtime_error( \
        std::string(#condition) + " at line " + std::to_string(__LINE__)); \
} while (false)

namespace {
constexpr uintptr_t RemoteBase = 0x100000;

struct FakeDevice {
    int nextFd = 1000;
    int openError = 0;
    int infoError = 0;
    int openFlags = 0;
    int openCalls = 0;
    int closeCalls = 0;
    int infoCalls = 0;
    int failCall = 0;
    int transferError = EFAULT;
    int unexpectedResult = 0;
    bool partialFailure = false;
    bool shortSyscall = false;
    int syscallReads = 0;
    int syscallWrites = 0;
    ext_mem_info info{EXT_MEM_ABI_VERSION, EXT_MEM_MAX_TRANSFER};
    std::set<int> fds;
    std::vector<ext_mem_req> requests;
    std::vector<uint8_t> memory = std::vector<uint8_t>(3 * EXT_MEM_MAX_TRANSFER + 127);
} fake;

void Reset() {
    CHECK(fake.fds.empty());
    fake = FakeDevice{};
    for (size_t i = 0; i < fake.memory.size(); ++i)
        fake.memory[i] = static_cast<uint8_t>(i % 251);
}

void ChunkedReads() {
    KernelDriver driver;
    CHECK(driver.Open());
    CHECK(fake.openFlags & O_CLOEXEC);
    CHECK(driver.Open());
    CHECK(fake.openCalls == 1 && fake.infoCalls == 1);

    for (size_t size : {size_t{8}, size_t{EXT_MEM_MAX_TRANSFER},
                       size_t{EXT_MEM_MAX_TRANSFER + 1},
                       size_t{2 * EXT_MEM_MAX_TRANSFER + 73}}) {
        fake.requests.clear();
        std::vector<uint8_t> buffer(size);
        CHECK(driver.Read(getpid(), RemoteBase, buffer.data(), size));
        CHECK(std::equal(buffer.begin(), buffer.end(), fake.memory.begin()));
        CHECK(fake.requests.size() == (size + EXT_MEM_MAX_TRANSFER - 1) / EXT_MEM_MAX_TRANSFER);
        size_t completed = 0;
        for (const auto& req : fake.requests) {
            CHECK(req.addr == RemoteBase + completed);
            CHECK(req.user_buf == reinterpret_cast<uintptr_t>(buffer.data()) + completed);
            CHECK(req.size == std::min(size - completed, size_t{EXT_MEM_MAX_TRANSFER}));
            completed += req.size;
        }
        CHECK(completed == size);
    }
}

void ChunkedWrites() {
    KernelDriver driver;
    CHECK(driver.Open());
    std::vector<uint8_t> buffer(2 * EXT_MEM_MAX_TRANSFER + 73, 0x5A);
    CHECK(driver.Write(getpid(), RemoteBase, buffer.data(), buffer.size()));
    CHECK(fake.requests.size() == 3);
    CHECK(std::equal(buffer.begin(), buffer.end(), fake.memory.begin()));
}

void NegotiatedChunkSize() {
    fake.info.max_transfer = 4096;
    KernelDriver driver;
    CHECK(driver.Open());
    std::vector<uint8_t> buffer(8193);
    CHECK(driver.Read(getpid(), RemoteBase, buffer.data(), buffer.size()));
    CHECK(fake.requests.size() == 3);
    CHECK(fake.requests[0].size == 4096 && fake.requests[2].size == 1);
    CHECK(std::equal(buffer.begin(), buffer.end(), fake.memory.begin()));
}

void Validation() {
    KernelDriver driver;
    uint64_t value = 0;
    CHECK(!driver.Read(getpid(), RemoteBase, value) && errno == EBADF);
    CHECK(driver.Open());
    CHECK(!driver.Read(0, RemoteBase, value) && errno == EINVAL);
    CHECK(!driver.Read(UINT32_MAX, RemoteBase, value) && errno == EINVAL);
    CHECK(!driver.Read(getpid(), 0, value) && errno == EINVAL);
    CHECK(!driver.Read(getpid(), RemoteBase, nullptr, 8) && errno == EINVAL);
    CHECK(!driver.Read(getpid(), RemoteBase, &value, 0) && errno == EINVAL);
    CHECK(!driver.Read(getpid(), UINTPTR_MAX - 3, &value, 8) && errno == EOVERFLOW);
    CHECK(!driver.Read(getpid(), RemoteBase, reinterpret_cast<void*>(UINTPTR_MAX - 3), 8)
          && errno == EOVERFLOW);
    CHECK(fake.requests.empty());
}

void StopsOnFailure() {
    KernelDriver driver;
    CHECK(driver.Open());
    fake.failCall = 2;
    std::vector<uint8_t> buffer(2 * EXT_MEM_MAX_TRANSFER + 73, 0xA5);
    CHECK(!driver.Read(getpid(), RemoteBase, buffer.data(), buffer.size()));
    CHECK(errno == EFAULT);
    CHECK(fake.requests.size() == 2);
    CHECK(std::equal(buffer.begin(), buffer.begin() + EXT_MEM_MAX_TRANSFER, fake.memory.begin()));
    CHECK(std::all_of(buffer.begin() + EXT_MEM_MAX_TRANSFER, buffer.end(),
                      [](uint8_t b) { return b == 0xA5; }));
}

void RejectsIncompatibleDrivers() {
    for (int test = 0; test < 4; ++test) {
        Reset();
        if (test == 0) fake.infoError = ENOTTY;
        if (test == 1) fake.info.abi_version = 1;
        if (test == 2) fake.info.max_transfer = 0;
        if (test == 3) fake.info.max_transfer = EXT_MEM_MAX_TRANSFER + 1;
        KernelDriver driver;
        CHECK(!driver.Open());
        CHECK(errno == (test < 2 ? EPROTONOSUPPORT : EPROTO));
        CHECK(!driver.IsOpen());
        CHECK(fake.fds.empty() && fake.closeCalls == 1);
    }
    Reset();
    KernelDriver driver;
    CHECK(driver.Open());
    fake.unexpectedResult = 1;
    uint64_t value = 0;
    CHECK(!driver.Read(getpid(), RemoteBase, value) && errno == EPROTO);
}

void ConnectionsAreIndependent() {
    Process first;
    Process second;
    CHECK(first.Attach(getpid()));
    CHECK(second.Attach(getpid()));
    CHECK(fake.fds.size() == 2);
    first.Detach();
    CHECK(fake.fds.size() == 1);
    uint64_t value = 0;
    CHECK(second.ReadRaw(RemoteBase, &value, sizeof(value)));
    second.Detach();
    CHECK(fake.fds.empty());
    second.Detach();
    CHECK(fake.closeCalls == 2);
}

void NoPerOperationFallback() {
    Process process;
    CHECK(process.Attach(getpid()));
    fake.failCall = 2;
    fake.transferError = EIO;
    fake.partialFailure = true;
    std::vector<uint8_t> buffer(2 * EXT_MEM_MAX_TRANSFER + 73, 0x5A);
    CHECK(!process.WriteRaw(RemoteBase, buffer.data(), buffer.size()));
    CHECK(errno == EIO);
    CHECK(fake.requests.size() == 2);
    CHECK(fake.syscallWrites == 0);
    CHECK(fake.memory[0] == 0x5A);
    CHECK(fake.memory[EXT_MEM_MAX_TRANSFER] == 0x5A);
    CHECK(fake.memory[2 * EXT_MEM_MAX_TRANSFER] != 0x5A);

    fake.requests.clear();
    fake.failCall = 1;
    CHECK(process.Read<uint64_t>(RemoteBase) == 0);
    CHECK(errno == EIO && fake.syscallReads == 0);
    // Recovery stays on the driver rather than switching backend after a fault.
    fake.failCall = 0;
    CHECK(process.ReadRaw(RemoteBase, buffer.data(), 8));
    CHECK(fake.syscallReads == 0);
}

void AttachmentTimeFallback() {
    for (bool outdated : {false, true}) {
        Reset();
        if (outdated) fake.infoError = ENOTTY;
        else fake.openError = ENOENT;
        Process process;
        CHECK(process.Attach(getpid()));
        uint64_t value = 0;
        CHECK(process.ReadRaw(RemoteBase, &value, sizeof(value)));
        CHECK(fake.syscallReads == 1 && fake.requests.empty());
        CHECK(process.Write(RemoteBase, value));
        CHECK(fake.syscallWrites == 1);
        fake.shortSyscall = true;
        CHECK(process.Read<uint64_t>(RemoteBase) == 0);
    }
}

void WarningThrottle() {
    std::ostringstream output;
    auto previous = spdlog::default_logger();
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    auto logger = std::make_shared<spdlog::logger>("test-driver", sink);
    logger->set_level(spdlog::level::warn);
    spdlog::set_default_logger(logger);
    KernelDriver driver;
    CHECK(driver.Open());
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&] {
            uint64_t value = 0;
            for (int j = 0; j < 100; ++j)
                driver.Read(0, RemoteBase, value);
        });
    }
    for (auto& thread : threads) thread.join();
    spdlog::set_default_logger(previous);
    const std::string text = output.str();
    const size_t first = text.find("[KernelDriver]");
    CHECK(first != std::string::npos);
    CHECK(text.find("[KernelDriver]", first + 1) == std::string::npos);
    CHECK(text.find("errno=22") != std::string::npos);
    CHECK(fake.requests.empty());
}

ssize_t SyscallTransfer(bool write, const iovec* local, unsigned long localCount,
                        const iovec* remote, unsigned long remoteCount) {
    CHECK(localCount == 1 && remoteCount == 1);
    CHECK(local[0].iov_len == remote[0].iov_len);
    const auto addr = reinterpret_cast<uintptr_t>(remote[0].iov_base);
    CHECK(addr >= RemoteBase);
    const size_t offset = addr - RemoteBase;
    size_t size = local[0].iov_len;
    CHECK(offset + size <= fake.memory.size());
    if (fake.shortSyscall) size /= 2;
    if (write) {
        ++fake.syscallWrites;
        std::memcpy(fake.memory.data() + offset, local[0].iov_base, size);
    } else {
        ++fake.syscallReads;
        std::memcpy(local[0].iov_base, fake.memory.data() + offset, size);
    }
    return size;
}
} // namespace

extern "C" int __real_open(const char*, int, ...);
extern "C" int __real_close(int);
extern "C" int __real_ioctl(int, unsigned long, ...);

extern "C" int __wrap_open(const char* path, int flags, ...) {
    if (std::strcmp(path, EXT_MEM_DEVICE) != 0) {
        mode_t mode = 0;
        if (flags & O_CREAT) {
            va_list args;
            va_start(args, flags);
            mode = va_arg(args, mode_t);
            va_end(args);
        }
        return __real_open(path, flags, mode);
    }
    ++fake.openCalls;
    fake.openFlags = flags;
    if (fake.openError) { errno = fake.openError; return -1; }
    const int fd = fake.nextFd++;
    fake.fds.insert(fd);
    return fd;
}

extern "C" int __wrap_close(int fd) {
    if (!fake.fds.erase(fd)) return __real_close(fd);
    ++fake.closeCalls;
    // Successful close may change errno; handshake failures must preserve it.
    errno = EBADF;
    return 0;
}

extern "C" int __wrap_ioctl(int fd, unsigned long cmd, ...) {
    va_list args;
    va_start(args, cmd);
    void* arg = va_arg(args, void*);
    va_end(args);
    if (!fake.fds.contains(fd)) return __real_ioctl(fd, cmd, arg);
    if (cmd == EXT_IOCTL_GET_INFO) {
        ++fake.infoCalls;
        if (fake.infoError) { errno = fake.infoError; return -1; }
        *static_cast<ext_mem_info*>(arg) = fake.info;
        return 0;
    }
    CHECK(cmd == EXT_IOCTL_READ || cmd == EXT_IOCTL_WRITE);
    const auto req = *static_cast<ext_mem_req*>(arg);
    CHECK(req.reserved == 0 && req.pid == static_cast<uint32_t>(getpid()));
    CHECK(req.size > 0 && req.size <= fake.info.max_transfer);
    CHECK(req.addr >= RemoteBase);
    const size_t offset = req.addr - RemoteBase;
    CHECK(offset + req.size <= fake.memory.size());
    fake.requests.push_back(req);
    if (fake.unexpectedResult) return fake.unexpectedResult;
    const bool fail = static_cast<int>(fake.requests.size()) == fake.failCall;
    if (!fail || fake.partialFailure) {
        const size_t size = fail ? req.size / 2 : req.size;
        void* buffer = reinterpret_cast<void*>(req.user_buf);
        if (cmd == EXT_IOCTL_READ)
            std::memcpy(buffer, fake.memory.data() + offset, size);
        else
            std::memcpy(fake.memory.data() + offset, buffer, size);
    }
    if (fail) { errno = fake.transferError; return -1; }
    return 0;
}

extern "C" ssize_t __wrap_process_vm_readv(pid_t pid, const iovec* local,
        unsigned long localCount, const iovec* remote, unsigned long remoteCount,
        unsigned long flags) {
    CHECK(pid == getpid() && flags == 0);
    return SyscallTransfer(false, local, localCount, remote, remoteCount);
}

extern "C" ssize_t __wrap_process_vm_writev(pid_t pid, const iovec* local,
        unsigned long localCount, const iovec* remote, unsigned long remoteCount,
        unsigned long flags) {
    CHECK(pid == getpid() && flags == 0);
    return SyscallTransfer(true, local, localCount, remote, remoteCount);
}

int main() {
    spdlog::set_level(spdlog::level::off);
    const std::pair<const char*, void(*)()> tests[] = {
        {"chunked reads and exact boundaries", ChunkedReads},
        {"chunked writes", ChunkedWrites},
        {"negotiated chunk size", NegotiatedChunkSize},
        {"argument and address-overflow validation", Validation},
        {"stop at first failed chunk and preserve errno", StopsOnFailure},
        {"ABI handshake and unexpected ioctl results", RejectsIncompatibleDrivers},
        {"per-process descriptor ownership", ConnectionsAreIndependent},
        {"no replay/fallback after a partial driver operation", NoPerOperationFallback},
        {"fallback only at attachment and failed typed reads", AttachmentTimeFallback},
        {"thread-safe warning throttle", WarningThrottle},
    };
    try {
        for (const auto& [name, test] : tests) {
            Reset();
            test();
            CHECK(fake.fds.empty());
            std::cout << "PASS: " << name << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
