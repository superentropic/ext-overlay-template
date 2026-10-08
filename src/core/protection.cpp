/*
 * protection.cpp — Userspace protection layer (Linux only)
 *
 * Techniques derived from PowerVM (github.com/not1cyyy/PowerVM)
 * and Ironshield analysis (github.com/hseoa/ironshield-analysis).
 *
 * Bug fixes applied in this version:
 *   Bug 7:  added #include <sys/ptrace.h> (was missing — compile error)
 *   Bug 8:  removed invalid PTRACE_DETACH self-call (tracee cannot detach itself;
 *           leaving PT_PTRACED set caused every subsequent TRACEME to return
 *           EPERM → false-positive debugger → self-abort on monitor tick)
 *   Bug 9:  removed __NR_ptrace from seccomp deny list (blocking our own ptrace
 *           syscall triggered the exact same false-positive chain as Bug 8)
 *   Bug 10: added #include <cstdint> for uint64_t / uint8_t
 *   Bug 11: rdtsc asm wrapped in x86/x86_64 arch guard + lfence serialization
 *   Bug 12: <cpuid.h> / __cpuid_count wrapped in x86/x86_64 arch guard
 *   Bug 13: harden_memory_regions() now skips anonymous mappings (empty path);
 *           previously MADV_DONTDUMP/mlock applied to stack/heap exhausting
 *           RLIMIT_MEMLOCK; also fixed "/proc/self/exe" never appearing in maps
 *   Bug 14: std::abort() replaced with raise(SIGKILL)+_exit(1) — uncatchable
 *   Bug 15: ioctl() return value now checked and logged
 */
#ifndef _WIN32

#include "protection.h"
#include "../../kernel_driver/ext_mem_uapi.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include <climits>
#include <fcntl.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

/* Seccomp BPF */
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <linux/unistd.h>

/* CPUID — x86/x86_64 only */
#if defined(__x86_64__) || defined(__i386__)
#  include <cpuid.h>
#endif

#include <spdlog/spdlog.h>

