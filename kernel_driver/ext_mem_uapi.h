#ifndef EXT_MEM_UAPI_H
#define EXT_MEM_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define EXT_MEM_DEVICE "/dev/ext_mem"
/* Runtime override: EXT_DEV_PATH env var lets run-linux.sh move the device
 * node to a hidden location after insmod, away from /dev/ext_mem. */
#define EXT_MEM_DEVICE_ENV "EXT_DEV_PATH"
#define EXT_MEM_ABI_VERSION 2U
#define EXT_MEM_MAX_TRANSFER 0x10000U
#define EXT_IOCTL_MAGIC 'D'

/* Explicit padding and alignment keep the layout identical on 32/64-bit hosts. */
struct ext_mem_req {
    __u32 pid;
    __u32 reserved;
    __aligned_u64 addr;
    __aligned_u64 size;
    __aligned_u64 user_buf;
};

struct ext_mem_info {
    __u32 abi_version;
    __u32 max_transfer;
};

/* Register/unregister a PID for exclusive driver access protection.
 * Inspired by PowerVM AddProtectedPid / IsProtectedPid.
 * pid=0 clears all registered PIDs (unregister). */
struct ext_protect_req {
    __u32 pid;
    __u32 flags; /* EXT_PROTECT_FLAG_* */
};
#define EXT_PROTECT_FLAG_HIDE_MODULE    (1U << 0) /* Trigger module self-hide from lsmod */
#define EXT_PROTECT_FLAG_REGISTER       (1U << 1) /* Register PID as protected owner */
#define EXT_PROTECT_FLAG_UNREGISTER     (1U << 2) /* Remove PID from protected set */
#define EXT_PROTECT_FLAG_UNHIDE_MODULE  (1U << 3) /* Restore module to list so rmmod works */

#define EXT_IOCTL_READ      _IOWR(EXT_IOCTL_MAGIC, 0x01, struct ext_mem_req)
#define EXT_IOCTL_WRITE     _IOWR(EXT_IOCTL_MAGIC, 0x02, struct ext_mem_req)
#define EXT_IOCTL_GET_INFO  _IOR (EXT_IOCTL_MAGIC, 0x03, struct ext_mem_info)
/* PowerVM-derived: protected PID registry + module stealth control */
#define EXT_IOCTL_PROTECT   _IOW (EXT_IOCTL_MAGIC, 0x04, struct ext_protect_req)

#endif
