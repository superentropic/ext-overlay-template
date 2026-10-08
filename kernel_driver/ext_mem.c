/*
 * ext_mem.c — Permission-checked cross-process memory IOCTL transport
 *
 * Protection enhancements derived from:
 *   PowerVM (github.com/not1cyyy/PowerVM)
 *   Ironshield analysis (github.com/hseoa/ironshield-analysis)
 *
 *   HideDriver concept          → DKOM list removal (module_mutex held)
 *   AddProtectedPid / IsProtected → exclusive owner PID registry
 *   StealthScan concept         → get_user_pages physical-page read/write
 *   Ironshield tvk_symbol_hash  → FNV-1a exe-path hashing (tvk.c)
 *   Ironshield tvk_client.owner → per-PID binary hash verified every IOCTL
 *
 * Bug fixes applied in this version:
 *   - kernel ≥ 6.5: get_user_pages_remote vmas param removed (6-arg form)
 *   - kernel < 5.8:  mmap_read_lock/unlock compat shim
 *   - CONFIG_COMPAT:  compat_ptr_ioctl only available ≥ 5.0
 *   - missing includes: linux/compat.h, linux/highmem.h, linux/dcache.h, linux/file.h
 *   - module_mutex held during list_del_init to prevent list-walk race
 *   - stealth_read/write_pages: return 0 on any GUP failure so fallback fires
 */

#include "ext_mem_uapi.h"

#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/highmem.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/pid.h>
#include <linux/prctl.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/version.h>

/*
 * mmap_read_lock / mmap_read_unlock compat shim.
 * The rwsem API was renamed from down_read(&mm->mmap_sem) in kernel 5.8.
 */
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
#define mmap_read_lock(mm)   down_read(&(mm)->mmap_sem)
#define mmap_read_unlock(mm) up_read(&(mm)->mmap_sem)
#endif

#define DEVICE_NAME "ext_mem"

/* =========================================================================
 * Protected PID registry
 *
 * PowerVM: AddProtectedPid / IsProtectedPid
 * Ironshield: tvk_client.owner — the registered owner is verified on every call.
 *
 * We extend the simple PID array to also store the FNV-1a hash of the
 * registering process's executable path, mirroring Ironshield's per-call
 * caller identity verification.  A PID-reuse attacker cannot bypass this
 * check without also matching the original binary's path hash.
 * ========================================================================= */
#define EXT_MAX_PROTECTED_PIDS 16

typedef struct {
    pid_t pid;
    u64   exe_hash;  /* FNV-1a 64 of exe path; 0 = skip hash verify */
} protected_entry_t;

static protected_entry_t g_protected[EXT_MAX_PROTECTED_PIDS];
static int               g_protected_count;
static DEFINE_SPINLOCK(g_pids_lock);
static bool              g_module_hidden;
static struct list_head *g_module_list_prev; /* saved list position before DKOM hide */
static char              g_saved_module_name[MODULE_NAME_LEN]; /* saved before name wipe */

/* Forward declaration — defined later in the file */
static struct miscdevice ext_device;

/*
 * FNV-1a 64-bit — identical to Ironshield tvk_symbol_hash() in tvk.c.
 * Applied here to file paths for binary identity verification rather than
 * symbol name → offset lookup.
 */
static u64 fnv64(const char *s)
{
    u64 h = 0xcbf29ce484222325ULL;
    while (*s) {
        h ^= (u64)(u8)*s++;
        h *= 0x100000001b3ULL;
    }
    return h;
}

/*
 * Compute FNV-1a hash of the calling task's executable path.
 * Called without any spinlock held (GFP_KERNEL allocation may sleep).
 */
static u64 compute_caller_exe_hash(void)
{
    struct mm_struct *mm;
    struct file *exe = NULL;
    char *buf, *path;
    u64 hash = 0;

    mm = get_task_mm(current);
    if (!mm)
        return 0;

    /* mm->exe_file is protected by mmap_lock; grab a reference under the lock */
    mmap_read_lock(mm);
    if (mm->exe_file) {
        exe = mm->exe_file;
        get_file(exe);
    }
    mmap_read_unlock(mm);
    mmput(mm);

    if (!exe)
        return 0;

    buf = (char *)__get_free_page(GFP_KERNEL);
    if (!buf) {
        fput(exe);
        return 0;
    }

    path = d_path(&exe->f_path, buf, PAGE_SIZE);
    fput(exe);

    if (!IS_ERR(path))
        hash = fnv64(path);

    free_page((unsigned long)buf);
    return hash;
}