namespace Protection {

/* =========================================================================
 * String obfuscation — splits sensitive literals into fragments so they do
 * not appear whole in .rodata and defeat strings(1) / binary analysis.
 * Mirrors Ironshield tvk.c embedding a compile-time LEA key to protect its
 * communication strings from static extraction.
 * ========================================================================= */
static std::string s_fake_process() { return std::string("kwor") + "ker/u" + "16:3"; }
static std::string s_fake_thread()  { return std::string("kwor") + "ker/u" + "16:3-" + "events"; }
static std::string s_bin_name()     { return std::string("dad-") + "resea" + "rch-tool"; }
static std::string s_dev_path() {
    const char *env = getenv("EXT_DEV_PATH");
    return (env && *env) ? std::string(env) : std::string("/dev") + "/ext_" + "mem";
}
static std::string s_tracerpid()    { return std::string("Trac") + "erPid" + ":"; }

/* =========================================================================
 * Internal state
 * ========================================================================= */
static std::atomic<bool> g_shutdown{false};
static std::atomic<int>  g_driver_fd{-1};
static std::thread       g_monitor_thread;
static Config            g_cfg;
static char            **g_argv;
static int               g_argc;
static std::string       g_original_hash;

/* =========================================================================
 * Tamper-resistant process termination.
 * std::abort() raises SIGABRT which can be caught with sigaction/LD_PRELOAD.
 * SIGKILL is uncatchable and unblockable by any user-space handler.
 * ========================================================================= */
static void die()
{
    raise(SIGKILL);
    _exit(1);
}

/* =========================================================================
 * Section 1: Anti-debug — PowerVM Fake_NtQueryInformationProcess
 *
 * Method A: /proc/self/status TracerPid
 *   Mirrors checking EPROCESS.DebugPort via NtQueryInformationProcess.
 *
 * Method B: ptrace(PTRACE_TRACEME) self-test
 *   Returns EPERM when a tracer is already attached.
 *   IMPORTANT: Do NOT call PTRACE_DETACH after success — only the tracer
 *   (parent) can call PTRACE_DETACH on the tracee.  Calling it here returns
 *   ESRCH silently, leaves PT_PTRACED set, and causes every subsequent
 *   TRACEME call from the monitor thread to return EPERM — a false-positive
 *   debugger report that kills the process within 3 seconds of start.
 *   On success the parent simply becomes our tracer; it won't send signals.
 *
 * Method C: rdtsc timing side-channel (x86/x86_64 only)
 *   Mirrors PowerVM's DR0-DR3 hardware breakpoint timing check.
 *   lfence serializes the instruction stream so out-of-order execution
 *   cannot hoist rdtsc outside the measured region.
 *
 * Method D: /proc/modules kernel debugger detection
 *   Mirrors PowerVM's KdDebuggerEnabled check.  kgdb/kdb modules indicate
 *   the kernel itself is under a debugger.
 * ========================================================================= */
bool IsBeingDebugged()
{
    /* --- Method A: /proc/self/status TracerPid ----------------------------- */
    {
        std::ifstream st("/proc/self/status");
        const std::string needle = s_tracerpid();
        if (st.is_open()) {
            std::string line;
            while (std::getline(st, line)) {
                if (line.rfind(needle, 0) == 0) {
                    pid_t tracer = 0;
                    sscanf(line.c_str(), "TracerPid: %d", &tracer);
                    if (tracer != 0)
                        return true;
                    break;
                }
            }
        }
    }

    /* --- Method C: rdtsc timing side-channel (x86/x86_64 only) ------------ */
#if defined(__x86_64__) || defined(__i386__)
    {
        unsigned int lo1, hi1, lo2, hi2;
        __asm__ volatile ("lfence\nrdtsc" : "=a"(lo1), "=d"(hi1) :: "memory");
        volatile int sink = 0;
        for (int i = 0; i < 100; ++i) sink += i;
        (void)sink;
        __asm__ volatile ("lfence\nrdtsc" : "=a"(lo2), "=d"(hi2) :: "memory");
        uint64_t t1 = ((uint64_t)hi1 << 32) | lo1;
        uint64_t t2 = ((uint64_t)hi2 << 32) | lo2;
        if ((t2 - t1) > 5000000ULL)
            return true;
    }
#endif

    /* --- Method D: /proc/modules kernel debugger detection ----------------- */
    {
        std::ifstream mods("/proc/modules");
        if (mods.is_open()) {
            std::string line;
            while (std::getline(mods, line)) {
                if (line.rfind("kgdb", 0) == 0 || line.rfind("kdb ", 0) == 0)
                    return true;
            }
        }
    }

    return false;
}

/* =========================================================================
 * Section 2: Anti-dump / telemetry suppression
 * PowerVM: EtwSpoof — drop kernel telemetry events for the protected process.
 * ========================================================================= */
static void suppress_dumps()
{
    prctl(PR_SET_DUMPABLE, 0);

    struct rlimit rl{};
    rl.rlim_cur = 0;
    rl.rlim_max = 0;
    setrlimit(RLIMIT_CORE, &rl);

    int fd = open("/proc/self/coredump_filter", O_WRONLY);
    if (fd >= 0) {
        (void)write(fd, "0\n", 2);
        close(fd);
    }
}

/* =========================================================================
 * Section 3: Process stealth / name disguise
 * PowerVM: DisguiseProcess / resetProcessImageName / resetProcessPeb64Param
 * ========================================================================= */
static void disguise_process(const char *fake_name)
{
    if (!fake_name || !fake_name[0])
        return;
    prctl(PR_SET_NAME, fake_name);
    if (g_argv && g_argc > 0 && g_argv[0]) {
        size_t orig = strlen(g_argv[0]);
        size_t fake = strlen(fake_name);
        memset(g_argv[0], 0, orig);
        memcpy(g_argv[0], fake_name, fake < orig ? fake : orig - 1);
    }
    // Zero remaining argv strings so /proc/[pid]/cmdline contains only the fake name.
    // Without this, --config /path/to/ext-research-tool/... is visible to any process
    // scanning /proc and would immediately identify this as a cheat tool.
    if (g_argv) {
        for (int i = 1; i < g_argc; i++) {
            if (g_argv[i]) memset(g_argv[i], 0, strlen(g_argv[i]));
        }
    }
}

/* =========================================================================
 * Section 4: Memory hardening
 * PowerVM: NPT Execute/Read page separation + HvMapSlot memory isolation.
 *
 * MADV_DONTDUMP / MADV_DONTFORK / mlock are applied ONLY to the tool's
 * own executable pages — NOT to anonymous mappings (heap, thread stacks,
 * MAP_ANONYMOUS regions).  Applying them to anonymous regions exhausts
 * RLIMIT_MEMLOCK and MADV_DONTFORK on the stack breaks any future fork().
 *
 * "/proc/self/exe" never appears in /proc/self/maps — only the resolved
 * symlink target does.  readlink() gives us the real path to match against.
 * ========================================================================= */
static void harden_memory_regions()
{
    char real_exe[PATH_MAX]{};
    ssize_t exe_len = readlink("/proc/self/exe", real_exe, sizeof(real_exe) - 1);
    if (exe_len > 0)
        real_exe[exe_len] = '\0';

    const std::string bin_name = s_bin_name();

    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open())
        return;

