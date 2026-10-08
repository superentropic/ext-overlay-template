#pragma once
#ifndef _WIN32

/*
 * protection.h — Userspace protection layer for ext-research-tool (Linux)
 *
 * Techniques derived from PowerVM (github.com/not1cyyy/PowerVM):
 *
 *   Fake_NtQueryInformationProcess → IsBeingDebugged() anti-debug checks
 *   DisguiseProcess / resetProcessImageName → DisguiseProcess() name spoofing
 *   HideDriver DKOM → HardenKernelModule() module self-hide trigger
 *   AddProtectedPid → RegisterWithDriver() exclusive device lock
 *   EtwSpoof → SuppressTelemetry() core-dump / audit suppression
 *   NPT memory protection → HardenMemory() DONTDUMP / mlock regions
 *   SSDT hooks (Fake_NtReadVirtualMemory) → InstallSeccompFilter() syscall filter
 *   DrvMain integrity checks → VerifyBinaryIntegrity() binary hash
 *   PowerVM CPUID magic 0x41414150 → QueryHypervisor() HV presence check
 */

#include <string>
#include <cstddef>

namespace Protection {

struct Config {
    /* Fake name shown in /proc, ps, top — mirrors DisguiseProcess */
    const char *fakeProcessName  = "kworker/u16:3";
    /* Fake thread names for non-main threads (resetProcessImageName) */
    const char *fakeThreadName   = "kworker/u16:3-events";
    bool enableAntiDebug         = true;  /* Fake_NtQueryInformationProcess */
    bool enableProcessStealth    = true;  /* DisguiseProcess */
    bool enableMemoryHardening   = true;  /* NPT/MADV_DONTDUMP regions */
    bool enableAntiDump          = true;  /* EtwSpoof / crash-dump suppression */
    bool enableSeccomp           = true;  /* SSDT hook equivalent: seccomp BPF */
    bool enableIntegrityCheck    = true;  /* DrvMain integrity verify */
    bool enableKernelModuleHide  = true;  /* HideDriver DKOM */
    bool enableKernelPidRegister = true;  /* AddProtectedPid */
};

/*
 * Init — call as the very first thing in main(), before any other work.
 * argc/argv are needed to overwrite the cmdline buffer in place
 * (mirrors resetProcessPeb64Param CommandLine replacement on Windows).
 */
bool Init(int argc, char **argv, const Config &cfg = Config{});

/*
 * IsBeingDebugged — multi-method debugger detection.
 * Mirrors PowerVM's Fake_NtQueryInformationProcess returning
 * ProcessDebugPort=0 / ProcessDebugFlags=1 to hide the debug state.
 * Here we do the inverse: actively *detect* if someone attached.
 * Returns true if a tracer is present.
 */
bool IsBeingDebugged();

/*
 * VerifyBinaryIntegrity — SHA-256 of /proc/self/exe on disk.
 * Mirrors PowerVM DrvMain integrity fingerprint check.
 * Returns false if the binary has been patched/replaced.
 */
bool VerifyBinaryIntegrity();

/*
 * QueryHypervisor — CPUID leaf 0x41414150 (PowerVM magic).
 * Returns true if the PowerVM hypervisor is active; the tool can then
 * use HV-assisted memory reads instead of the kernel driver.
 */
bool QueryHypervisor();

/*
 * Shutdown — signal the background monitor thread to exit.
 * Call before process exit.
 */
void Shutdown();

} // namespace Protection

#endif /* !_WIN32 */