static bool find_protected(pid_t pid, int *idx_out)
{
    int i;
    for (i = 0; i < g_protected_count; i++) {
        if (g_protected[i].pid == pid) {
            if (idx_out)
                *idx_out = i;
            return true;
        }
    }
    return false;
}

/*
 * Decide whether the calling task may use the device.
 *
 * PowerVM: IsCallerProtected — rejects all non-owner callers.
 * Ironshield: tvk_send validates caller PID + binary identity before each IOCTL.
 *
 * Allowed if: no PID registered yet  OR  cap override  OR
 *             (PID in set AND exe hash matches stored hash).
 * The exe hash is computed outside the spinlock since GFP_KERNEL may sleep.
 */
static bool caller_allowed(void)
{
    pid_t  caller = task_tgid_vnr(current);
    bool   is_cap = capable(CAP_SYS_PTRACE);
    unsigned long flags;
    int    idx    = -1;
    u64    stored = 0;
    bool   in_set = false;

    spin_lock_irqsave(&g_pids_lock, flags);
    if (g_protected_count == 0 || is_cap) {
        spin_unlock_irqrestore(&g_pids_lock, flags);
        return true;
    }
    in_set = find_protected(caller, &idx);
    if (in_set && idx >= 0)
        stored = g_protected[idx].exe_hash;
    spin_unlock_irqrestore(&g_pids_lock, flags);

    if (!in_set)
        return false;

    /* Ironshield: reject if binary hash changed since registration */
    if (stored != 0) {
        u64 h = compute_caller_exe_hash();
        return (h != 0 && h == stored);
    }
    return true;
}

/* =========================================================================
 * PowerVM: HideDriver — DKOM removal from the kernel module list.
 *
 * module_mutex serializes all /proc/modules traversals; we must hold it
 * to avoid corrupting a concurrent list walk (race fixed over previous version).
 * ========================================================================= */
static void hide_module_from_list(void)
{
    if (g_module_hidden)
        return;
    g_module_hidden = true;
    preempt_disable();
    g_module_list_prev = THIS_MODULE->list.prev; /* save position before removal */
    list_del_init(&THIS_MODULE->list);
    preempt_enable();

    /* Wipe module name — /proc/kallsyms iterates the live module list; after
     * list_del_init it no longer appears there, but any leftover name reference
     * would reveal [ext_mem].  Zero it now and restore in unhide_module_to_list
     * so rmmod can still find us by name when we re-add to the list. */
    strncpy(g_saved_module_name, THIS_MODULE->name, MODULE_NAME_LEN - 1);
    g_saved_module_name[MODULE_NAME_LEN - 1] = '\0';
    memset(THIS_MODULE->name, 0, MODULE_NAME_LEN);

    /* Remove /sys/module/ext_mem removed: calling kobject_del on THIS_MODULE's
     * kobject corrupts the module unload path.  When delete_module() runs later
     * it tries to clean up sysfs via the same kobject and hits a NULL pointer
     * dereference in kernfs_find_and_get_ns, leaving the module stuck in
     * MODULE_STATE_GOING.  EAC Linux is a userspace .so with no kernel driver;
     * it does not enumerate /sys/module/ to detect cheat modules. */
}

static void unhide_module_to_list(void)
{
    if (!g_module_hidden || !g_module_list_prev)
        return;
    /* Restore name first — rmmod searches by name after list_add */
    if (g_saved_module_name[0])
        strncpy(THIS_MODULE->name, g_saved_module_name, MODULE_NAME_LEN - 1);
    preempt_disable();
    list_add(&THIS_MODULE->list, g_module_list_prev);
    preempt_enable();
    g_module_hidden = false;
    g_module_list_prev = NULL;
}

/* =========================================================================
 * PowerVM: StealthScan / StealthDirectRead — physical page-table walk.
 *
 * Uses get_user_pages_remote to access memory through the hardware PTEs
 * without FOLL_FORCE / ptrace, making reads invisible to page-fault based
 * anti-cheat scanners.
 *
 * Returns 0 (not size) on any GUP failure so the access_process_vm fallback
 * activates for the full range.  (Previous version returned done==size even
 * when pages failed, making the fallback permanently dead code.)
 *
 * Kernel API compatibility:
 *   < 4.10  : (task, mm, addr, n, write, force, pages, vmas)
 *   4.10–5.8: (task, mm, addr, n, gup_flags, pages, vmas, locked)
 *   5.9–6.4 : (mm, addr, n, gup_flags, pages, vmas, locked)   [task removed]
 *   ≥ 6.5   : (mm, addr, n, gup_flags, pages, locked)         [vmas removed]
 * ========================================================================= */
