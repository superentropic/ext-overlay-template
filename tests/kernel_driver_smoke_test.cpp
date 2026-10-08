#include "core/kernel_driver.h"

#include <iostream>
#include <vector>
#include <sys/mman.h>

int main() {
    KernelDriver driver;
    if (!driver.Open()) {
        const int error = errno;
        std::cout << "SKIP: compatible driver unavailable: " << std::strerror(error);
        if (error == EPROTONOSUPPORT)
            std::cout << " (loaded module needs manual rebuild/reload)";
        std::cout << '\n';
        return 77;
    }

    // No game/other process is accessed, and no module is loaded by this test.
    std::vector<uint8_t> source(2 * EXT_MEM_MAX_TRANSFER + 37);
    for (size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<uint8_t>(i % 251);
    std::vector<uint8_t> destination(source.size());
    if (!driver.Read(getpid(), reinterpret_cast<uintptr_t>(source.data()),
                     destination.data(), destination.size()) || source != destination) {
        std::cerr << "FAIL: large own-buffer read\n";
        return 1;
    }
    std::fill(destination.begin(), destination.end(), 0);
    if (!driver.Write(getpid(), reinterpret_cast<uintptr_t>(destination.data()),
                      source.data(), source.size()) || source != destination) {
        std::cerr << "FAIL: large own-buffer write\n";
        return 1;
    }

    const long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0 || 2 * pageSize > EXT_MEM_MAX_TRANSFER) {
        std::cerr << "FAIL: unsupported page size for partial-transfer test\n";
        return 1;
    }
    const size_t mappingSize = 2 * static_cast<size_t>(pageSize);
    void* mapping = mmap(nullptr, mappingSize, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return 1;
    std::memset(mapping, 0x5A, pageSize);
    if (mprotect(static_cast<uint8_t*>(mapping) + pageSize, pageSize, PROT_NONE) != 0) {
        munmap(mapping, mappingSize);
        return 1;
    }
    destination.assign(mappingSize, 0xA5);
    const bool partialRead = driver.Read(getpid(), reinterpret_cast<uintptr_t>(mapping),
                                         destination.data(), destination.size());
    const int readError = errno;
    const bool unchanged = std::all_of(destination.begin(), destination.end(),
                                       [](uint8_t b) { return b == 0xA5; });
    const bool emptyRead = driver.Read(getpid(), reinterpret_cast<uintptr_t>(mapping) + pageSize,
                                       destination.data(), 8);
    const int emptyError = errno;
    munmap(mapping, mappingSize);
    if (partialRead || readError != EIO || !unchanged || emptyRead || emptyError != EFAULT) {
        std::cerr << "FAIL: partial/zero-byte reads were not rejected correctly\n";
        return 1;
    }
    std::cout << "PASS: own-buffer large reads/writes and partial/zero-byte read rejection\n";
    return 0;
}