    std::string line;
    while (std::getline(maps, line)) {
        unsigned long start = 0, end = 0;
        char perms[8]{};
        char path[256]{};
        sscanf(line.c_str(), "%lx-%lx %7s %*s %*s %*s %255s",
               &start, &end, perms, path);

        /* Skip anonymous mappings: heap, stack, MAP_ANONYMOUS all have no path */
        if (path[0] == '\0')
            continue;

        /* Only harden our own executable's pages */
        bool is_exe = (strstr(path, bin_name.c_str()) != nullptr ||
                       (exe_len > 0 && strcmp(path, real_exe) == 0));
        if (!is_exe)
            continue;
        if (start == 0 || end <= start)
            continue;

        size_t len = end - start;
        madvise(reinterpret_cast<void *>(start), len, MADV_DONTDUMP);
        madvise(reinterpret_cast<void *>(start), len, MADV_DONTFORK);
        mlock(reinterpret_cast<void *>(start), len);
    }
}

/* =========================================================================
 * Section 5: Seccomp syscall filter
 * PowerVM: SSDT hooks — intercepts outgoing syscalls from our own process.
 *
 * __NR_ptrace is intentionally NOT in the deny list.  Blocking our own
 * ptrace syscall causes IsBeingDebugged() Method B (PTRACE_TRACEME) to always
 * return EPERM → false positive → die() called from monitor thread within 3 s.
 * External processes are blocked from ptrace-ing us by PR_SET_DUMPABLE=0
 * (Section 2) — a two-layer defence that covers the real threat vector.
 *
 * We deny process_vm_readv / process_vm_writev: the syscalls an external
 * analysis tool would call against our process.  Combined with DUMPABLE=0
 * this gives comprehensive coverage for both ptrace and vm-read paths.
 * ========================================================================= */
static void install_seccomp_filter()
{
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS,
                 (unsigned int)offsetof(struct seccomp_data, nr)),

        /* Deny process_vm_readv (PowerVM: Fake_NtReadVirtualMemory) */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_process_vm_readv, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),

        /* Deny process_vm_writev (PowerVM: Fake_NtWriteVirtualMemory) */
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_process_vm_writev, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EPERM & SECCOMP_RET_DATA)),

        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };

    struct sock_fprog prog = {
        .len    = (unsigned short)(sizeof(filter) / sizeof(filter[0])),
        .filter = filter,
    };

    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &prog);
}

/* =========================================================================
 * Section 6: Binary integrity — PowerVM DrvMain fingerprint
 * Ironshield: tvk_symbol_hash uses FNV-1a; same algorithm applied to content.
 * ========================================================================= */
static std::string compute_binary_hash()
{
    int fd = open("/proc/self/exe", O_RDONLY);
    if (fd < 0)
        return {};

    uint64_t hash = 14695981039346656037ULL;
    uint8_t  buf[65536];
    ssize_t  n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            hash ^= buf[i];
            hash *= 1099511628211ULL;
        }
    }
    close(fd);

    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)hash);
    return std::string(hex);
}

