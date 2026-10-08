# ext-overlay-template

External memory overlay template for Linux. Implements a stealth kernel
driver + userspace overlay pipeline with full anti-detection hardening.

Fork this repo to build a game-specific tool. The infrastructure layer
is complete and game-agnostic. Only the **game layer** needs to be filled in.

---

## Architecture

```
┌─────────────────────────────────────────────────────┐
│  INFRASTRUCTURE  (do not modify between game forks)  │
├──────────────────────┬──────────────────────────────┤
│  kernel_driver/      │  Kernel module (ext_mem.ko)  │
│    ext_mem.c         │  GUP-based stealth reads      │
│    ext_mem_uapi.h    │  DKOM hide from /proc/modules │
├──────────────────────┼──────────────────────────────┤
│  src/core/           │  Process attach (no ptrace)   │
│    kernel_driver.h   │  Protection + anti-debug      │
│    process_linux.cpp │  Hardware fingerprint spoof   │
│    protection.cpp    │  Seccomp, binary integrity    │
│    runtime_paths.cpp │  Tmpfs log / hidden paths     │
├──────────────────────┼──────────────────────────────┤
│  src/render/         │  X11 overlay (ImGui/OpenGL)   │
│    overlay_linux.cpp │  Window identity spoofing     │
│    drawing.cpp       │  ESP / screen primitives      │
├──────────────────────┼──────────────────────────────┤
│  run-linux.sh        │  Launch script                │
│                      │  Module reload, device hide   │
│                      │  MAC/DMI spoof, process mask  │
└──────────────────────┴──────────────────────────────┘

┌─────────────────────────────────────────────────────┐
│  GAME LAYER  (implement per fork)                    │
├──────────────────────┬──────────────────────────────┤
│  src/game/           │  Actor/entity management      │
│    actor_manager.*   │  → fill in game structs       │
│    entity.h          │  → fill in offsets            │
│    item_database.*   │  → fill in item logic         │
├──────────────────────┼──────────────────────────────┤
│  src/sdk/            │  Engine SDK types             │
│    ue5_types.h       │  → replace with target SDK    │
│    gnames.*          │  → game-specific name cache   │
│    gworld.*          │  → game world traversal       │
├──────────────────────┼──────────────────────────────┤
│  src/main.cpp        │  Target process name          │
│  config/settings.json│  Offsets + feature flags      │
└──────────────────────┴──────────────────────────────┘
```

---

## Forking for a new game

1. **Set the target process** in `src/main.cpp`:
   ```cpp
   std::wstring processName = L"YourGame.exe";
   ```

2. **Fill in offsets** in `config/settings.json`

3. **Implement the game layer** — `src/game/` and `src/sdk/`

4. **Re-seal the binary hash** after first trusted build:
   ```bash
   sha256sum build/ext-research-tool | awk '{print $1}' > .bin_hash
   chmod 600 .bin_hash
   ```

5. **Build and run**:
   ```bash
   # Build kernel module
   make -C kernel_driver

   # Build userspace
   mkdir build && cd build
   cmake .. -DCMAKE_BUILD_TYPE=Release
   cmake --build . -j$(nproc)
   cd ..

   # Launch (handles module load, device hiding, MAC/DMI spoof)
   ./run-linux.sh
   ```

---

## Anti-detection: what this template provides

| Vector | Mitigation |
|---|---|
| `/proc/modules` | DKOM `list_del_init` — hidden while running |
| `/proc/kallsyms` | Module name wiped after hide |
| `/dev/ext_mem` device | Moved to `/tmp/opencode/.k_ioc` at runtime |
| Process cmdline | No args visible — config via `EXT_CONFIG` env var |
| MAC addresses | Randomised via sysfs bind-mount each launch |
| DMI hardware serials | Randomised via sysfs bind-mount each launch |
| Memory reads | `get_user_pages_remote` — invisible to userspace AC |
| ptrace / `process_vm_readv` | Not used |
| Overlay window identity | WM_CLASS → Steam, `_NET_WM_PID` removed |

---

## What requires a reboot between kernel module changes

If `insmod` fails with `Device or resource busy`, the previous module
is stuck. Reboot clears it.  Normal restarts (Ctrl-C → `dad`) cycle
the module cleanly without a reboot.

---

## Structure

```
ext-overlay-template/
├── kernel_driver/       # Kernel module — build with make
│   ├── ext_mem.c
│   └── ext_mem_uapi.h
├── src/
│   ├── core/            # Infrastructure — process, protection, paths
│   ├── game/            # TODO: game-specific entity/actor logic
│   ├── sdk/             # TODO: game engine SDK types
│   ├── render/          # Overlay, ImGui, ESP drawing
│   ├── shared/          # Shared memory IPC structures
│   └── main.cpp
├── config/
│   ├── settings.json    # Offsets + runtime config
│   └── items.json       # TODO: game item database
├── run-linux.sh         # Launch script
└── CMakeLists.txt
```
