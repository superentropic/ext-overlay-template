# Linux memory transport

This is an `ioctl` transport, **not** direct `mmap` access or syscall-free access.
It provides no guarantee of invisibility to monitoring or anti-cheat software.

## Fixes in ABI 2

- Transfers larger than 64 KiB are split into bounded requests in userspace.
- Module version 2.1 uses `kvmalloc`/`kvfree` for request buffers, allowing a
  virtually contiguous allocation when a physically contiguous 64-KiB allocation
  fails. The userspace ABI remains version 2.
- Only a complete transfer is reported as successful; zero-byte transfers return
  `EFAULT`, and partial transfers return `EIO`.
- Task references are held during transfers to avoid process-exit races.
- The device defaults to mode `0600`. Cross-process requests require
  `CAP_SYS_PTRACE`; access to the device alone does not grant that privilege.
- The application never loads the module automatically. It rejects older modules
  that lack the ABI handshake instead of trusting their incorrect success results.
- A driver failure is not retried through `process_vm_*`. Earlier chunks of a
  failed write may already have changed memory, so replaying them is unsafe.
- Warnings include the address, size, completed chunks, and errno, at most once
  per five seconds per connection.

## Build and manual reload

From the project root:

```sh
make -C kernel_driver
cmake --build build-test -j4
```

Rebuilding does not replace a module already loaded in the kernel. Close programs
using the device before an administrator manually reloads it:

```sh
sudo make -C kernel_driver uninstall
sudo make -C kernel_driver install
```

`insmod: File exists` means the module is already loaded. Check its version with
`cat /sys/module/ext_mem/version`; use the reload commands above to replace it.

Do not use `chmod 666` on the device. An older loaded module may still have that
unsafe permission until it is unloaded; restrict it to `0600` while arranging the
reload. The ABI 2 device is root-only by default, and cross-process use needs
explicit administrator-authorized privileges. An unprivileged application falls
back to its ordinary `process_vm_readv/writev` backend **at attachment time** if it
cannot open a compatible driver, with a clear log message.

For an explicitly authorized root run on the current X11 desktop, launch from the
project root while preserving the display connection:

```sh
sudo --preserve-env=DISPLAY,XAUTHORITY ./build-test/ext-research-tool
```

These changes address transport correctness, not attack redirection or aim
behavior. Successful startup/scanning does not verify those features.

## Tests

```sh
cmake -S . -B build-test -DBUILD_TESTING=ON
cmake --build build-test -j4
ctest --test-dir build-test --output-on-failure
```

The regression suite uses mocked syscalls and requires no privileges. The smoke
test only accesses its own process's memory and skips if the current user cannot
open a compatible ABI 2 module. Do not elevate privileges just to run the suite.