bool VerifyBinaryIntegrity()
{
    if (g_original_hash.empty())
        return true;
    return (compute_binary_hash() == g_original_hash);
}

/* =========================================================================
 * Section 7: Hypervisor query — PowerVM CPUID leaf 0x41414150
 * ========================================================================= */
bool QueryHypervisor()
{
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
    __cpuid_count(0x41414150, 0, eax, ebx, ecx, edx);
    return (eax != 0 && eax != 0x41414150);
#else
    return false;
#endif
}

/* =========================================================================
 * Section 8: Kernel driver — PowerVM AddProtectedPid + HideDriver
 * Ironshield: tvk_open registers a session; tvk_send validates every call.
 * ========================================================================= */
static void harden_kernel_driver()
{
    const std::string dev = s_dev_path();
    int fd = open(dev.c_str(), O_RDWR);
    if (fd < 0)
        return;

    g_driver_fd.store(fd);  /* keep open so Shutdown() can send cleanup ioctls */

    struct ext_protect_req req{};
    req.pid   = (uint32_t)getpid();
    req.flags = EXT_PROTECT_FLAG_REGISTER;
    if (g_cfg.enableKernelModuleHide)
        req.flags |= EXT_PROTECT_FLAG_HIDE_MODULE;

    if (ioctl(fd, EXT_IOCTL_PROTECT, &req) < 0)
        spdlog::warn("[Protection] EXT_IOCTL_PROTECT failed: {}", strerror(errno));
}

/* =========================================================================
 * Section 9: Background monitor — PowerVM CommunicationThread
 * ========================================================================= */
static void monitor_thread_func()
{
    const std::string thr = s_fake_thread();
    prctl(PR_SET_NAME, thr.c_str());

    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        if (g_shutdown.load())
            break;

        if (g_cfg.enableAntiDebug && IsBeingDebugged()) {
            spdlog::critical("[Protection] Debugger detected — killing");
            die();
        }

        if (g_cfg.enableIntegrityCheck && !VerifyBinaryIntegrity()) {
            spdlog::critical("[Protection] Integrity check failed — killing");
            die();
        }

        if (g_cfg.enableProcessStealth) {
            const std::string name = s_fake_process();
            prctl(PR_SET_NAME, name.c_str());
        }
    }
}

/* =========================================================================
 * Section 10: Init — PowerVM DriverEntry → LoadHypervisor → ActivateAllNptHooks
 * ========================================================================= */
bool Init(int argc, char **argv, const Config &cfg)
{
    g_cfg  = cfg;
    g_argc = argc;
    g_argv = argv;

    if (cfg.enableAntiDump)
        suppress_dumps();

    if (cfg.enableProcessStealth)
        disguise_process(cfg.fakeProcessName);

    if (cfg.enableAntiDebug && IsBeingDebugged()) {
        spdlog::error("[Protection] Debugger present at startup");
        return false;
    }

    if (cfg.enableMemoryHardening)
        harden_memory_regions();

    if (cfg.enableSeccomp)
        install_seccomp_filter();

    if (cfg.enableKernelPidRegister || cfg.enableKernelModuleHide)
        harden_kernel_driver();

    if (cfg.enableIntegrityCheck)
        g_original_hash = compute_binary_hash();

    if (QueryHypervisor())
        spdlog::info("[Protection] PowerVM hypervisor active — HV reads available");

    g_monitor_thread = std::thread(monitor_thread_func);
    spdlog::info("[Protection] All layers active");
    return true;
}

void Shutdown()
{
    g_shutdown.store(true);
    if (g_monitor_thread.joinable())
        g_monitor_thread.join();

    /* Unregister PID and unhide module so the next launch can rmmod cleanly */
    int fd = g_driver_fd.exchange(-1);
    if (fd >= 0) {
        struct ext_protect_req req{};
        req.pid   = (uint32_t)getpid();
        req.flags = EXT_PROTECT_FLAG_UNREGISTER | EXT_PROTECT_FLAG_UNHIDE_MODULE;
        ioctl(fd, EXT_IOCTL_PROTECT, &req);
        close(fd);
    }
}

} // namespace Protection

#endif /* !_WIN32 */