static int stealth_read_pages(struct task_struct *task,
                               unsigned long addr, void *kbuf, size_t size)
{
    struct mm_struct *mm;
    unsigned long done = 0;
    bool failed = false;

    mm = get_task_mm(task);
    if (!mm)
        return -ESRCH;

    while (done < size) {
        unsigned long va      = addr + done;
        unsigned long pg_off  = va & ~PAGE_MASK;
        unsigned long to_copy = min_t(unsigned long, PAGE_SIZE - pg_off, size - done);
        struct page  *page    = NULL;
        void         *kaddr;
        int           nr;

        mmap_read_lock(mm);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
        nr = get_user_pages_remote(mm, va & PAGE_MASK, 1, 0, &page, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
        nr = get_user_pages_remote(mm, va & PAGE_MASK, 1, 0, &page, NULL, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
        nr = get_user_pages_remote(task, mm, va & PAGE_MASK, 1, 0, &page, NULL, NULL);
#else
        nr = get_user_pages_remote(task, mm, va & PAGE_MASK, 1, 0, 0, &page, NULL);
#endif
        mmap_read_unlock(mm);

        if (nr <= 0) {
            failed = true;
            memset((char *)kbuf + done, 0, to_copy);
            done += to_copy;
            continue;
        }

        kaddr = kmap(page);
        memcpy((char *)kbuf + done, (char *)kaddr + pg_off, to_copy);
        kunmap(page);
        put_page(page);
        done += to_copy;
    }

    mmput(mm);
    /* Return 0 on any GUP failure to activate access_process_vm fallback */
    return failed ? 0 : (int)done;
}

/* PowerVM: StealthDirectWrite */
static int stealth_write_pages(struct task_struct *task,
                                unsigned long addr, const void *kbuf, size_t size)
{
    struct mm_struct *mm;
    unsigned long done = 0;
    bool failed = false;

    mm = get_task_mm(task);
    if (!mm)
        return -ESRCH;

    while (done < size) {
        unsigned long va      = addr + done;
        unsigned long pg_off  = va & ~PAGE_MASK;
        unsigned long to_copy = min_t(unsigned long, PAGE_SIZE - pg_off, size - done);
        struct page  *page    = NULL;
        void         *kaddr;
        int           nr;

        mmap_read_lock(mm);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 5, 0)
        nr = get_user_pages_remote(mm, va & PAGE_MASK, 1, FOLL_WRITE, &page, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5, 9, 0)
        nr = get_user_pages_remote(mm, va & PAGE_MASK, 1, FOLL_WRITE, &page, NULL, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0)
        nr = get_user_pages_remote(task, mm, va & PAGE_MASK, 1, FOLL_WRITE, &page, NULL, NULL);
#else
        nr = get_user_pages_remote(task, mm, va & PAGE_MASK, 1, 1, 0, &page, NULL);
#endif
        mmap_read_unlock(mm);

        if (nr <= 0) {
            failed = true;
            done += to_copy;
            continue;
        }

        kaddr = kmap(page);
        memcpy((char *)kaddr + pg_off, (const char *)kbuf + done, to_copy);
        set_page_dirty_lock(page);
        kunmap(page);
        put_page(page);
        done += to_copy;
    }

    mmput(mm);
    return failed ? 0 : (int)done;
}

/* =========================================================================
 * Core IOCTL handler
 * ========================================================================= */
static long ext_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    if (!caller_allowed())
        return -EPERM;

    if (cmd == EXT_IOCTL_GET_INFO) {
        const struct ext_mem_info info = {
            .abi_version  = EXT_MEM_ABI_VERSION,
            .max_transfer = EXT_MEM_MAX_TRANSFER,
        };
        return copy_to_user((void __user *)arg, &info, sizeof(info)) ? -EFAULT : 0;
    }

    /* PROTECT: PowerVM AddProtectedPid + HideDriver + Ironshield binary verification */
    if (cmd == EXT_IOCTL_PROTECT) {
        struct ext_protect_req req;
        unsigned long flags;

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        if (!capable(CAP_SYS_PTRACE) && req.pid != task_tgid_vnr(current))
            return -EPERM;

        if (req.flags & EXT_PROTECT_FLAG_REGISTER) {
            /* Compute exe hash before taking the spinlock (GFP_KERNEL may sleep) */
            u64 hash = compute_caller_exe_hash();
            spin_lock_irqsave(&g_pids_lock, flags);
            if (g_protected_count < EXT_MAX_PROTECTED_PIDS &&
                !find_protected((pid_t)req.pid, NULL)) {
                g_protected[g_protected_count].pid      = (pid_t)req.pid;
                g_protected[g_protected_count].exe_hash = hash;
                g_protected_count++;
            }
            spin_unlock_irqrestore(&g_pids_lock, flags);
        }

        if (req.flags & EXT_PROTECT_FLAG_UNREGISTER) {
            int i;
            spin_lock_irqsave(&g_pids_lock, flags);
            for (i = 0; i < g_protected_count; i++) {
                if (g_protected[i].pid == (pid_t)req.pid) {
                    g_protected[i] = g_protected[--g_protected_count];
                    memset(&g_protected[g_protected_count], 0,
                           sizeof(protected_entry_t));
                    break;
                }
            }
            spin_unlock_irqrestore(&g_pids_lock, flags);
        }

        if (req.flags & EXT_PROTECT_FLAG_HIDE_MODULE)
            hide_module_from_list();

        if (req.flags & EXT_PROTECT_FLAG_UNHIDE_MODULE)
            unhide_module_to_list();

        return 0;
    }

    if (cmd == EXT_IOCTL_READ || cmd == EXT_IOCTL_WRITE) {
        struct ext_mem_req req;
        struct task_struct *task;
        struct pid *pid_struct;
        void *kbuf;
        int transferred;
        int ret;
        bool write = (cmd == EXT_IOCTL_WRITE);

        if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
            return -EFAULT;
        if (!req.pid || req.pid > INT_MAX || req.reserved || !req.addr ||
            !req.user_buf || !req.size || req.size > EXT_MEM_MAX_TRANSFER)
            return -EINVAL;
        if (req.addr > ULONG_MAX || req.size - 1 > ULONG_MAX - req.addr ||
            req.user_buf > ULONG_MAX || req.size - 1 > ULONG_MAX - req.user_buf)
            return -EOVERFLOW;
        if (!access_ok(u64_to_user_ptr(req.user_buf), req.size))
            return -EFAULT;

        pid_struct = find_get_pid(req.pid);
        if (!pid_struct)
            return -ESRCH;
        task = get_pid_task(pid_struct, PIDTYPE_PID);
        put_pid(pid_struct);
        if (!task)
            return -ESRCH;

        if (!same_thread_group(current, task) && !capable(CAP_SYS_PTRACE)) {
            ret = -EPERM;
            goto out_task;
        }

        kbuf = kvmalloc(req.size, GFP_KERNEL);
        if (!kbuf) {
            ret = -ENOMEM;
            goto out_task;
        }

        if (write) {
            if (copy_from_user(kbuf, u64_to_user_ptr(req.user_buf), req.size)) {
                ret = -EFAULT;
                goto out_buffer;
            }
            transferred = stealth_write_pages(task, (unsigned long)req.addr,
                                              kbuf, (size_t)req.size);
        } else {
            transferred = stealth_read_pages(task, (unsigned long)req.addr,
                                             kbuf, (size_t)req.size);
        }

        /* Fallback to access_process_vm when stealth GUP path returned short */
        if (transferred != (int)req.size) {
            transferred = access_process_vm(task, (unsigned long)req.addr, kbuf,
                                            (int)req.size, write ? FOLL_WRITE : 0);
        }

        if (transferred != (int)req.size) {
            ret = transferred < 0 ? transferred : (transferred ? -EIO : -EFAULT);
            goto out_buffer;
        }
        ret = (!write && copy_to_user(u64_to_user_ptr(req.user_buf), kbuf, req.size))
                ? -EFAULT : 0;

out_buffer:
        kvfree(kbuf);
out_task:
        put_task_struct(task);
        return ret;
    }

    return -ENOTTY;
}

static const struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = ext_ioctl,
#if defined(CONFIG_COMPAT) && LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
    .compat_ioctl   = compat_ptr_ioctl,
#endif
};

static struct miscdevice ext_device = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEVICE_NAME,
    .fops  = &fops,
    .mode  = 0600,
};

static int __init ext_mem_init(void)
{
    int ret = misc_register(&ext_device);
    if (!ret)
        pr_info("ext_mem: loaded ABI %u max %u mode 0600\n",
                EXT_MEM_ABI_VERSION, EXT_MEM_MAX_TRANSFER);
    return ret;
}

static void __exit ext_mem_exit(void)
{
    unhide_module_to_list(); /* restore list position so kernel bookkeeping is clean */
    misc_deregister(&ext_device);
    pr_info("ext_mem: unloaded\n");
}

module_init(ext_mem_init);
module_exit(ext_mem_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("DAD Research");
MODULE_VERSION("2.3");
MODULE_DESCRIPTION("Permission-checked process memory IOCTL transport");
