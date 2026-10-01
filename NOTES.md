# Advanced Linux Kernel Programming: Revision Notes

**How to use these notes:** read the **Remember** box at the top of each section first. It holds the facts you must know. The rest of the section explains them. Blocks marked ▶ are optional deep dives: skip them on a first pass. Test yourself with the revision questions at the end of each section.

## Contents

- [Big Picture](#big-picture)
- [1. Where the Kernel Lives: `/boot` and Kernel Images](#1-where-the-kernel-lives-boot-and-kernel-images)
- [2. Where Kernels Come From: Distribution vs Vendor (BSP) Kernels](#2-where-kernels-come-from-distribution-vs-vendor-bsp-kernels)
- [3. Virtual Address Space: User/Kernel Split](#3-virtual-address-space-userkernel-split)
- [4. vDSO and vsyscall: Kernel Code Mapped into User Space](#4-vdso-and-vsyscall-kernel-code-mapped-into-user-space)
- [5. The First Processes: PID 0, PID 1 (`init`) and PID 2 (`kthreadd`)](#5-the-first-processes-pid-0-pid-1-init-and-pid-2-kthreadd)
- [6. Kernel Headers: In-Tree, Module-Build and UAPI](#6-kernel-headers-in-tree-module-build-and-uapi)
- [7. Loadable Kernel Modules (LKMs)](#7-loadable-kernel-modules-lkms)
- [8. Linux Capabilities](#8-linux-capabilities)
- [9. Kernel Architecture: Monolithic vs Microkernel](#9-kernel-architecture-monolithic-vs-microkernel)
- [10. Tracing: ftrace, kprobes, `trace_marker` and ptrace](#10-tracing-ftrace-kprobes-trace_marker-and-ptrace)
- [11. Pages and Page Size](#11-pages-and-page-size)
- [12. Synchronisation: Spinlocks, RW Locks and RCU](#12-synchronisation-spinlocks-rw-locks-and-rcu)
- [Labs & Exercises](#labs--exercises)
- [Quick Reference](#quick-reference)
- [Glossary](#glossary)
- [Open Questions](#open-questions)

---

## Big Picture

How the topics so far fit together:

```text
power on → firmware → bootloader (GRUB)
                          └─ loads /boot/vmlinuz + initramfs ........................ §1
                                └─ kernel decompresses itself, start_kernel()
                                      └─ PID 0 (idle) creates PID 1 (init/systemd)
                                         and PID 2 (kthreadd → all kernel threads) .. §5

every process sees one virtual address space:
      low half  = user space (private per process) .................................. §3
      high half = kernel space (shared, kernel mode only) ........................... §3
      a few kernel pages mapped into user space: [vdso], [vsyscall] ................. §4

extending the running kernel:
      module source ── built against the matching headers (§6) ──> .ko ── insmod ─> §7
      permission to load it (and other privileged operations): capabilities ......... §8

where the running kernel came from: kernel.org → distro / vendor BSP / Android GKI .. §2

why a module bug is a kernel bug: Linux is monolithic, one shared kernel space ..... §9

watching it all run: ftrace, kprobes, trace_marker, ptrace/strace ................ §10
```

**Golden rule so far:** *installed ≠ running*. Everything you build (modules, headers) must match the **running** kernel: `uname -r`.

---

## 1. Where the Kernel Lives: `/boot` and Kernel Images

> **Remember**
>
> - The kernel is a **file in `/boot`**, loaded once at boot by the **bootloader**. It is not a process.
> - Several kernels can be **installed**, but only one is **running**. Check with `uname -r`.
> - `vmlinuz` = compressed, bootable image. `vmlinux` = uncompressed ELF with symbols, used by debuggers.
> - The **initramfs** is a temporary RAM root filesystem. Its job is to mount the real root filesystem.
> - `System.map` = static, link-time symbol addresses. `/proc/kallsyms` = live addresses (these include the KASLR offset), hidden from non-root by `kptr_restrict`.
> - Modules are **not** in `/boot`. They are in `/lib/modules/$(uname -r)/`.

### Overview

On a typical distribution the kernel is an ordinary file in **`/boot`**. The **bootloader** (GRUB on x86_64) loads it, plus an **initramfs**, into memory and jumps to it. Before you build modules or custom kernels, you need to know which files are there and how they relate to the *running* kernel, because everything you build must match it.

### What is in `/boot`

One set of files per installed kernel, suffixed with the release string (e.g. `6.8.0-139-generic`):

| File | What it is |
| ---- | ---------- |
| `vmlinuz-<ver>` | **Compressed, bootable kernel image** (on x86 a **bzImage**). GRUB loads this. |
| `initrd.img-<ver>` | **initramfs**: compressed `cpio` archive unpacked into RAM as the first root filesystem. Holds the drivers and scripts needed to mount the real root (storage drivers, LVM, disk encryption). |
| `System.map-<ver>` | Kernel **symbol table** (address, type, name) at link-time addresses |
| `config-<ver>` | The `.config` the kernel was built with (every `CONFIG_*` option) |
| `vmlinuz`, `initrd.img` | Symlinks to the **newest installed** kernel (default boot entry) |
| `vmlinuz.old`, `initrd.img.old` | Symlinks to the previous kernel (fallback) |
| `grub/` | GRUB config (`grub.cfg` is generated by `update-grub`; do not edit it by hand) |

### Boot flow (x86_64)

```text
Firmware (UEFI/BIOS)
   └─> GRUB  (reads /boot/grub/grub.cfg)
         ├─ loads /boot/vmlinuz-<ver>      (kernel image)
         ├─ loads /boot/initrd.img-<ver>   (initramfs)
         └─ passes the command line        (see /proc/cmdline)
               └─> kernel decompresses itself, initialises
                     └─> unpacks initramfs as rootfs, runs /init
                           └─> mounts real root (/), switch_root
                                 └─> /sbin/init (systemd) = PID 1   (§5)
```

**ARM64:** the build output is `arch/arm64/boot/Image` (or `Image.gz`). There is **no self-decompressor**, so the bootloader must unpack `Image.gz` itself. The Pi 5 boots `/boot/firmware/kernel_2712.img` through its own firmware (`config.txt`), not GRUB.

### `vmlinuz` vs `vmlinux`

| | `vmlinux` | `vmlinuz` / `bzImage` |
| - | --------- | --------------------- |
| Format | Uncompressed ELF, full symbols (+ debug info with `CONFIG_DEBUG_INFO`) | Compressed, bootable, stripped |
| Built at | Top of the build tree | `arch/x86/boot/bzImage` → installed as `/boot/vmlinuz-<ver>` |
| Used for | Debugging: `gdb`, `crash`, `perf`, `addr2line` | Booting |
| On Ubuntu | Debug-symbol package (`linux-image-<ver>-dbgsym`) → `/usr/lib/debug/boot/` | `/boot` |

### How the kernel is entered

The bootloader-to-kernel handoff (CPU mode, registers, where parameters are) is **architecture-specific**. On x86 one `bzImage` file is built to be several kinds of executable at once:

| Boot path | How it enters the kernel |
| --------- | ------------------------ |
| **Legacy BIOS** (the test box) | GRUB loads the image, fills in the **setup header** (the "boot protocol") and jumps to the setup code or the 32/64-bit entry point |
| **UEFI** | With `CONFIG_EFI_STUB=y` the bzImage is **also a PE32+ executable**, so UEFI firmware can run it directly as an EFI application. The **EFI stub** then calls the kernel's entry code. |
| **ARM64** | The bootloader jumps to `Image` with the MMU off and the Device Tree's address in register `x0`. It can also be an EFI application via the EFI stub. |
| **Android** | A raw `boot` **partition** holds `boot.img` (`ANDROID!` header + kernel `Image.gz` + ramdisk), parsed by the vendor bootloader. **⚠️ Verify** what the instructor meant (see Open Questions). |

<details>
<summary>▶ Deep dive: inside a bzImage (layout, <code>file</code> output, PE header bytes)</summary>

**Layout:**

```text
offset 0x000  ┌──────────────────────────────┐  "MZ": DOS/PE header (for UEFI)
              │ legacy boot sector (512 B)   │  now only prints "Use a boot loader."
offset 0x1F1  │ setup header ("HdrS" @0x202) │  boot-protocol fields filled in by GRUB
              ├──────────────────────────────┤
              │ real-mode setup code         │  BIOS path: 16-bit → protected → long mode
              ├──────────────────────────────┤
              │ compressed kernel + stub     │  decompressor, then start of vmlinux
              └──────────────────────────────┘
```

**`file` output** (test box; images are mode `0600`, so run it with `sudo`):

```text
$ sudo file /boot/vmlinuz-6.8.0-142-generic
vmlinuz-6.8.0-142-generic: Linux kernel x86 boot executable bzImage,
  version 6.8.0-142-generic (buildd@lcy02-amd64-049) #142-Ubuntu SMP PREEMPT_DYNAMIC
  Wed Sep  2 14:24:27 UTC 2026, RO-rootFS, swap_dev 0XE, Normal VGA
```

| Field | Meaning |
| ----- | ------- |
| `bzImage` | x86 boot format: setup code + compressed kernel |
| `version … (buildd@…)` | Kernel release and the user@host that built it (Ubuntu build farm) |
| `#142-Ubuntu SMP PREEMPT_DYNAMIC` | Same string as `uname -v`: SMP kernel, preemption model chosen at boot (`preempt=`) |
| `RO-rootFS`, `swap_dev 0XE`, `Normal VGA` | Obsolete legacy header fields, ignored by modern bootloaders. **⚠️ Verify:** which field `file` labels `swap_dev`. |

**Hexdump: the PE header** (from class):

```text
$ sudo hexdump -C /boot/vmlinuz-6.8.0-142-generic | head
00000000  4d 5a 00 00 ...                                   |MZ..............|
00000030  00 00 00 00 00 00 00 00  cd 23 82 81 40 00 00 00  |.........#..@...|
00000040  50 45 00 00 64 86 04 00  ...                      |PE..d...........|
00000050  01 00 00 00 a0 00 06 02  0b 02 ...                |................|
```

| Offset | Bytes | Meaning |
| ------ | ----- | ------- |
| `0x00` | `4d 5a` | `"MZ"`: DOS header magic that every PE file starts with |
| `0x38` | `cd 23 82 81` | `LINUX_PE_MAGIC` (`0x818223cd`, `include/linux/pe.h`) |
| `0x3C` | `40 00 00 00` | `e_lfanew`: the PE header is at `0x40` |
| `0x40` | `50 45 00 00` | `"PE\0\0"` signature |
| `0x44` | `64 86` | Machine `0x8664` = x86-64 |
| `0x58` | `0b 02` | Optional-header magic `0x020b` = **PE32+** (64-bit) |

So the same file is both a bzImage (for GRUB/BIOS) and a PE32+ EFI application (for UEFI).

</details>

### Kernel image compression

The "z" in `vmlinuz` means *compressed*. The algorithm is chosen at build time (*General setup → Kernel compression mode*), and a small decompressor inside the bzImage unpacks the kernel at boot.

| Option | Best at | Note |
| ------ | ------- | ---- |
| `CONFIG_KERNEL_GZIP` | Compatibility | Historic default |
| `CONFIG_KERNEL_XZ` | **Smallest image** | Slow to decompress; popular on embedded systems |
| `CONFIG_KERNEL_LZ4` | **Fastest decompression** | Largest image |
| `CONFIG_KERNEL_ZSTD` | **Best trade-off** | Near-xz size at near-LZ4 speed (5.9+); **Ubuntu 24.04 default** (test box) |

Other choices: `BZIP2`, `LZMA`, `LZO`, `UNCOMPRESSED`. The initramfs (`CONFIG_RD_*`, test box: zstd) and modules (`CONFIG_MODULE_COMPRESS_*`, test box: `.ko.zst`) are compressed **separately**.

### Image size vs runtime footprint

| Measure (test box) | Size |
| ------------------ | ---- |
| `/boot/vmlinuz-…` on disk (zstd) | ≈ 15 MB |
| Decompressed image in RAM (boot log `Memory:` line: code + data + bss) | ≈ 50 MB (≈ 5 MB `init` part freed after boot) |
| Runtime allocations (slab, stacks, page tables, vmalloc, per-CPU) | ≈ 450 MB, and grows with workload |

The instructor's figure of **50–70 MB** is the loaded image plus its basic data.

### Kernel symbols: `System.map`, `/proc/kallsyms`, `kptr_restrict`

| | `System.map-<ver>` | `/proc/kallsyms` |
| - | ------------------ | ---------------- |
| Kind | **Static** file produced by the build | **Live**, generated by the running kernel (`CONFIG_KALLSYMS`) |
| Addresses | Link-time (no KASLR offset) | Real runtime addresses |
| Modules | No | Yes (`[module]` suffix) |
| Test box | Mode `0600`, root only | Readable, but addresses are zeros for non-root |

**`kptr_restrict`** (sysctl `kernel.kptr_restrict`) controls whether kernel pointers are shown:

| Value | Who sees real addresses | Default on |
| ----- | ----------------------- | ---------- |
| `0` | Root (`CAP_SYSLOG`); unprivileged users only if `perf_event_paranoid <= 1` | Upstream |
| `1` | Only readers with **`CAP_SYSLOG`** | **Ubuntu / test box** |
| `2` | **Nobody**, not even root | Android |

- **Why hide them:** a leaked kernel address defeats **KASLR**, which turns a memory-corruption bug into code execution. This is defence in depth: root can always change the setting.
- The check uses the credentials of whoever **opened** the file (§8).

### Commands / debugging

```sh
uname -r                                        # running kernel release, e.g. 6.8.0-139-generic
uname -m                                        # architecture: x86_64 (test box), aarch64 (Pi 5)
uname -v                                        # build string: #139-Ubuntu SMP PREEMPT_DYNAMIC <date>
ls -l /boot                                     # installed kernels, initramfs images and symlinks
cat /proc/cmdline                               # command line the running kernel was booted with
[ -d /sys/firmware/efi ] && echo UEFI || echo BIOS   # how this machine booted (test box: BIOS)
sudo file /boot/vmlinuz-$(uname -r)             # identify image format and version (needs root on Ubuntu)
grep -E '^CONFIG_KERNEL_(GZIP|XZ|LZ4|ZSTD)' /boot/config-$(uname -r)   # which image compression was used
grep CONFIG_PREEMPT /boot/config-$(uname -r)    # check any config option
lsinitramfs /boot/initrd.img-$(uname -r) | head # list initramfs contents (Ubuntu/Debian)
journalctl -k -b | grep 'Memory:'               # kernel image size in RAM from the boot log
cat /proc/sys/kernel/kptr_restrict              # pointer-hiding level (test box: 1)
sudo grep -w start_kernel /proc/kallsyms        # live (KASLR-shifted) address; non-root sees zeros
ls /lib/modules/$(uname -r)/                    # modules for the running kernel
```

Observed on the test box: running `-139`, but `/boot/vmlinuz` points to `-142`. A newer kernel is installed and will boot next time, so build modules for `-139` until you reboot.

### Pitfalls

- Building a module against a different kernel than `uname -r` → `insmod` fails with `Invalid module format` (vermagic mismatch, §7).
- After an upgrade without a reboot, `/lib/modules/$(uname -r)/build` still points to the old headers. That is **correct** for the running kernel: do not "fix" it.
- `System.map` addresses ≠ runtime addresses because of **KASLR**. Use `/proc/kallsyms` (as root).
- Do not delete old kernels from `/boot` by hand. Use the package manager (`apt autoremove`).

### Corrections to raw notes

| Raw notes said | Correct |
| -------------- | ------- |
| The boot executable boots from sector 0 of the disk | That was the pre-2.6.24 floppy boot sector. It no longer works; a bootloader is always needed. |
| "uefa: pe32" | **UEFI**, **PE32+** (64-bit) |
| xz is the most popular/efficient | xz gives the best **ratio** but decompresses slowly. Most distros now use **zstd**. |
| "vmlinux 16 megabytes" | That is the compressed **`vmlinuz`**. `vmlinux` with debug info is hundreds of MB. |
| `System.map` exists only on desktop/server Linux, not Android | Every build produces it. Android just does not ship it on the device, and SELinux blocks `/proc/kallsyms`. |
| `kptr_restrict=0` = visible to everyone | On Ubuntu `perf_event_paranoid=4`, so unprivileged users still see zeros. Only root gains visibility. |

### Revision questions

1. What is the difference between `vmlinux` and `vmlinuz`, and which one would you give to `crash` or `gdb`?
2. Why does a system need an initramfs, and when could you boot without one?
3. `/boot/vmlinuz` points to a different version than `uname -r`. What does that tell you, and which version must your module be built for?
4. Why might `System.map` addresses not match `/proc/kallsyms`?

<details>
<summary>Answers</summary>

1. `vmlinux` is the uncompressed ELF with symbols and debug info; `vmlinuz` is compressed, stripped and bootable. Debuggers need `vmlinux`.
2. It supplies the drivers and logic (modules, LVM, LUKS, RAID) needed to mount the real root. You can boot without one only if everything needed to mount root is built in (`=y`) and the root device is given with `root=`.
3. A newer kernel is installed but the machine has not rebooted. Build for the **running** kernel (`uname -r`).
4. KASLR shifts the kernel base at every boot, and `System.map` holds link-time addresses. Also, `kptr_restrict` shows zeros to unprivileged users.

</details>

### Source pointers

- `arch/x86/boot/` (setup code, `header.S`: boot sector, PE header, setup header), `arch/x86/boot/compressed/` (decompressor)
- `drivers/firmware/efi/libstub/` (EFI stub), `init/main.c` (`start_kernel()`), `init/initramfs.c`
- `Documentation/arch/x86/boot.rst` (x86 boot protocol), `Documentation/arch/arm64/booting.rst`
- `Documentation/filesystems/ramfs-rootfs-initramfs.rst`, `Documentation/admin-guide/kernel-parameters.txt`
- `init/Kconfig` (`KERNEL_*` compression), `kernel/ksyms_common.c` (`kallsyms_show_value()`)

---

## 2. Where Kernels Come From: Distribution vs Vendor (BSP) Kernels

> **Remember**
>
> - **Every** Linux kernel comes from kernel.org (**mainline** → **stable/LTS**). Others add patches on top.
> - PCs and servers run **distribution kernels**. Phones and boards run **vendor BSP kernels**.
> - Android **GKI**: one Google-built core kernel, with hardware support in **vendor modules** loaded against a stable **KMI**.
> - Patching the kernel directly creates a **fork** (endless rebasing), and **GPLv2** requires you to publish the source if you distribute it. Prefer modules, eBPF, or upstreaming.
> - Build modules against the **exact** kernel you run: "6.8" from Ubuntu ≠ "6.8" from kernel.org.

### Overview

Few systems run a pure mainline kernel. Knowing whether you are on a distro kernel or a vendor kernel tells you which source tree, config and patches your modules must be built against.

### How kernels are derived

```text
mainline (Linus, torvalds/linux.git)
   └─> stable / LTS (Greg KH, linux-6.12.y, ...)
          ├─> distribution kernels (PC/server): Ubuntu 6.8.0-NN-generic, Fedora, RHEL, SUSE, Debian
          │      + backports, security fixes, distro config
          └─> Android Common Kernel (ACK, Google) → GKI
                 └─> SoC vendor BSP kernels (Qualcomm, MediaTek, Samsung, ...)
                        └─> device (OEM) kernels
```

| | Distribution kernel | Vendor BSP kernel |
| - | ------------------- | ----------------- |
| Supplied by | Ubuntu, Fedora/RHEL, SUSE, Debian | SoC vendor: Qualcomm, MediaTek, NXP, TI, Rockchip, … |
| Hardware | Generic: one image for many PCs, most drivers as modules | One SoC family; board described by a **Device Tree** |
| Out-of-tree code | Minimal | Often large (GPU, modem, camera drivers) |
| Updates | Frequent, via the package manager | Frozen at one LTS; updates depend on the OEM |

- **BSP** (Board Support Package): the vendor's kernel tree, bootloader, Device Trees, drivers and firmware for its SoC.
- The Pi's kernel (`6.12.x+rpt-rpi-2712`) is itself a vendor kernel (`raspberrypi/linux`).

### GKI (Generic Kernel Image, Android 12+, kernel 5.10+)

```text
Before GKI (per-device kernel)          With GKI
┌──────────────────────────────┐        ┌──────────────────────────────┐
│ one monolithic kernel         │        │ GKI kernel (Google-built,     │  same binary for every
│ = LTS + Android + SoC vendor  │        │ signed, per LTS branch)       │  device on that branch
│   + OEM patches, all mixed    │        ├──────── KMI (stable) ────────┤  frozen list of exported
└──────────────────────────────┘        │ vendor modules (.ko): SoC,    │  symbols + types
                                         │ board, GPU, modem drivers     │
                                         └──────────────────────────────┘
```

| Advantage | Why |
| --------- | --- |
| Faster security updates | Google ships a new GKI without every vendor rebasing a private fork |
| Less fragmentation | One core kernel per branch instead of thousands of device forks |
| Stable vendor interface | The **KMI** is frozen within a branch, so vendor modules keep loading |
| Upstream alignment | Core changes must go upstream or into ACK |
| Central testing | One binary is tested and certified centrally |

- **Trade-offs:** vendors are limited to the KMI symbol list, and module loading at boot adds complexity (`vendor_boot` and `vendor_dlkm` partitions).
- GKI is a real-world example of §7: hardware support lives in **modules** against a controlled symbol list.

### Licensing: GPLv2 and why not to patch the kernel directly

- The kernel is **GPL-2.0-only**. Anyone who **distributes** a modified kernel must provide the source to recipients.
- **Avoid patching the kernel directly:**
  - **Legal:** the changes fall under GPLv2 and must be released with any binary you ship.
  - **Engineering:** you create a **fork** that must be rebased on every upstream release. This is the root of the Android update problem.
- **Preferred alternatives:** a **loadable module**, existing extension points (**eBPF**, tracepoints, Device Tree, sysfs), or **upstream** the change.
- **Modules:** `MODULE_LICENSE("GPL")` unlocks `EXPORT_SYMBOL_GPL()` symbols. A non-GPL licence **taints** the kernel (`P`). Whether a proprietary module is a derivative work is legally disputed (**⚠️ Verify** the course's position).

### Pitfalls

- Distro/BSP headers are **not** interchangeable with mainline sources of the same version number.
- Vendor trees often carry older or modified APIs, so mainline 6.x code may not build on them unchanged.

### Corrections to raw notes

| Raw notes said | Correct |
| -------------- | ------- |
| "All changes must be kept open source" | The obligation is triggered by **distribution**. Private changes that are never shipped need not be published. |

### Revision questions

1. Why is it hard to update the kernel on an old Android phone, and how does GKI help?
2. The test box runs `6.8.0-139-generic`. Is that a mainline kernel?
3. Give two reasons to write a module instead of patching the kernel.

<details>
<summary>Answers</summary>

1. Each device runs a vendor BSP kernel with out-of-tree drivers on an old LTS, so updates need the vendor and the OEM to rebase their patches. GKI separates one Google-maintained core kernel from vendor modules that load against a stable KMI, so the core can be updated on its own.
2. No. It is Ubuntu's distribution kernel: upstream 6.8 plus Ubuntu patches, backports and config. `-139` is Ubuntu's ABI/upload number.
3. No private fork to rebase on every release. The code stays separate from GPL obligations on the kernel proper (subject to licence). It can be loaded and unloaded without rebuilding the kernel.

</details>

### Source pointers

- `Documentation/process/2.Process.rst`, `Documentation/process/stable-kernel-rules.rst`
- `COPYING`, `LICENSES/`, `Documentation/process/license-rules.rst`, `Documentation/admin-guide/tainted-kernels.rst`
- `arch/arm64/boot/dts/qcom/`, `arch/arm64/boot/dts/mediatek/`
- Android: `source.android.com/docs/core/architecture/kernel` (GKI, KMI)

---

## 3. Virtual Address Space: User/Kernel Split

> **Remember**
>
> - Every process has its own **virtual address space**: **user space** in the low half (private), **kernel space** in the high half (shared by all processes, kernel mode only).
> - x86_64 with 4-level paging: user `0x0000_0000_0000_0000`–`0x0000_7fff_ffff_ffff` (**47 bits, 128 TiB**); kernel from `0xffff_8000_0000_0000`. The gap in between is **non-canonical** and faults.
> - An address works only if it is **mapped** (a page-table entry points to a physical page). Otherwise the CPU raises a **page fault**.
> - `mmap()` creates only a **VMA** (a reserved range). Physical pages are allocated **on first touch** (**demand paging**).
> - Kernel code must **never** dereference a user pointer. Use `copy_from_user()` / `copy_to_user()`.

### Overview

The number of usable address bits depends on the **architecture and the number of page-table levels**, not on installed RAM. 128 TiB of user space does not need 128 TiB of RAM.

### Layout (x86_64, 4-level paging)

```text
0xffff_ffff_ffff_ffff ┌────────────────────────────┐
                      │ kernel space (128 TiB)     │  direct map, vmalloc, vmemmap,
                      │ shared by all processes    │  kernel text (0xffffffff8…)
0xffff_8000_0000_0000 ├────────────────────────────┤
                      │ non-canonical hole         │  access → #GP fault
0x0000_7fff_ffff_ffff ├────────────────────────────┤  ← TASK_SIZE
                      │ user space (128 TiB)       │  stack (top), mmap/libs,
                      │ per process                │  heap, text (bottom)
0x0000_0000_0000_0000 └────────────────────────────┘
```

| Architecture / config | User space |
| --------------------- | ---------- |
| x86 32-bit | 3 GiB (classic **3G/1G split**, kernel at `0xc0000000`) |
| **x86_64, 4-level (default)** | **128 TiB (47 bits)** |
| x86_64, 5-level (`CONFIG_X86_5LEVEL` + CPU flag `la57`) | Up to 64 PiB (56 bits); addresses above 47 bits only if requested via an `mmap()` hint |
| ARM64 (`CONFIG_ARM64_VA_BITS` = 39/47/48/52) | 2^VA_BITS; separate page-table roots for user (`TTBR0_EL1`) and kernel (`TTBR1_EL1`) |

- **Test box:** `CONFIG_X86_5LEVEL=y`, but the vCPU lacks `la57`, so it runs **4-level**: 47-bit user space.
- **Pi 5:** `ARM64_VA_BITS=47`, 16 KiB pages, 128 TiB user space.

### Mapped vs unmapped (key term)

A virtual page is usable only if it is **mapped**. When a process touches an unmapped address, the MMU raises a **page fault** and the kernel checks whether the address lies in a valid **VMA**:

```text
page fault
  ├─ inside a VMA, page not present yet → demand paging: allocate/read page, fill PTE, retry (invisible)
  └─ no VMA, or wrong permissions      → SIGSEGV (user) / oops (kernel)
```

- **NULL dereference always faults:** the lowest pages are never mapped (`vm.mmap_min_addr` = 65536). This turns kernel NULL bugs into oopses rather than exploits.

### Reading `/proc/<pid>/maps`: libc example

```text
$ grep libc /proc/$$/maps
7de3a0c00000-7de3a0c28000 r--p 00000000 fc:00 1061951  /usr/lib/x86_64-linux-gnu/libc.so.6
7de3a0c28000-7de3a0db1000 r-xp 00028000 fc:00 1061951  /usr/lib/x86_64-linux-gnu/libc.so.6
7de3a0db1000-7de3a0e00000 r--p 001b1000 fc:00 1061951  /usr/lib/x86_64-linux-gnu/libc.so.6
7de3a0e00000-7de3a0e04000 r--p 001ff000 fc:00 1061951  /usr/lib/x86_64-linux-gnu/libc.so.6
7de3a0e04000-7de3a0e06000 rw-p 00203000 fc:00 1061951  /usr/lib/x86_64-linux-gnu/libc.so.6
```

| Column | Meaning |
| ------ | ------- |
| `start-end` | Virtual range of one **VMA** (all below `0x7fff_ffff_ffff`, so user space) |
| `r-xp` | Permissions + `p` private (copy-on-write) / `s` shared |
| `00028000` | Offset in the file |
| `fc:00`, `1061951` | Device (major:minor), inode |
| path | Backing file, or `[heap]`, `[stack]`, `[vdso]`, blank = anonymous |

One shared library → **one mapping per segment**: headers (`r--`), code `.text` (`r-x`), read-only data (`r--`), **RELRO** (`r--`, made read-only after linking), writable data `.data`/`.bss` (`rw-`). The code pages are **shared** physically by every process that uses libc. Only written pages get private copies.

### Where the kernel's own memory shows up

`maps` shows only user mappings. Kernel memory usage is in **`/proc/meminfo`**:

| Field | Meaning |
| ----- | ------- |
| `Slab` | Kernel object caches (`kmalloc`, dentries, inodes) |
| `KernelStack` | Kernel stacks of all threads |
| `PageTables` | Memory used by page tables |
| `VmallocUsed` | `vmalloc()` area in use |
| `Percpu` | Per-CPU allocations |

### Key APIs / structures

| Symbol | Header | Purpose | Context |
| ------ | ------ | ------- | ------- |
| `copy_from_user()` / `copy_to_user()` | `<linux/uaccess.h>` | Safe user ↔ kernel copy; returns bytes **not** copied | Process context, **may sleep** |
| `access_ok()` | `<linux/uaccess.h>` | Range lies below the user limit (does not check that it is mapped) | Process context |
| `__user` | `<linux/compiler_types.h>` | `sparse` annotation for user pointers | n/a |
| `TASK_SIZE` | `<asm/processor.h>` | Top of user space for `current` | Any |
| `PAGE_OFFSET` | `<asm/page.h>` | Start of the kernel's direct map of RAM | Any |

### Commands / debugging

```sh
cat /proc/$$/maps                       # layout of the current shell ($$ = shell's PID)
grep -m1 'address sizes' /proc/cpuinfo  # physical/virtual address bits the CPU supports
grep -o la57 /proc/cpuinfo | head -1    # prints la57 if the CPU supports 5-level paging (x86)
grep -E 'X86_5LEVEL|ARM64_VA_BITS|PGTABLE_LEVELS' /boot/config-$(uname -r)   # paging config
cat /proc/meminfo                       # system-wide memory, including kernel usage
sudo slabtop -o | head -15              # biggest kernel slab caches
sysctl vm.mmap_min_addr                 # lowest address user space may map (65536)
```

### Pitfalls

- Dereferencing a `__user` pointer directly: a bug even when it "works". **SMAP** (x86) / **PAN** (ARM64) make it fault. Catch it with `sparse` (`make C=1`).
- Calling `copy_*_user()` with a spinlock held or in interrupt context: it may sleep on a page fault.
- Assuming user addresses always fit in 47 bits (breaks under 5-level paging).

### Corrections to raw notes

| Raw notes said | Correct |
| -------------- | ------- |
| User space is "commonly 37–40 bits" | The range written next to it (`0x0`–`0x7fff_ffff_ffff`) is **47 bits** (x86_64). 39 bits is used on some ARM64/Android configs. |
| Kernel space at `0xfffffff?????` | Starts at `0xffff_8000_0000_0000` (x86_64, 4-level). The kernel **image** is at `0xffffffff8…`. |
| `mmap` claims a physical page and maps it | `mmap()` only creates the **VMA**. Pages are allocated lazily on first access, unless `MAP_POPULATE`/`mlock()` is used. |

### Revision questions

1. On x86_64 with 4-level paging, what are the user and kernel ranges, and what happens if you access `0x0000_8000_0000_0000`?
2. Why can't a driver `memcpy()` from a pointer passed in an `ioctl()` argument?
3. The test box has `CONFIG_X86_5LEVEL=y`. Why is its user space still 47 bits?
4. A program `mmap()`s 1 GiB and `MemFree` barely changes. Why?

<details>
<summary>Answers</summary>

1. User `0x0`–`0x0000_7fff_ffff_ffff`, kernel `0xffff_8000_0000_0000`–`0xffff_ffff_ffff_ffff`. `0x0000_8000_0000_0000` is non-canonical, so the CPU raises a general-protection fault (SIGSEGV in user space, an oops in the kernel).
2. It is a user virtual address. It may be unmapped, paged out or malicious (pointing into the kernel), and SMAP/PAN block direct access. `copy_from_user()` validates the range and handles faults.
3. The CPU also needs `la57`. Without it the kernel falls back to 4-level paging at boot. Even with it, addresses above 47 bits are only handed out when requested via an `mmap()` hint.
4. `mmap()` only creates a VMA. Pages are allocated on first touch (demand paging).

</details>

### Source pointers

- `Documentation/arch/x86/x86_64/mm.rst`, `Documentation/arch/x86/x86_64/5level-paging.rst`, `Documentation/arch/arm64/memory.rst`
- `arch/x86/include/asm/page_64_types.h` (`TASK_SIZE_MAX`), `include/linux/uaccess.h`
- `mm/memory.c` (`handle_mm_fault()`), `arch/x86/mm/fault.c` (`exc_page_fault()`), `mm/mmap.c`
- `Documentation/filesystems/proc.rst` (`maps`, `meminfo`)

---

## 4. vDSO and vsyscall: Kernel Code Mapped into User Space

> **Remember**
>
> - The **vDSO** is a small ELF shared library **supplied by the kernel** and mapped into every process as `[vdso]`, with a data page `[vvar]`.
> - It lets hot, read-only calls (`clock_gettime`, `gettimeofday`, `time`, `getcpu`) run **without entering the kernel**, which avoids a mode switch.
> - How it works: the kernel keeps the time data in `[vvar]` up to date, and the vDSO code reads that data plus the CPU counter, entirely in user mode.
> - **vsyscall** is the legacy x86_64 version at a **fixed** address (`0xffffffffff600000`). It is now emulated, because a fixed address helps exploits.
> - Real x86_64 syscalls use the **`syscall`** instruction with the number in `rax` (`read` = 0).
> - `strace` cannot see vDSO calls, because no syscall happens.

### How a call reaches the vDSO

```text
app: gettimeofday(&tv)                    app: getpid()
   └─> glibc wrapper                         └─> glibc wrapper
         └─> [vdso] __vdso_gettimeofday           └─> syscall instruction ─> kernel entry ─> sys_getpid
               reads [vvar] + rdtsc                     (mode switch, ~100 ns+)
               (user mode only)
```

- The kernel passes the vDSO address in the auxiliary vector (`AT_SYSINFO_EHDR`). glibc finds it there and binds these functions to it automatically.
- The vDSO's position is randomised by ASLR. It appears in `ldd` output as `linux-vdso.so.1`, but there is **no file on disk**.

| Machine | vDSO exports |
| ------- | ------------ |
| Test box (x86_64, 6.8) | `clock_gettime`, `clock_getres`, `gettimeofday`, `time`, `getcpu`, `sgx_enter_enclave` |
| Pi 5 (arm64, 6.12) | `clock_gettime`, `clock_getres`, `gettimeofday`, `getrandom` (6.11+), signal-return trampoline |

### vsyscall (legacy, x86_64 only)

- A page at the fixed address `0xffffffffff600000` providing `gettimeofday`, `time` and `getcpu`. Its fixed, executable address defeats ASLR, so it was replaced by the vDSO.
- It is kept only for very old static binaries. It is **emulated**: the page is execute-only (`--xp`), and a call traps into the kernel, which does a real syscall. Boot option: `vsyscall=xonly|emulate|none`.

### x86 system-call entry instructions

| Instruction | Where | Notes |
| ----------- | ----- | ----- |
| `int 0x80` | 32-bit x86 | Original software-interrupt gate; slowest |
| `sysenter` / `sysexit` | 32-bit mode | Fast entry, Pentium II onwards |
| **`syscall` / `sysret`** | **All x86_64** | **The modern 64-bit instruction** |

On 32-bit x86, the vDSO (`linux-gate.so.1`) supplies `__kernel_vsyscall`, which picks the fastest instruction the CPU supports: the "syscall gate".

### Commands / debugging

```sh
grep -E 'vdso|vvar|vsyscall' /proc/self/maps   # where the kernel-supplied pages are mapped
LD_SHOW_AUXV=1 /bin/true | grep SYSINFO        # AT_SYSINFO_EHDR = vDSO address passed by the kernel
ldd /bin/ls | grep vdso                        # linux-vdso.so.1: listed, but no file on disk
strace -e trace=clock_gettime date             # no clock_gettime syscall shown: served by the vDSO
cat /sys/devices/system/clocksource/clocksource0/current_clocksource   # tsc = vDSO fast path works
```

### Pitfalls

- `strace` does not show vDSO calls. Use `ltrace` or `perf` / uprobes instead.
- If the clocksource cannot be read from user space (e.g. an unstable TSC in a VM), the vDSO falls back to a real syscall and loses its speed advantage.
- `vsyscall=none` breaks very old static binaries.

### Corrections to raw notes

| Raw notes said | Correct |
| -------------- | ------- |
| `memset` is a vDSO function | It is **not** in either vDSO. CPU-optimised `memset` comes from glibc (IFUNC, chosen at load time) or, in the kernel, from **alternatives** patched at boot. **⚠️ Verify.** |
| vsyscall "syscall gate used in Intel only" | vsyscall is **x86_64-specific** (Intel and AMD). The "syscall gate" is the 32-bit vDSO's `__kernel_vsyscall`. |
| vsyscall wraps **all** syscalls, picking `int`/`syscall`/`sysenter`; `sysenter` is the modern one | Picking the instruction is `__kernel_vsyscall` in the 32-bit vDSO. The x86_64 vsyscall page only ever had 3 functions. Normal syscalls go through glibc, which executes `syscall` directly. On x86_64 **`syscall`** is the modern instruction. **⚠️ Verify.** |
| `read` is syscall 3 | 3 on **i386**; **0** on x86_64 (`arch/x86/entry/syscalls/syscall_64.tbl`) |

### Revision questions

1. Why is `clock_gettime()` cheaper than `getpid()`?
2. Why was vsyscall replaced by the vDSO?
3. Does the vDSO appear in `ldd` output? Is there a file for it on disk?

<details>
<summary>Answers</summary>

1. The vDSO serves `clock_gettime()` by reading `[vvar]` and the CPU counter in user mode, with no syscall. `getpid()` is a real syscall.
2. vsyscall sits at a fixed, executable address in every process, which defeats ASLR and gives exploits useful gadgets. The vDSO is at a randomised address and can be extended with new functions.
3. Yes, as `linux-vdso.so.1`. There is no file: the image is built into the kernel and mapped at `exec()`.

</details>

### Source pointers

- `arch/x86/entry/vdso/`, `arch/x86/entry/vsyscall/vsyscall_64.c`, `arch/arm64/kernel/vdso/`
- `lib/vdso/gettimeofday.c` (generic vDSO time code), `man 7 vdso`

---

## 5. The First Processes: PID 0, PID 1 (`init`) and PID 2 (`kthreadd`)

> **Remember**
>
> - **PID 0** = the **idle task** (`swapper`, `init_task`). It is built into the kernel image, not forked. It creates PID 1 and PID 2 in `rest_init()`, which is why both show **PPID 0**.
> - **PID 1** starts as a kernel thread, then `exec`s **systemd**. It is the ancestor of every user process, adopts orphans, and if it exits the kernel **panics**.
> - **PID 2 `kthreadd`** creates **every other kernel thread**, so `ps --ppid 2` lists them all.
> - **Kernel threads** have no user address space (`mm == NULL`). That is why `ps` shows them as `[name]` with `VSZ 0` / `RSS 0`.
> - Every thread, process and kernel thread is one **`struct task_struct`**. A "PID" is really the thread group ID (**TGID**).

### How it works

```text
start_kernel()                       PID 0 (idle / swapper), static init_task
  └─> rest_init()
        ├─ user_mode_thread(kernel_init)  → PID 1
        │     finish init, free __init memory,
        │     run /init (initramfs) or /sbin/init  ──exec──> systemd (user space)
        │           └─> all user processes (login, sshd, bash, ...)
        ├─ kernel_thread(kthreadd)        → PID 2
        │     loop: take requests from kthread_create_list → fork kernel threads
        │           └─> kworker/*, ksoftirqd/*, migration/*, rcu_*, kswapd0, ...
        └─ cpu_startup_entry()            PID 0 becomes the idle loop (one per CPU)
```

### Key concepts

- **Tasks, not processes:** threads are just tasks that share an `mm`, open files, etc. (`clone()` flags). Each thread has its own TID. The process's PID is the TGID.
- **`/proc/<pid>`** entries are **generated on demand** when looked up (procfs is a pseudo-filesystem). Threads are in `/proc/<pid>/task/<tid>`.
- **Why kernel threads show 0 memory:** `ps` reports **user** memory only. Their stacks and data are kernel memory (`KernelStack` and `Slab` in `/proc/meminfo`).
- **Lazy TLB (why kernel threads are cheap):** a kernel thread borrows the previous task's page tables (`active_mm`). The kernel half is identical in every address space, so there is no page-table switch or TLB flush.
- **Parent vs adoption:** a kernel thread's parent is PID 2 because **kthreadd created it**. Adoption (reparenting when a parent dies) is a user-process mechanism, handled by PID 1 or a subreaper (`PR_SET_CHILD_SUBREAPER`).

### Tasks: `task_struct`, TCB vs PCB, and PID vs TGID

Classic OS textbooks keep two kinds of descriptor:

- **TCB (thread control block):** per-thread state: registers/saved context, kernel stack, thread state (running, sleeping, …), scheduling class, priority and CPU affinity.
- **PCB (process control block):** per-process state: the address space, and handles such as open files and sockets, credentials, signal handlers, and the list of its threads.

**Linux unifies both into one structure, `struct task_struct`** (`include/linux/sched.h`). There is one `task_struct` per thread. The "process" parts are not copied into each one: they live in separate, reference-counted structures that the threads of a process **share by pointer**. Which ones are shared is chosen by the `clone()` flags (`CLONE_VM`, `CLONE_FILES`, `CLONE_FS`, `CLONE_SIGHAND`, `CLONE_THREAD`).

```text
 process (thread group, TGID 1000)
 ┌───────────────────────┐  ┌───────────────────────┐  ┌───────────────────────┐
 │ task_struct           │  │ task_struct           │  │ task_struct           │
 │ pid  = 1000 (leader)  │  │ pid  = 1001           │  │ pid  = 1002           │
 │ tgid = 1000           │  │ tgid = 1000           │  │ tgid = 1000           │
 │ __state, prio, stack, │  │ __state, prio, stack, │  │ __state, prio, stack, │   <- "TCB" part:
 │ thread (regs), se ... │  │ thread (regs), se ... │  │ thread (regs), se ... │      per thread
 └──┬──────┬──────┬──────┘  └──┬──────┬──────┬──────┘  └──┬──────┬──────┬──────┘
    │mm    │files │signal      │      │      │            │      │      │
    ▼      ▼      ▼            ▼      ▼      ▼            ▼      ▼      ▼
 mm_struct   files_struct   signal_struct  (+ fs_struct, sighand_struct, nsproxy, cred)
 (address    (fd table:     (shared signal
  space)      files,         state, rlimits)                              <- "PCB" part:
              sockets)                                                       shared
```

| Field / call | Kernel meaning | User-space name |
| ------------ | -------------- | --------------- |
| `task->pid` | Unique ID of **this thread** | **TID** (`gettid()`, `ps -L` column `LWP`) |
| `task->tgid` | ID of the thread group = PID of the first thread | **PID** (`getpid()`) |
| `task->group_leader` | The main thread (where `pid == tgid`) | The process |
| `/proc/<pid>/status` `Tgid:` / `Pid:` | Same two values | |

- So **every PID in the kernel names a thread**; a process is the group of tasks sharing one `tgid`. The main thread is the one where **`pid == tgid`**. In a single-threaded program the two values are equal.
- *Raw notes said "PID always a thread": true in the kernel (`task->pid`). In user space, though, `getpid()` returns the **TGID**, and the thread's own ID is called the TID. Raw notes also said `struct tast_struct`; the name is `struct task_struct`.*
- `fork()` = `clone()` sharing nothing (new `mm` copied on write) → new TGID. `pthread_create()` = `clone(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | …)` → same TGID, new TID.
- `task_struct` is large (several KiB on x86_64; it varies with config) and is allocated from its own slab cache (`task_struct` in `/proc/slabinfo`).

```c
pr_info("comm=%s pid(TID)=%d tgid(PID)=%d leader=%d\n",	/* print the current task's identities */
	current->comm,	/* task name (16 bytes, as in ps) */
	task_pid_nr(current),	/* this thread's ID: what user space calls the TID */
	task_tgid_nr(current),	/* thread-group ID: what user space calls the PID */
	thread_group_leader(current));	/* 1 if this is the main thread (pid == tgid) */
```

```bash
ps -eLf | head                                   # one line per thread: PID column = TGID, LWP column = TID
ls /proc/$$/task                                 # thread IDs (TIDs) of the current shell (just one)
grep -E '^(Tgid|Pid|Threads):' /proc/$$/status   # TGID, this task's PID (TID) and thread count
sudo grep -w task_struct /proc/slabinfo          # slab cache for task_structs: object size and count
```

### The task list: `init_task` and `for_each_process()`

- **`init_task`** (`init/init_task.c`) is the statically defined `task_struct` of **PID 0**, the very first task (the boot CPU's idle task, `swapper/0`). Every other task is created by copying from it, directly or indirectly. The other CPUs' idle tasks (`swapper/1`, …) are also PID 0, created by `fork_idle()`.
- All processes are linked into one **circular doubly-linked list** through `task->tasks` (a `struct list_head`), with `init_task` as its head. Only **thread-group leaders** (processes) are on this list. A process's threads are on a second list, `signal->thread_head`.
- `for_each_process(p)` starts at `init_task` and follows `.tasks.next` until it gets back to `init_task`. It therefore visits every process **except PID 0 itself**.

```text
           ┌──────────────────────────────────────────────────────────────┐
           ▼                                                              │
   init_task (PID 0) ─tasks.next─> systemd (1) ─> kthreadd (2) ─> ... ─> bash (1234)
           ▲                                                              │
           └──────────────────────────── tasks.prev ──────────────────────┘
   for_each_thread(p, t): walks p->signal->thread_head (all threads of one process)
```

```c
/* include/linux/sched/signal.h (6.x), simplified */
#define next_task(p) \
	list_entry_rcu((p)->tasks.next, struct task_struct, tasks)	/* the task after p in the list */
#define for_each_process(p) \
	for (p = &init_task; (p = next_task(p)) != &init_task; )	/* start after init_task, stop when back at it */
```

*Raw notes said "for each process(..) takes init_task does init.task.tasks.next": correct. The names are `for_each_process()` and `init_task.tasks.next`.*

| Macro | Visits | Header |
| ----- | ------ | ------ |
| `for_each_process(p)` | Every process (thread-group leader), not PID 0 | `<linux/sched/signal.h>` |
| `for_each_thread(p, t)` | Every thread `t` of process `p` | `<linux/sched/signal.h>` |
| `for_each_process_thread(p, t)` | Every thread in the system (nested loop) | `<linux/sched/signal.h>` |

**Locking:** the list changes as tasks fork and exit, so walk it inside `rcu_read_lock()` / `rcu_read_unlock()` (readers; may not sleep, §12). Use `read_lock(&tasklist_lock)` only if you need the list to be stable. To keep using a task after the walk, take a reference with `get_task_struct()` and drop it with `put_task_struct()`. `while_each_thread()` is deprecated: use `for_each_thread()`.

```c
#include <linux/sched/signal.h>	/* for_each_process(), for_each_thread() */
#include <linux/rcupdate.h>	/* rcu_read_lock(), rcu_read_unlock() */

static void list_tasks(void)	/* print every process and its thread count */
{	/* start of list_tasks() */
	struct task_struct *p, *t;	/* p = process (leader), t = one of its threads */
	int n;	/* thread counter */

	rcu_read_lock();	/* protect the task lists while we walk them; no sleeping until unlock */
	for_each_process(p) {	/* init_task.tasks.next, ... until back at init_task */
		n = 0;	/* reset the count for this process */
		for_each_thread(p, t)	/* every thread in p's thread group */
			n++;	/* count it */
		pr_info("%-16s pid=%d threads=%d\n", p->comm, p->pid, n);	/* name, PID (= TGID for a leader), threads */
	}	/* end of for_each_process */
	rcu_read_unlock();	/* end of the RCU read-side section */
}	/* end of list_tasks() */
```

⚠️ Verify: snippet not yet built on the test box; `init_task` is exported, so this works from a module.

### Reading `ps` output

| Item | Meaning |
| ---- | ------- |
| `[kthreadd]` | Kernel thread: empty `/proc/<pid>/cmdline`, so `ps` shows the name in brackets |
| `VSZ 0`, `RSS 0` | No user address space (`mm == NULL`) |
| `STAT` `S` / `I` / `<` / `s` | Sleeping / **Idle** kernel thread (not counted in the load average) / high priority / session leader |
| `systemd --deserialize=67` | systemd re-executed itself (e.g. after an upgrade) and restored its state from fd 67. Still PID 1. |
| `Kthread: 1` in `/proc/<pid>/status` | Flags a kernel thread |

<details>
<summary>▶ Common kernel thread names</summary>

| Name | Purpose |
| ---- | ------- |
| `kworker/<cpu>:<id>[H]` | **Workqueue** worker bound to a CPU (`H` = high priority) |
| `kworker/u<n>:<id>` | Unbound workqueue worker |
| `kworker/R-<wq>` | **Rescuer** thread for a `WQ_MEM_RECLAIM` workqueue: guarantees progress under memory pressure |
| `ksoftirqd/<cpu>` | Runs deferred softirqs under load |
| `migration/<cpu>` | Moves tasks between CPUs |
| `rcu_preempt`, `rcu_tasks_*` | RCU grace-period machinery |
| `kswapd0` | Page reclaim (one per NUMA node) |
| `cpuhp/<cpu>`, `idle_inject/<cpu>` | CPU hotplug, idle injection |

</details>

### Key APIs / structures

| API | Header | Purpose | Context |
| --- | ------ | ------- | ------- |
| `kthread_run(fn, data, namefmt, ...)` | `<linux/kthread.h>` | Create **and** start a kernel thread (a child of `kthreadd`) | Process context, may sleep |
| `kthread_create()` + `wake_up_process()` | `<linux/kthread.h>` | Create stopped, then start | Process context, may sleep |
| `kthread_should_stop()` | `<linux/kthread.h>` | Checked in the thread's loop | In the kthread |
| `kthread_stop(task)` | `<linux/kthread.h>` | Ask the thread to stop and wait for it to exit | Process context, sleeps |
| `current` | `<asm/current.h>` | The running task's `task_struct` | Process context |
| `current->mm` / `->active_mm` | `<linux/sched.h>` | `mm` is `NULL` in kernel threads; `active_mm` is the borrowed one | Any |

### Code example

Full file and `Makefile`: [`examples/kthread_demo/`](examples/kthread_demo/). **Built on 6.8 x86_64** (not loaded).

```c
// SPDX-License-Identifier: GPL-2.0
/* ^ SPDX tag: machine-readable licence of this file (required in kernel sources) */
/*
 * kthread_demo.c - start a kernel thread at load, stop it at unload.
 */
#include <linux/module.h>	/* module_init(), module_exit(), MODULE_*() macros */
#include <linux/kthread.h>	/* kthread_run(), kthread_stop(), kthread_should_stop() */
#include <linux/delay.h>	/* msleep_interruptible() */
#include <linux/sched.h>	/* current, struct task_struct */
static struct task_struct *worker;	/* handle to our kernel thread (NULL until started) */
static int worker_fn(void *data)	/* body of the kernel thread; 'data' is the arg from kthread_run() */
{					/* start of worker_fn() */
	while (!kthread_should_stop()) {	/* loop until kthread_stop() is called on us */
		pr_info("kthread demo: pid=%d mm=%p\n", current->pid, current->mm); /* mm is NULL: no user address space */
		msleep_interruptible(5000);	/* sleep ~5 s; wakes early when kthread_stop() wakes us */
	}				/* end of loop: a stop was requested */
	return 0;			/* exit code handed back to kthread_stop() */
}					/* end of worker_fn() */
static int __init kthread_demo_init(void)	/* runs at insmod */
{						/* start of kthread_demo_init() */
	worker = kthread_run(worker_fn, NULL, "kthread_demo"); /* create (via kthreadd) and wake the thread */
	if (IS_ERR(worker))			/* kthread_run() returns an ERR_PTR on failure, never NULL */
		return PTR_ERR(worker);		/* convert the error pointer to -errno; load fails */
	return 0;				/* success: thread is running, module is Live */
}						/* end of kthread_demo_init() */
static void __exit kthread_demo_exit(void)	/* runs at rmmod */
{						/* start of kthread_demo_exit() */
	kthread_stop(worker);			/* set should_stop, wake the thread, wait for it to return */
}						/* end: thread gone, safe to unload the code it ran */
module_init(kthread_demo_init);			/* register the entry point */
module_exit(kthread_demo_exit);			/* register the exit point */
MODULE_LICENSE("GPL");				/* GPL-compatible licence: needed for GPL-only kthread symbols */
MODULE_DESCRIPTION("Minimal kernel thread demo");	/* shown by modinfo */
```

**What to expect after `insmod`:** `ps` shows PPID 2 with VSZ/RSS 0, and `dmesg` shows `mm=0000000000000000`.

### Commands / debugging

```sh
ps -ef | head                        # PID 1 and PID 2 both have PPID 0
ps --ppid 2 -o pid,stat,comm         # every kernel thread (children of kthreadd)
ps -eo pid,ppid,vsz,rss,stat,comm    # VSZ/RSS are 0 for kernel threads
grep Kthread /proc/2/status          # Kthread: 1 marks a kernel thread
cat /proc/2/cmdline | wc -c          # 0 bytes: why ps shows [kthreadd]
ls /proc/1/task/                     # thread IDs (TIDs) of a process
ps -eLf | head                       # one line per thread (LWP column = TID)
pstree -p 1 | head                   # user-space process tree under systemd
```

### Pitfalls

- A kthread loop that never checks `kthread_should_stop()` makes `rmmod` hang forever in `kthread_stop()`.
- Forgetting `kthread_stop()` in module exit → the thread runs code that has been unloaded → oops.
- Kernel threads have no `mm`, so they must not touch user memory. They ignore signals unless they opt in (`allow_signal()`).
- If PID 1 exits → kernel panic ("Attempted to kill init!").

### Corrections to raw notes

| Raw notes said | Correct |
| -------------- | ------- |
| PID 1 is "created out of nothing" | PID 1 and PID 2 are **forked from PID 0** in `rest_init()`, the only tasks not created by `fork()`/`clone()` from user space |
| Creating a task "automatically produces `/proc/pid`" | procfs **generates** `/proc/<pid>` on demand when it is looked up; no files are created |
| "fake PID 2" | `kthreadd` is a **real** kernel thread |
| Kernel threads are "adopted" by PID 2 | Not adopted: PID 2 **created** them. Adoption is for orphaned user processes (PID 1). |
| "PID always a thread"; `struct tast_struct` | True in the kernel (`task->pid` = thread). User space calls that the **TID**; `getpid()` returns the **TGID**. The struct is `struct task_struct`. |

### Revision questions

1. Why do PID 1 and PID 2 both show PPID 0, and what is PID 0?
2. Why does `ps` show `VSZ`/`RSS` of 0 for `[kworker/0:1]`, and what page tables does it run on?
3. What is the difference between a kernel thread's parent being `kthreadd` and a user process being reparented to `init`?

<details>
<summary>Answers</summary>

1. Both are created directly by PID 0, the static idle/swapper task, in `rest_init()`. `ps` does not show PID 0.
2. Kernel threads have no user address space (`mm == NULL`), and `ps` reports user memory only. They borrow the previous task's page tables (`active_mm`, lazy TLB), since the kernel half is the same in all of them.
3. `kthreadd` really created every kernel thread. Reparenting happens only when a user process's parent dies, so that the orphan can be reaped.

</details>

### Source pointers

- `init/main.c` (`rest_init()`, `kernel_init()`), `init/init_task.c`
- `kernel/kthread.c` (`kthreadd()`), `kernel/workqueue.c`, `Documentation/core-api/workqueue.rst`
- `kernel/exit.c` (orphan reparenting), `kernel/sched/core.c` (`context_switch()`, lazy TLB)

---

## 6. Kernel Headers: In-Tree, Module-Build and UAPI

> **Remember**
>
> - "Kernel headers" means **three different things**:
>   1. **Internal** headers in the source tree (`include/linux/`): for kernel code, **no stable API**.
>   2. The **headers package** (`/lib/modules/$(uname -r)/build`): for building modules against **one exact** installed kernel.
>   3. **UAPI** headers (`/usr/include/linux/`): for user-space programs, a **stable ABI**.
> - The headers package includes `.config` and `Module.symvers`, not just `.h` files.
> - Always build modules through Kbuild: `make -C /lib/modules/$(uname -r)/build M=$PWD modules`.

### The three header sets

| | Internal headers | Headers package | UAPI headers |
| - | ---------------- | --------------- | ------------ |
| Location | `include/linux/`, `arch/<arch>/include/asm/` | `/usr/src/linux-headers-<ver>` via `/lib/modules/$(uname -r)/build` | `/usr/include/linux/`, `/usr/include/asm/` |
| Used by | The kernel and modules | Out-of-tree modules for **that** kernel | User-space programs, glibc |
| Stability | **None**: changes every release | Matches one build (version + `.config`) | **Stable**: never broken on purpose |
| Comes from | kernel.org source | `linux-headers-$(uname -r)` package | `include/uapi/` → `make headers_install`; Ubuntu `linux-libc-dev` |

- `#include <linux/module.h>` → `include/linux/module.h`. `#include <asm/page.h>` → `arch/x86/include/asm/page.h` for the architecture being built.
- **No stable in-kernel API** (`Documentation/process/stable-api-nonsense.rst`): internal functions and structs change freely, and all in-tree users are updated with them. Only the user-space boundary (syscalls, UAPI, `/proc`, `/sys`) is stable.
- **What the headers package adds beyond `.h` files:** `.config`, `Module.symvers` (exported symbols + CRCs), `include/generated/` (`autoconf.h`, `utsrelease.h`), Makefiles, Kconfig, `scripts/`.

### Test box layout

```text
/lib/modules/6.8.0-139-generic/build ──> /usr/src/linux-headers-6.8.0-139-generic   (flavour: .config,
                                            │                                          Module.symvers, generated/)
                                            └─ symlinks ──> /usr/src/linux-headers-6.8.0-139/   (common part:
                                                                                     headers, Kbuild, scripts)
/usr/include/linux/                     UAPI headers (linux-libc-dev)
~/Advanced_Linux_Kernel_Course/linux/     full source 6.12.111 (shallow git clone)
~/Advanced_Linux_Kernel_Course/linux7.2/  full source 7.2.8
```

Both the `-139` (running) and `-142` (installed) header sets are present. Use `-139` until you reboot (§1).

### Kernel source tree: top-level directories

The ones to know first are in **bold**. The headers package has the same directory names but mostly only Makefiles, Kconfig and headers. Ubuntu adds `ubuntu/`.

| Directory | Contents |
| --------- | -------- |
| **`arch/`** | Architecture-specific code (`x86/`, `arm64/`): boot, syscall entry, MMU, `include/asm/`, Device Trees |
| **`drivers/`** | Device drivers: by far the largest directory |
| **`fs/`** | VFS plus each filesystem (`ext4/`, `proc/`, `sysfs/`) |
| **`include/`** | `linux/` (internal), `uapi/` (user-space API), `asm-generic/` |
| **`init/`** | Boot initialisation: `main.c` (`start_kernel()`) |
| **`kernel/`** | Core: scheduler (`sched/`), `fork.c`, signals, timers, locking, RCU, `kthread.c`, tracing, BPF, modules |
| **`mm/`** | Memory management: page allocator, SLUB, `vmalloc`, page faults, reclaim |
| **`net/`** | Networking stack |
| `block/` | Block layer, I/O schedulers |
| `crypto/`, `certs/` | Crypto API; keys for module signing |
| `ipc/`, `io_uring/` | System V IPC and message queues; io_uring |
| `lib/` | Helpers: strings, lists, rbtrees, CRCs, decompressors |
| `security/` | LSMs: SELinux, AppArmor, Landlock, Yama |
| `sound/`, `virt/`, `rust/` | ALSA; KVM core; Rust support |
| `samples/` | Small example modules: good learning material |
| `scripts/`, `tools/` | Build helpers (`checkpatch.pl`, `get_maintainer.pl`, modpost); `perf`, `bpftool`, selftests |
| `usr/` | Builds the initramfs embedded in the kernel |
| `Documentation/` | reStructuredText docs (also at docs.kernel.org) |
| `MAINTAINERS`, `COPYING`, `LICENSES/`, `Makefile`, `Kconfig`, `Kbuild` | Maintainers list; licence; top of the build system (the version number is at the top of `Makefile`) |

### Commands / debugging

```sh
sudo apt install linux-headers-$(uname -r)              # install headers for the running kernel
readlink -f /lib/modules/$(uname -r)/build              # where module builds look for headers
make -C /lib/modules/$(uname -r)/build M=$PWD modules   # build an out-of-tree module in this directory
dpkg -S /usr/include/linux/types.h                      # shows linux-libc-dev: these are UAPI headers
make headers_install INSTALL_HDR_PATH=/tmp/uapi         # export UAPI headers from a source tree
grep -rn 'EXPORT_SYMBOL' kernel/kthread.c | head        # which functions modules are allowed to call
```

### Pitfalls

- Including `<linux/...>` from `/usr/include` (`-I/usr/include`) when building a module: those are **UAPI** headers. Always build through Kbuild.
- User-space code must never include kernel-internal headers.
- Headers from a different version/config than the running kernel → build errors, or `Invalid module format` / `disagrees about version of symbol` at load time.
- Code written against the 7.2 tree may not build against the test box's 6.8 headers.

### Revision questions

1. What is the difference between `/usr/include/linux/sched.h` and `include/linux/sched.h` in the source tree?
2. Why does the headers package need `Module.symvers` and `.config`, not just `.h` files?
3. Why can Linux promise user space a stable ABI but not promise module authors a stable API?

<details>
<summary>Answers</summary>

1. The first is the **UAPI** header for user space (clone flags, scheduling policies). The second is internal and defines `struct task_struct` etc. for kernel code only.
2. `.config` (via `autoconf.h`) decides struct layouts and which code exists. `Module.symvers` lists the exported symbols and their CRCs, so modpost can resolve and version-check the module's imports against that exact kernel.
3. The syscall/UAPI boundary is small and deliberately frozen ("don't break user space"). Internal interfaces must stay free to change so they can be improved. In-tree users are updated together, and out-of-tree code must follow or be upstreamed.

</details>

### Source pointers

- `include/linux/`, `include/uapi/`, `arch/x86/include/asm/`
- `Documentation/kbuild/modules.rst`, `Documentation/kbuild/headers_install.rst`, `Documentation/process/stable-api-nonsense.rst`
- `scripts/mod/modpost.c`

---

## 7. Loadable Kernel Modules (LKMs)

> **Remember**
>
> - A **module** (`.ko`) is kernel code loaded into the **running** kernel. It runs in kernel mode with full privileges: **no sandbox**, and a bug can crash the whole system.
> - It can only use **exported** symbols and hook into **registration APIs** (drivers, filesystems, netfilter, …). It cannot replace the scheduler, page allocator or syscall table. Custom scheduling uses **sched_ext** (BPF), not modules.
> - **`MODULE_LICENSE()`** is mandatory. A non-GPL licence **taints** the kernel and blocks `EXPORT_SYMBOL_GPL` symbols (≈ 60 % of exports).
> - **vermagic** in `.modinfo` ties a `.ko` to one kernel build: if it does not match, the load fails.
> - `insmod`/`rmmod` do **no** dependency handling. `modprobe` / `modprobe -r` do, using `modules.dep`.
> - Every module needs `module_init()` + `module_exit()`. If init fails, undo everything (`goto` unwinding); in exit, clean up everything.

### Overview

Modules are how distributions ship **one generic kernel** for all hardware: drivers, filesystems, netfilter and crypto are loaded on demand. The test box has about 95 modules loaded.

### Life cycle

```text
hello.c ──Kbuild (make -C /lib/modules/$(uname -r)/build M=$PWD)──> hello.ko
   (modpost checks imports against Module.symvers, adds vermagic + .modinfo)

insmod hello.ko / modprobe hello
   └─> finit_module() syscall   (needs CAP_SYS_MODULE, §8)
         ├─ check signature (CONFIG_MODULE_SIG), vermagic, symbol CRCs (modversions)
         ├─ allocate memory, relocate, resolve symbols against exports
         ├─ apply module_param values
         └─ call module_init() function ── returns 0 → "Live"; error → unloaded
rmmod hello
   └─> delete_module() syscall: refcount must be 0 → module_exit() → free
```

### Licences: `MODULE_LICENSE()`

The kernel only compares the **string**. These count as GPL-compatible:

| String | Meaning |
| ------ | ------- |
| `"GPL"` | GPL v2 or later (the usual choice) |
| `"GPL v2"` | GPL v2 only |
| `"GPL and additional rights"` | GPL v2 plus extra rights |
| `"Dual BSD/GPL"`, `"Dual MIT/GPL"`, `"Dual MPL/GPL"` | Dual-licensed |
| anything else (e.g. `"Proprietary"`) | **Taints** the kernel (`P`); GPL-only symbols forbidden |

A missing `MODULE_LICENSE()` is a **build error** in current kernels.

### `.modinfo`: metadata inside the `.ko`

The `MODULE_*()` macros store `key=value` strings in the ELF section `.modinfo`. `modinfo` prints them. The instructor stressed **`license`**, **`parm`** and **`vermagic`**:

```text
license=GPL                                    ← MODULE_LICENSE()
parm=count:Number of greetings to print ...    ← MODULE_PARM_DESC()
parmtype=count:int                             ← module_param()
depends=                                       ← added by modpost: modules this one needs
vermagic=6.8.0-139-generic SMP preempt mod_unload modversions   ← added by the build
```

- **vermagic** = kernel release + key options. A mismatch → `Invalid module format`. (`modprobe --force-vermagic` overrides it, which is dangerous and taints the kernel with `F`.)
- **modversions** also checks a **CRC per imported symbol** against `Module.symvers`, which catches ABI changes even when vermagic matches.
- **`alias`** (from `MODULE_DEVICE_TABLE`) lets udev auto-load drivers for detected hardware. **`depends`** tells `modprobe` what to load first.

### Key APIs / structures

| Macro / function | Header | Purpose |
| ---------------- | ------ | ------- |
| `module_init(fn)` / `module_exit(fn)` | `<linux/module.h>` | Entry/exit points (process context, may sleep) |
| `__init` / `__exit` | `<linux/init.h>` | Code freed after init / dropped if built in |
| `MODULE_LICENSE()`, `MODULE_DESCRIPTION()`, `MODULE_AUTHOR()` | `<linux/module.h>` | Metadata; the licence also controls symbol access and taint |
| `module_param(name, type, perm)` + `MODULE_PARM_DESC()` | `<linux/moduleparam.h>` | Load-time parameter, visible in `/sys/module/<mod>/parameters/` |
| `EXPORT_SYMBOL()` / `EXPORT_SYMBOL_GPL()` | `<linux/export.h>` | Make a symbol usable by other modules |
| `MODULE_DEVICE_TABLE(type, table)` | `<linux/module.h>` | `alias=` entries for hardware auto-loading |
| `try_module_get()` / `module_put()` | `<linux/module.h>` | Pin a module while its code may still run |

### Code example

Full file and `Makefile`: [`examples/hello_module/`](examples/hello_module/). **Built on 6.8 x86_64** (not loaded).

```c
// SPDX-License-Identifier: GPL-2.0
/* ^ SPDX tag: machine-readable licence of this file (required in kernel sources) */
/*
 * hello.c - minimal loadable kernel module with a parameter.
 */
#include <linux/init.h>		/* __init / __exit section markers */
#include <linux/module.h>	/* module_init(), module_exit(), MODULE_*() macros */
#include <linux/moduleparam.h>	/* module_param(), MODULE_PARM_DESC() */
#include <linux/printk.h>	/* pr_info() logging to the kernel ring buffer */
static int count = 1;		/* module parameter: how many greetings; default 1 */
module_param(count, int, 0444);	/* expose 'count' as an int param, read-only in /sys/module/hello/parameters/ */
MODULE_PARM_DESC(count, "Number of greetings to print at load time"); /* description shown by modinfo (parm=) */
static int __init hello_init(void)	/* runs once at insmod; __init code is freed after loading */
{					/* start of hello_init() */
	int i;				/* loop counter */
	if (count < 0 || count > 10)	/* reject out-of-range parameter values */
		return -EINVAL;		/* non-zero return = load fails, module is not inserted */
	for (i = 0; i < count; i++)	/* repeat 'count' times */
		pr_info("hello: loaded (%d/%d)\n", i + 1, count); /* log a KERN_INFO message (see dmesg) */
	return 0;			/* 0 = success, module becomes "Live" */
}					/* end of hello_init() */
static void __exit hello_exit(void)	/* runs at rmmod; __exit code is dropped if built in */
{					/* start of hello_exit() */
	pr_info("hello: unloaded\n");	/* log that the module is being removed */
}					/* end of hello_exit(): no return value, cannot fail */
module_init(hello_init);		/* register hello_init() as the module's entry point */
module_exit(hello_exit);		/* register hello_exit() as the module's exit point */
MODULE_LICENSE("GPL");			/* GPL-compatible: allows GPL-only symbols, no taint */
MODULE_DESCRIPTION("Minimal hello-world loadable kernel module"); /* shown by modinfo */
MODULE_AUTHOR("Advanced Linux Kernel Programming course");	/* shown by modinfo */
```

```make
# Makefile: builds hello.c into the loadable kernel module hello.ko
obj-m := hello.o	# tell Kbuild to compile hello.c and link it as hello.ko
KDIR ?= /lib/modules/$(shell uname -r)/build	# kernel build tree: headers of the running kernel
all:	# default target, run by a plain "make"
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules	# enter the kernel tree (-C) and build the module in this dir (M=)
clean:	# target run by "make clean"
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean	# let Kbuild delete all generated files (.ko, .o, .mod.c, ...)
```

The build prints "Skipping BTF generation … unavailability of vmlinux". This is harmless.

### Module parameters

A **module parameter** is a global variable whose value can be set at load time and (optionally) read or changed later through sysfs.

```c
static int int_param = 0;		/* module parameter: an integer, default 0 */
module_param(int_param, int, 0644);	/* expose it as an int param; 0644 = root can change it at runtime in sysfs */
MODULE_PARM_DESC(int_param, "An integer parameter (default 0)");	/* description shown by modinfo (parm:) */
static char *filename = "none";		/* module parameter: a string (char pointer), default "none" */
module_param(filename, charp, 0444);	/* charp = kernel copies the string at load time; 0444 = read-only in sysfs */
MODULE_PARM_DESC(filename, "A file name string (default \"none\")");	/* description shown by modinfo (parm:) */
```

| `type` | C variable | Notes |
| ------ | ---------- | ----- |
| `int`, `uint`, `long`, `ulong`, `short`, `ushort`, `ullong`, `hexint` | matching integer | Bad input (`int_param=abc`) → load fails with `Invalid parameters` (`-EINVAL`) |
| `bool` / `invbool` | `bool` | Accepts `1/0`, `y/n`, `Y/N`; `invbool` stores the inverse |
| `byte` | `unsigned char` | A single **8-bit number**, not a character |
| `charp` | `char *` | A **string**: the kernel allocates a copy at load time |
| `module_param_string(name, buf, len, perm)` | `char buf[len]` | String copied into your fixed-size buffer |
| `module_param_array(name, type, &count, perm)` | array | Comma-separated: `vals=1,2,3`; `count` receives how many were given |
| `module_param_cb(name, &ops, &var, perm)` | any | Custom `set`/`get` callbacks, e.g. to **validate** or react to runtime writes |

**The `perm` argument** sets the permissions of `/sys/module/<mod>/parameters/<name>`:

| `perm` | Effect |
| ------ | ------ |
| `0` | Not visible in sysfs; set only at load time |
| `0444` | World-readable, read-only |
| `0644` | Readable; **root can change it at runtime** |
| World-writable (e.g. `0666`) | **Build error**: the kernel refuses world-writable parameter files |

**Setting a parameter:**

```bash
sudo insmod hello.ko int_param=42 filename=/etc/hostname   # at load, as name=value (no spaces around '=')
sudo dmesg | tail -2                                       # module logged: hello: int_param=42 filename=/etc/hostname
cat /sys/module/hello/parameters/int_param                 # read the live value
grep -r . /sys/module/hello/parameters/                    # all parameters as path:value
echo 7 | sudo tee /sys/module/hello/parameters/int_param   # change at runtime (only because perm is 0644)
sudo rmmod hello                                           # unload (runtime changes are lost)
echo 'options hello int_param=42' | sudo tee /etc/modprobe.d/hello.conf   # persistent default when loaded via modprobe
modinfo -F parm hello.ko                                   # list parameters: name:description (type)
```

- **Built-in** code takes the same parameters on the kernel command line as `<module>.<param>=value` (e.g. `printk.time=1`).
- *Class note: "char called filename". A file name is a string, so the type is `charp` (`char *`). A single `char` would be `byte`, which holds an 8-bit number.*

**Pitfalls:**

- **Runtime writes are silent.** A write to a `0644` parameter changes the variable, but the module is not told. Code that read the value at init will not see the change. Use `module_param_cb()` to validate or react to writes.
- **No locking.** A runtime write can race with code reading the variable. Read it once (`READ_ONCE()`), or protect it with a callback and a lock.
- Values given at load are **not validated** beyond the type. Check ranges in `module_init` and return `-EINVAL`.
- Don't keep a `charp` pointer after the module is unloaded, and don't `kfree()` it yourself: the parameter code owns it.

### Dependencies: `rmmod` vs `modprobe` (class demo: `vfat` → `fat`)

`vfat` uses symbols exported by `fat`, so `fat`'s refcount counts `vfat` as a user:

```text
$ lsmod | grep fat
vfat     20480  0
fat      86016  1 vfat                              ← used by 1 module: vfat
$ sudo rmmod fat
rmmod: ERROR: Module fat is in use by: vfat        ← refcount > 0, refused
$ sudo rmmod vfat && sudo rmmod fat                ← reverse order: works
$ sudo modprobe vfat                               ← loads fat first, then vfat
$ sudo modprobe -r vfat                            ← removes vfat, then the now-unused fat
```

| Tool | Handles dependencies? | Takes |
| ---- | --------------------- | ----- |
| `insmod` | No: `Unknown symbol` if a dependency is missing | Path to a `.ko` |
| `rmmod` | No: refuses if the module is in use | Module name |
| `modprobe` / `modprobe -r` | **Yes**, via `modules.dep` (generated by `depmod`) | Module name or alias |

- **On the course machines this demo will not work as shown:** `fat`/`vfat` are **built in** (`modinfo vfat` → `filename: (builtin)`), and built-in code can never be unloaded. Test-box alternative: `udf` depends on `crc-itu-t`. *Loading or unloading modules needs root: ask first.*

### `/proc/modules` vs `/sys/module/`

| Path | Contents |
| ---- | -------- |
| `/proc/modules` | One line per **loaded** module: name, size, refcount, users, state, address. `lsmod` just formats this file. |
| `/sys/module/<name>/` | Loaded modules **and** built-in code that has parameters: `parameters/`, `refcnt`, `holders/`, `sections/`. Built-in parameters are set on the kernel command line as `<module>.<param>=value`. |

Test box: 94 loaded modules, but 219 entries in `/sys/module/`, because of built-ins such as `printk` and `kernel`.

### Kernel sysctls (`/proc/sys/kernel`) and `modules_disabled`

`/proc/sys/` is the **sysctl** interface: each file is a run-time kernel tunable. `/proc/sys/kernel/` holds core-kernel settings (133 entries on the test box). Read them with `cat` or `sysctl kernel.<name>`; writing needs root (`CAP_SYS_ADMIN` for most). Changes are lost on reboot unless put in `/etc/sysctl.d/*.conf`. Reference: [Documentation for /proc/sys/kernel/](https://docs.kernel.org/admin-guide/sysctl/kernel.html) (`Documentation/admin-guide/sysctl/kernel.rst`).

| sysctl (`/proc/sys/kernel/…`) | Purpose |
| ----------------------------- | ------- |
| `modules_disabled` | `1` = no more module loading **or** unloading, until reboot (one-way) |
| `tainted` | Taint bitmask (see above) |
| `kptr_restrict`, `dmesg_restrict` | Hide kernel pointers / restrict `dmesg` to privileged users |
| `kexec_load_disabled` | `1` = forbid loading a new kernel with `kexec_load()` (also one-way) |
| `printk` | Console log levels (current, default, minimum, boot default) |
| `panic`, `panic_on_oops` | Reboot N s after a panic; turn an oops into a panic |
| `pid_max`, `threads-max` | Upper limits for PIDs and threads |
| `sysrq` | Which magic SysRq functions are allowed |
| `yama/ptrace_scope` | ptrace restriction level (§10) |

**`modules_disabled`:**

- Writing `1` makes `init_module()`, `finit_module()` and `delete_module()` fail with `EPERM`. So `insmod`, `modprobe` (including automatic on-demand loading) and `rmmod` all stop working. *Raw notes said "stops install and removal of kernel mods": correct, both loading and unloading are blocked.*
- It is **one-way**: writing `0` back is refused. Only a reboot clears it. That is the point: an attacker who later gains root cannot load a rootkit module.
- Typical use: hardened servers set it at the end of boot, once all needed modules are loaded (e.g. a late `sysctl.d` file or systemd unit). Modules already loaded keep working.
- Related hardening: Secure Boot **lockdown** (`/sys/kernel/security/lockdown`; test box: `[none] integrity confidentiality`) and module signing (`module.sig_enforce=1`) restrict *which* modules load, rather than stopping all of them.

```bash
ls /proc/sys/kernel                                # list the core-kernel sysctls
sysctl kernel.modules_disabled                     # read it (0 = loading allowed; test box: 0)
echo 1 | sudo tee /proc/sys/kernel/modules_disabled   # disable module load/unload until reboot (irreversible!)
sudo sysctl -w kernel.modules_disabled=1           # same thing through the sysctl tool
```

*Raw notes wrote `echo 1 > /proc/sys/kernel/modules_disabled`. That only works in a root shell: `sudo echo 1 > file` fails because the redirection runs as your user, so use `sudo tee` or `sysctl -w`. Do not run it on the test box unless you want to reboot before the next lab.*

### Commands / debugging

```sh
lsmod                                   # loaded modules, size and users (formats /proc/modules)
modinfo hello.ko                        # license, vermagic, params, depends
modinfo -F vermagic hello.ko            # print a single field
uname -r                                # compare with vermagic: they must match
sudo insmod hello.ko count=3            # load a file with a parameter (no dependency handling)
sudo modprobe <name>                    # load by name from /lib/modules, dependencies first
sudo rmmod hello                        # unload (refused while in use)
sudo dmesg | tail                       # pr_info() output (dmesg_restrict=1 on the test box)
cat /sys/module/hello/parameters/count  # read a parameter at runtime
cat /proc/sys/kernel/tainted            # 0 = clean; non-zero after proprietary/unsigned/out-of-tree modules
objcopy -O binary -j .modinfo hello.ko /dev/stdout | tr '\0' '\n'   # dump the raw .modinfo section
```

### Pitfalls

- `module_init` returning an error means the module is **not** loaded, so undo any partial setup first (`goto` unwinding).
- Missing cleanup in `module_exit` (timers, kthreads, callbacks) → the kernel later calls freed code → oops.
- Out-of-tree/unsigned modules taint the kernel (`O`, `E`). With Secure Boot + lockdown, unsigned modules are refused.
- A GPL-only symbol without `MODULE_LICENSE("GPL")` → modpost error `… is a GPL-only symbol`.

### Corrections to raw notes

| Raw notes said | Correct |
| -------------- | ------- |
| Modules can't affect low-level scheduling "until 7.1/7.2" | Custom scheduling became possible in **6.12** with **sched_ext** (built-in `CONFIG_SCHED_CLASS_EXT`). The policy is a **BPF program**, not a module. **⚠️ Verify** what 7.1/7.2 referred to. |
| Licences: "gpl gplv2 lgpl mt" | There is **no `"LGPL"`** string: it would taint. "mt" is presumably `"Dual MIT/GPL"`. |
| `MODULE_LICENSE` is "legally binding as open source" | It is a **declaration** the kernel acts on technically (taint, symbol access). The legal obligations come from the code's actual licence. |
| `MODULE_PARAM` | `module_param()` + `MODULE_PARM_DESC()` |
| `rmod`, `/sys/modules` | **`rmmod`**, **`/sys/module`** (singular) |

### Revision questions

1. What can a module do, and what can it not change? How does sched_ext fit?
2. What is the difference between `insmod` and `modprobe`?
3. Why does `MODULE_LICENSE()` matter at build and load time?
4. A module built on another machine fails with `Invalid module format`. What do you check first?

<details>
<summary>Answers</summary>

1. It can add drivers, filesystems, protocols and hooks through exported APIs and registration points. It cannot replace non-exported core machinery (scheduler classes, syscall table, page allocator). sched_ext (6.12+) allows custom scheduling policies as BPF programs attached to a built-in scheduling class.
2. `insmod` loads one given file with no dependency resolution. `modprobe` finds the module by name, loads its dependencies first (`modules.dep`) and applies `/etc/modprobe.d` options.
3. Only GPL-compatible licences may use `EXPORT_SYMBOL_GPL` symbols (modpost and the loader enforce this). Non-GPL modules taint the kernel (`P`).
4. `modinfo -F vermagic` against `uname -r`: the module must be built against the running kernel's headers.

</details>

### Source pointers

- `kernel/module/main.c` (`load_module()`), `include/linux/module.h`, `include/linux/export.h`, `include/linux/license.h`
- `kernel/sched/ext.c`, `Documentation/scheduler/sched-ext.rst`
- `Documentation/kbuild/modules.rst`, `Documentation/admin-guide/module-signing.rst`

---

## 8. Linux Capabilities

*Interpreted as **Linux (POSIX) capabilities** (`CAP_*`). **⚠️ Verify**: the raw notes said "kernel capabilities and where to find them". If that meant kernel features/config options, see `/boot/config-$(uname -r)` (§1).*

> **Remember**
>
> - **Capabilities** split root's power into **41** independent privileges (`CAP_*`, bits 0–40). The kernel checks the **specific** capability an operation needs, not "is UID 0?".
> - Each thread has **5 sets**. **Effective** is what the kernel checks. **Permitted** is what the thread may raise. **Bounding** is a ceiling that can never be exceeded.
> - **File capabilities** give a binary one privilege without setuid root (e.g. `ping`: `cap_net_raw=ep`).
> - In kernel code: `capable(CAP_X)` / `ns_capable()`, and return **`-EPERM`** if the check fails.
> - Loading modules needs **`CAP_SYS_MODULE`**, which is effectively root. So is `CAP_SYS_ADMIN`.

### The five capability sets

| Set | `/proc/<pid>/status` | Meaning |
| --- | -------------------- | ------- |
| **Effective** | `CapEff` | What the kernel actually checks right now |
| **Permitted** | `CapPrm` | Upper limit the thread may raise into Effective |
| **Inheritable** | `CapInh` | May be passed across `execve()` (only with matching file capabilities) |
| **Bounding** | `CapBnd` | Hard ceiling: never gained, even by setuid-root programs |
| **Ambient** | `CapAmb` | Kept across `execve()` of non-privileged programs (4.3+) |

| Test box process | `CapEff` | Meaning |
| ---------------- | -------- | ------- |
| Normal shell (`alex`) | `0000000000000000` | No capabilities |
| PID 1 (systemd, root) | `000001ffffffffff` | All 41 |

- Capabilities are **per user namespace**: root in a container has power only over its own namespace's resources (`ns_capable()`).
- `CAP_SYS_ADMIN` is the overloaded "new root". Prefer a specific capability.

### Capabilities met so far

| Capability | # | Grants | Section |
| ---------- | - | ------ | ------- |
| `CAP_SYS_MODULE` | 16 | Load/unload modules | §7 |
| `CAP_SYSLOG` | 34 | Real kernel addresses when `kptr_restrict=1`; `dmesg` when `dmesg_restrict=1` | §1 |
| `CAP_NET_RAW` | 13 | Raw/packet sockets (`ping`) | |
| `CAP_NET_ADMIN` | 12 | Network configuration | |
| `CAP_NET_BIND_SERVICE` | 10 | Bind ports < 1024 | |
| `CAP_SYS_ADMIN` | 21 | Catch-all: mount, many ioctls, … | |
| `CAP_SYS_PTRACE` | 19 | `ptrace` any process | |
| `CAP_PERFMON` / `CAP_BPF` | 38 / 39 | perf / BPF (split out of `CAP_SYS_ADMIN` in 5.8) | |
| `CAP_CHECKPOINT_RESTORE` | 40 | CRIU; the last one (`CAP_LAST_CAP`) | |

### Key APIs / structures (kernel side)

| API | Header | Purpose | Context |
| --- | ------ | ------- | ------- |
| `capable(CAP_X)` | `<linux/capability.h>` | Does `current` have `CAP_X` in the **initial** user namespace? (runs LSM hooks) | Process context |
| `ns_capable(ns, CAP_X)` | `<linux/capability.h>` | Same, relative to user namespace `ns` | Process context |
| `file_ns_capable(file, ns, CAP_X)` | `<linux/capability.h>` | Check the credentials of whoever **opened** `file` | Process context |
| `has_capability(task, CAP_X)` | `<linux/capability.h>` | Check another task, without auditing | Process context |
| `struct cred` | `<linux/cred.h>` | Where the sets live (`current_cred()`) | Any (RCU) |

### Code example

Typical permission check in a driver's `ioctl` handler (fragment):

```c
#include <linux/capability.h>	/* capable(), CAP_* constants */
#include <linux/fs.h>		/* struct file */
static long demo_ioctl(struct file *file, unsigned int cmd, unsigned long arg) /* ioctl entry point */
{							/* start of demo_ioctl() */
	if (!capable(CAP_SYS_ADMIN))			/* privileged operation: require CAP_SYS_ADMIN (initial userns) */
		return -EPERM;				/* caller lacks it: "Operation not permitted" */
	/* ... privileged work goes here ... */	/* only reached by sufficiently privileged callers */
	return 0;					/* success */
}							/* end of demo_ioctl() */
```

Check the **narrowest** capability that fits (e.g. `CAP_NET_ADMIN` for network settings). Return `-EPERM` for a missing capability; `-EACCES` is for file permission bits.

### Commands / debugging

```sh
grep ^Cap /proc/$$/status                 # the five sets of the current shell (hex bitmasks)
capsh --decode=000001ffffffffff           # turn a hex mask into capability names
capsh --print                             # current process's capabilities, human-readable
cat /proc/sys/kernel/cap_last_cap         # highest capability number this kernel knows (40)
getcap /usr/bin/ping                      # file capabilities of a binary
getpcaps <pid>                            # capabilities of another process
sudo setcap cap_net_bind_service=ep ./srv # give a binary one capability (ask first on the test box)
```

### Pitfalls

- Checking `uid == 0` in kernel code instead of `capable()`: this breaks with namespaces and bypasses LSMs.
- `capable()` vs `ns_capable()` mix-ups: a container's root could get host-wide power.
- Checking at `read`/`write` time instead of at `open`: a privileged process can be tricked into using an unprivileged fd. Use `file_ns_capable()`.
- `CAP_SYS_MODULE`, `CAP_SYS_ADMIN`, `CAP_SYS_PTRACE` and `CAP_DAC_OVERRIDE` are **root-equivalent**. Granting them is not least privilege.

### Revision questions

1. What is the difference between the Permitted, Effective and Bounding sets?
2. Why can `ping` send raw ICMP packets without being setuid root on Ubuntu?
3. Which capability does `insmod` need, and why is it effectively root?

<details>
<summary>Answers</summary>

1. Effective is what is checked now. Permitted is what may be raised into Effective. Bounding is the ceiling no `exec` can exceed.
2. The binary has the file capability `cap_net_raw=ep`, so it gains only `CAP_NET_RAW` at exec.
3. `CAP_SYS_MODULE`. A module runs arbitrary code in kernel mode, so it can grant itself anything.

</details>

### Source pointers

- `include/uapi/linux/capability.h` (`CAP_*` numbers), `kernel/capability.c`, `security/commoncap.c`
- `include/linux/cred.h`, `man 7 capabilities`, `man 7 user_namespaces`

---

## 9. Kernel Architecture: Monolithic vs Microkernel

> **Remember**
>
> - Linux is a **monolithic kernel**: system calls, the scheduler, memory management, filesystems, networking and drivers all run in **one address space** (kernel space, the high half: `0xffff_8…` on x86_64), in kernel mode, and call each other as **ordinary functions**.
> - A **microkernel** keeps only the minimum (IPC, scheduling, basic memory management) in kernel mode. Filesystems and drivers run as **user-space servers** that talk by **message passing**.
> - Monolithic = **fast** (a function call, no IPC or context switch) but **no isolation**: one bad driver or module can corrupt or crash the whole kernel.
> - Linux is **monolithic but modular**: modules (§7) are loaded into the same single address space, with the same full privileges.

### Overview

The kernel architecture decides where OS services run and how they communicate. Linux keeps everything in one privileged address space for performance. This is why a module bug is a kernel bug, and why kernel code must never trust or directly dereference user pointers (§3).

### How it works

```text
        MONOLITHIC (Linux)                       MICROKERNEL (QNX, seL4, MINIX 3)
 ┌──────────────────────────────┐         ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐
 │ user space: apps, libc       │         │ app  │ │ FS   │ │ net  │ │driver│  user space
 │ 0x0000… – 0x0000_7fff_ffff…  │         │      │ │server│ │server│ │server│  (servers)
 ├──────── syscall ─────────────┤         └──┬───┘ └──┬───┘ └──┬───┘ └──┬───┘
 │ kernel space: 0xffff_8000_…  │            └── IPC messages ─┴────────┘
 │  syscalls  sched  mm  VFS/FS │         ┌──────────────────────────────────┐
 │  net stack  drivers  modules │         │ microkernel: IPC, sched, basic mm│  kernel mode
 │  (all direct function calls) │         └──────────────────────────────────┘
 └──────────────────────────────┘
```

| Design | Examples | In kernel mode | Communication | Trade-off |
| ------ | -------- | -------------- | ------------- | --------- |
| **Monolithic** | Linux, FreeBSD | Everything | Direct function calls | Fast; one bug can take down everything |
| **Microkernel** | QNX, seL4, MINIX 3, L4 | IPC, scheduling, basic mm | Message passing (IPC) | Isolation and restartable servers; IPC overhead |
| **Hybrid** | Windows NT, macOS XNU | Most services, microkernel-style structure | Mostly direct calls | A compromise; in practice close to monolithic |

- On x86_64 with 4-level paging, user space is `0x0000_0000_0000_0000`–`0x0000_7fff_ffff_ffff` and kernel space starts at `0xffff_8000_0000_0000` (§3).
- Each half is **2^47 bytes = 128 TiB**. *Raw notes said user space is O(2^27); correct is 2^47 (2^27 would be only 128 MiB).*
- Everything between the two halves (the **non-canonical hole**, almost all of the 2^64 range) is **unaddressable**: any access raises a general-protection fault (#GP). The CPU implements only 48 virtual-address bits, and bits 63–48 must copy bit 47. With 5-level paging (`la57`), there are 57 bits and each half grows to 2^56 = 64 PiB.
- Linux still moves *some* work to user space where it helps: FUSE filesystems, UIO/VFIO user-space drivers, and eBPF programs (verified, sandboxed code run *in* the kernel).
- Historical note: the 1992 **Tanenbaum–Torvalds debate** (MINIX microkernel vs Linux monolithic).

### Commands / debugging

```bash
sudo grep -c ' [tT] ' /proc/kallsyms          # count kernel text (function) symbols: all subsystems share one symbol table
sudo grep -w -e vfs_read -e tcp_sendmsg -e schedule /proc/kallsyms  # FS, network and scheduler functions side by side in kernel space
cat /proc/filesystems                          # filesystems the kernel supports, all inside the kernel itself
lsmod | head                                   # modules loaded into the same kernel address space
```

### Pitfalls

- "Modular" does not mean "isolated": a loaded module has the same privileges as the rest of the kernel. A NULL dereference in a module can oops the whole kernel.
- Microkernel does not mean "small Linux". It is a different design with different trade-offs, not just a Linux with fewer drivers.

### Revision questions

1. What makes Linux a monolithic kernel, and why is that fast?
2. Why does a bug in a loaded module crash the whole system, when a bug in a microkernel's filesystem server might not?
3. Give one way Linux runs driver or filesystem code in user space.

<details>
<summary>Answers</summary>

1. All kernel services share one privileged address space and call each other directly, so there is no IPC or context switch between subsystems.
2. The module runs in the same kernel address space with full privileges, so it can corrupt any kernel data. A microkernel server is a separate user-space process that can be killed and restarted.
3. FUSE (filesystems) or UIO/VFIO (drivers).

</details>

### Source pointers

- `init/main.c` (`start_kernel()` sets up every subsystem in one image), `kernel/`, `mm/`, `fs/`, `net/`, `drivers/`
- `Documentation/filesystems/fuse.rst`, `Documentation/driver-api/uio-howto.rst`

---

## 10. Tracing: ftrace, kprobes, `trace_marker` and ptrace

> **Remember**
>
> - **ftrace** is the kernel's built-in tracer. It writes events into a per-CPU **ring buffer** and is controlled through **tracefs** at `/sys/kernel/tracing`. Pick a tracer by writing to `current_tracer`: `function` (every kernel function call) or `function_graph` (entry + exit, call tree, durations).
> - A **kprobe** dynamically instruments (almost) **any kernel instruction** at run time, with no recompile or reboot. It works by patching in a breakpoint (`int3` on x86, `BRK` on ARM64), or a jump when optimised. A **kretprobe** fires on function **return**.
> - kprobes can be used three ways: from a **module** (`register_kprobe()`, GPL-only), from **tracefs** (`kprobe_events`, no code), or from **eBPF** (`bpftrace -e 'kprobe:…'`).
> - kprobe handlers run in **atomic context**: they must not sleep and must be fast.
> - **`trace_marker`** lets **user space** write text into the same ftrace ring buffer, so app events get **accurate kernel timestamps** and appear interleaved with kernel events on one timeline. Android's **atrace** (`ATRACE_BEGIN/END`, used by systrace/Perfetto) is built on it: user-mode events piggybacking on kernel tracing.
> - **`ptrace()`** is the system call one process uses to trace/debug another (stop it, read/write its memory and registers, stop at each syscall). **gdb** and **strace** are built on it. It is a *different mechanism* from ftrace: per-process, stop-based and slow.

### Overview

Tracing answers "what is the kernel actually doing, and when?" without a debugger stopping the system. Static **tracepoints** are fixed hooks compiled into the source. **kprobes** add dynamic hooks wherever you need them. `trace_marker` joins the user-space view to the kernel timeline, so you can correlate "the app started drawing a frame" with "the scheduler preempted it".

### ftrace tracers

ftrace has two parts: **tracers** (one active at a time, chosen via `current_tracer`) and **events** (tracepoints, kprobe events, markers: enabled independently).

| Tracer | What it records | Config (all `=y` on the test box) |
| ------ | --------------- | ------ |
| `nop` | Nothing (default); events still work | n/a |
| `function` | Every kernel function entry, with its caller | `CONFIG_FUNCTION_TRACER` |
| `function_graph` | Entry **and** exit: an indented call tree with per-function duration | `CONFIG_FUNCTION_GRAPH_TRACER` |
| `wakeup`, `wakeup_rt` | Worst-case wake-up latency | `CONFIG_SCHED_TRACER` |
| `irqsoff`, `preemptoff` | Longest time with IRQs / preemption disabled | `CONFIG_IRQSOFF_TRACER`, `CONFIG_PREEMPT_TRACER` |

- **How `function` tracing is nearly free when off:** the compiler inserts a call to `__fentry__` at the start of every function (`-pg -mfentry`). With `CONFIG_DYNAMIC_FTRACE`, the kernel patches these into **NOPs** at boot and only patches the ones you select back into calls.
- **Always filter.** Tracing every function produces millions of lines per second. Use `set_ftrace_filter` (functions to trace), `set_graph_function` (roots for `function_graph`) and `set_ftrace_pid`.
- `trace-cmd` is the command-line front end; KernelShark visualises its output.

```bash
cd /sys/kernel/tracing                          # tracefs control directory (root; ask first on the test box)
cat available_tracers                           # tracers built into this kernel
echo do_sys_openat2 > set_graph_function        # graph only calls made beneath do_sys_openat2
echo function_graph > current_tracer            # select the function_graph tracer
echo 1 > tracing_on                             # start recording
cat /etc/hostname > /dev/null                   # do something that opens a file
echo 0 > tracing_on                             # stop recording
head -40 trace                                  # view the call tree with durations
echo nop > current_tracer                       # switch tracing off again
echo > set_graph_function                       # clear the filter
sudo trace-cmd record -p function_graph -g do_sys_openat2 cat /etc/hostname   # same thing via trace-cmd
sudo trace-cmd report | head -40                                              # print the recorded trace
```

Example `function_graph` output (shape):

```text
 1)               |  do_sys_openat2() {
 1)               |    getname() {
 1)   0.912 us    |      kmem_cache_alloc();
 1)   1.803 us    |    }
 1) + 12.345 us   |  }
```

### How it works: kprobes

```text
 register_kprobe(&kp)                        CPU executes probed address
        │                                              │
        ▼                                              ▼
 save original instruction            int3 trap ─> kprobe handler dispatch
 write int3 (0xCC) over it                         │
 (or a jmp, if optimised: OPTPROBES)               ├─ pre_handler(p, regs)
                                                   ├─ single-step the saved original
                                                   │   instruction (out of line)
                                                   ├─ post_handler (optional)
                                                   └─ resume after the probe
 kretprobe: at entry, the return address is replaced with a trampoline
            → the handler runs when the function returns (return value in regs)
```

- **Blacklist:** code the kprobe machinery itself uses cannot be probed (functions marked `NOKPROBE_SYMBOL()`, `__kprobes`, parts of entry code). List them with `/sys/kernel/debug/kprobes/blacklist`.
- **Inlined or `static` functions** may have no symbol of their own, so probe by address + offset or pick a caller. Check that the symbol exists with `grep -w <sym> /proc/kallsyms`.
- **Cost:** an `int3` probe costs a trap per hit (~µs). An **optimised** probe (`CONFIG_OPTPROBES`, `debug.kprobes-optimization = 1`) uses a jump and is much cheaper. A probe on a function entry that has an ftrace `fentry` site uses ftrace instead (`CONFIG_KPROBES_ON_FTRACE`).
- **Test box:** `CONFIG_KPROBES`, `KRETPROBES`, `OPTPROBES`, `KPROBES_ON_FTRACE`, `KPROBE_EVENTS`, `UPROBE_EVENTS` are all `=y`; tracefs is mounted at `/sys/kernel/tracing`.

### uprobes: kprobes for user space (Linux 3.5+)

**uprobes** bring the kprobe idea to **user-space code**. Since Linux 3.5, you can probe any instruction in a user binary or shared library by **file + offset**, usually given as a user **symbol** (e.g. `readline` in `/bin/bash`, `malloc` in libc). A **uretprobe** fires on return.

- **Mechanism:** the kernel places a breakpoint (`int3`) in the **page cache page** of the file at that offset, copy-on-write per process. So **every process** that maps the file hits the probe, including processes started later, unless you filter by PID. The trap enters the kernel, which runs the handler (a trace event, BPF program or perf) and then single-steps the original instruction out of line (XOL area).
- **No ptrace, no recompile:** the target is not stopped and there is no tracer process. Compared with `ltrace`, it is far cheaper, but each hit still costs a user→kernel trap (~1–3 µs).
- **Symbols:** they need the binary's symbol table (or debuginfo). Stripped binaries can still be probed by raw offset.
- **USDT** (user statically defined tracing, e.g. `DTRACE_PROBE` in glibc, Python, PostgreSQL) are static NOP markers in user code, activated through uprobes: the user-space analogue of tracepoints.
- Config: `CONFIG_UPROBES`, `CONFIG_UPROBE_EVENTS` (`=y` on the test box).

```bash
sudo bpftrace -e 'uprobe:/bin/bash:readline { printf("readline by pid %d\n", pid); }'           # fire on every bash readline() call
sudo bpftrace -e 'uretprobe:/bin/bash:readline { printf("%s\n", str(retval)); }'                # print each line typed into any bash
sudo bpftrace -e 'uprobe:/lib/x86_64-linux-gnu/libc.so.6:malloc /pid == 1234/ { @[arg0] = count(); }'  # histogram of malloc sizes for one PID
sudo perf probe -x /bin/bash readline                                                           # create a uprobe event via perf
echo 'p:bashrl /bin/bash:0x<offset>' | sudo tee -a /sys/kernel/tracing/uprobe_events            # raw tracefs form: file + offset (offset from nm/objdump)
```

### Worked example: tracing outbound TCP connections with bpftrace

Class demo: attach a kprobe to `tcp_v4_connect()` and print who opens each IPv4 TCP connection, and to which address. Run it in one terminal, then run `curl` in another.

```bash
sudo bpftrace -e 'kprobe:tcp_v4_connect {                   /* fire on entry to tcp_v4_connect() */
	$s = (struct sockaddr_in *)arg1;                     /* arg1 = 2nd argument (uaddr), cast to IPv4 sockaddr */
	printf("%s %d %s %s\n", username, pid, comm,        /* user name, PID, process name ... */
	       ntop($s->sin_addr.s_addr));                   /* ... and destination IPv4 address as text */
}'
curl -4 https://example.com                                  # in a second terminal: -4 forces IPv4 so the probe fires
```

Find probe points before writing a script:

```bash
sudo bpftrace -l 'kprobe:tcp*'                               # list every kprobe-able kernel function starting with "tcp"
sudo bpftrace -l 'tracepoint:sock:*'                         # list the (stable) socket tracepoints
sudo bpftrace -lv 'tracepoint:sock:inet_sock_set_state'      # -v also shows the tracepoint's argument fields
```


Example output (illustrative):

```text
alex 4242 curl 93.184.215.14
```

- **Signature:** `int tcp_v4_connect(struct sock *sk, struct sockaddr *uaddr, int addr_len)` in `net/ipv4/tcp_ipv4.c`. In bpftrace, `arg0`, `arg1`, … are the probed function's arguments (read from `pt_regs`: `rdi`, `rsi`, … on x86_64; `x0`, `x1`, … on ARM64), so `arg1` is `uaddr`.
- **Why the cast works:** bpftrace reads kernel type definitions from **BTF** (`/sys/kernel/btf/vmlinux`, present on the test box), so `struct sockaddr_in` resolves with no headers. On kernels without BTF you need `#include <linux/in.h>` in the script.
- **Builtins used:** `username` (user name from the UID), `pid` (really the TGID), `comm` (task name, 16 bytes), and `ntop()` (formats an IP address as a string).
- **IPv6:** `tcp_v4_connect` only sees IPv4. If `example.com` resolves to IPv6, `curl` uses `tcp_v6_connect()` and nothing prints. Use `curl -4`, or also probe `kprobe:tcp_v6_connect` and cast `arg1` to `struct sockaddr_in6 *`.
- **Scope:** this fires on the `connect()` path only (outbound, before the handshake completes). Accepted (inbound) connections go through `inet_csk_accept()`. bcc's `tcpconnect` tool does the same job with more polish (`sudo tcpconnect-bpfcc` on Ubuntu).
- **Stable alternative:** the `sock:inet_sock_set_state` tracepoint (`tracepoint:sock:inet_sock_set_state`) sees TCP state changes for IPv4 and IPv6 without depending on internal function names.
- ⚠️ Not run on the test box: it needs root. The ingredients are present: bpftrace v0.20.2, BTF, and the `tcp_v4_connect` symbol in `/proc/kallsyms`.

### How it works: `trace_marker` and Android atrace

```text
 app / framework                          kernel
 ATRACE_BEGIN("draw")  ─ write() ─>  /sys/kernel/tracing/trace_marker
   "B|1234|draw"                            │
 ATRACE_END()          ─ write() ─>         ▼
   "E|1234"                        ftrace ring buffer  <── sched_switch, irq, kprobe events …
                                            │
                                    atrace / Perfetto  ──> one timeline (UI: ui.perfetto.dev)
```

- Anything written to `trace_marker` appears in the trace as a `tracing_mark_write:` event with the writer's PID and a timestamp.
- Android atrace text format: `B|<pid>|<name>` begins a slice, `E|<pid>` ends it, and `C|<pid>|<name>|<value>` records a counter. **atrace** enables *categories* (`gfx`, `view`, `sched`, `freq`, …) and collects the buffer. **Perfetto** has replaced systrace as the recording/viewing tool.
- `trace_marker_raw` accepts binary records instead of text.
- **Why go through the kernel?** Timestamps come from the same trace clock as scheduler, IRQ and kprobe events, so user and kernel events line up accurately on one timeline. atrace is user-mode instrumentation piggybacking on kernel tracing, not a separate tracer.

### ptrace and strace

**`ptrace()`** is the system call behind debuggers and `strace`. The **tracer** attaches to a **tracee**. The kernel then stops the tracee at chosen points and wakes the tracer (via `waitpid()`), which can inspect and modify the tracee before letting it continue.

```text
 strace (tracer)                       kernel                       traced process (tracee)
 ptrace(PTRACE_SEIZE, pid) ──────────> attach                        running …
 ptrace(PTRACE_SYSCALL)    ──────────> resume, stop at next syscall  openat(...) ──┐
 waitpid()  <────────────── syscall-entry stop  <────────────────────────────────┘
 PTRACE_GET_SYSCALL_INFO: read nr + args, print "openat(AT_FDCWD, "/etc/hostname", …"
 ptrace(PTRACE_SYSCALL)    ──────────> run syscall, stop at exit
 waitpid()  <────────────── syscall-exit stop: print " = 3"
             … two stops and four context switches per system call …
```

| Request | Purpose |
| ------- | ------- |
| `PTRACE_TRACEME` | Child asks to be traced by its parent (how `strace cmd` / `gdb cmd` start) |
| `PTRACE_ATTACH` / `PTRACE_SEIZE` | Attach to a running process (`SEIZE` does not stop it; preferred) |
| `PTRACE_SYSCALL` | Continue, stopping at the next syscall entry/exit (strace) |
| `PTRACE_PEEKDATA` / `POKEDATA`, `GETREGS` / `SETREGS` | Read/write tracee memory and registers (gdb breakpoints) |
| `PTRACE_CONT` / `PTRACE_DETACH` | Resume / detach |

- **Permissions:** you can trace your own processes (same UID, no setuid). **Yama** `kernel.yama.ptrace_scope` restricts this further: 0 = classic, **1 = only your descendants** (Ubuntu default; test box = 1), 2 = only with `CAP_SYS_PTRACE`, 3 = no ptrace at all. `CAP_SYS_PTRACE` overrides (§8).
- **Overhead:** each syscall stops the tracee twice, so `strace` can slow syscall-heavy programs by 10–100×. For low overhead, use `perf trace` or `bpftrace` (in-kernel, no stops).
- A process can have only **one** tracer, so you cannot `strace` a process that `gdb` is already attached to. `TracerPid:` in `/proc/<pid>/status` shows who is tracing it.
- `ltrace` traces **library** calls (via breakpoints on PLT entries, again using ptrace).

```bash
strace -f -e trace=openat,read -o out.txt ls    # follow children, only openat/read, write to out.txt
strace -c ls > /dev/null                        # summary: count and time per syscall
strace -T -tt -p <pid>                          # attach to a running process (needs ptrace permission), show time per call
cat /proc/sys/kernel/yama/ptrace_scope          # current Yama ptrace restriction level
grep TracerPid /proc/<pid>/status               # PID of the process tracing <pid> (0 = none)
sudo perf trace -s ls                           # strace-like syscall summary without ptrace stops
```

### Cross-memory attach: `process_vm_readv()` / `process_vm_writev()`

**Cross-memory attach (CMA)** (Linux 3.2+, `CONFIG_CROSS_MEMORY_ATTACH`, on by default) lets a process copy data **directly between its own memory and another process's memory** in one system call. The kernel copies straight from one address space to the other, so the data is copied only once. Nothing is mapped, and the target process does not need to stop. *Raw notes said "xma" and `process_vm_ready`; correct names are CMA and `process_vm_readv`.*

```c
ssize_t process_vm_readv(pid_t pid,	/* target process (TGID) */
	const struct iovec *local_iov, unsigned long liovcnt,	/* where to put the data, in our memory */
	const struct iovec *remote_iov, unsigned long riovcnt,	/* where to read from, in the target's memory */
	unsigned long flags);	/* must be 0 */
/* process_vm_writev() has the same arguments and copies the other way */
```

| Method | Copies | Target must stop? | Calls needed |
| ------ | ------ | ----------------- | ------------ |
| `PTRACE_PEEKDATA` / `POKEDATA` | One word (8 bytes) per call | Yes (ptrace-stopped) | One per word: very slow |
| `/proc/<pid>/mem` + `pread()` / `pwrite()` | Any size, one region per call | No | `open()` plus one per region |
| Pipe / socket / shared memory | Twice (in and out of a kernel buffer), or needs both sides to set up a mapping | No, but the target must cooperate | Several |
| **`process_vm_readv()` / `writev()`** | **Once**, many scattered regions per call (`iovec`) | **No** | **One** |

- **Why it is preferred:** single copy, scatter/gather in one call, no cooperation needed from the target. MPI libraries (Open MPI, MPICH) use it for large intra-node messages, and debuggers and profilers use it to read a target's memory quickly.
- **Permission:** the same check as `ptrace` attach (`PTRACE_MODE_ATTACH_REALCREDS`): same user and not setuid, or `CAP_SYS_PTRACE`. **Yama** `ptrace_scope` applies too, so with the Ubuntu default of 1 you can only read your own descendants unless you have `CAP_SYS_PTRACE`.
- **Not atomic:** the target keeps running and may change the data mid-copy. A short count is returned if a remote page is unmapped (`EFAULT` only if nothing was copied). Errors: `ESRCH` (no such process), `EPERM` (not allowed).
- Kernel source: `mm/process_vm_access.c` (it pins the remote pages with `pin_user_pages_remote()` and copies with `copy_page_to_iter()` / `copy_page_from_iter()`).

```bash
grep CONFIG_CROSS_MEMORY_ATTACH /boot/config-$(uname -r)   # is CMA built in? (=y on Ubuntu)
man 2 process_vm_readv                                     # full API and error codes
strace -e trace=process_vm_readv gdb -p <pid> -batch       # see a debugger use it (needs ptrace permission)
```

### Key APIs / structures

| API | Header | Purpose | Context |
| --- | ------ | ------- | ------- |
| `struct kprobe` | `<linux/kprobes.h>` | `.symbol_name` / `.addr` / `.offset`, `.pre_handler`, `.post_handler` | n/a |
| `register_kprobe()` / `unregister_kprobe()` | `<linux/kprobes.h>` | Plant / remove a probe (`EXPORT_SYMBOL_GPL`) | Process; may sleep |
| `struct kretprobe`, `register_kretprobe()` | `<linux/kprobes.h>` | Handler on function return; `regs_return_value(regs)` | Process; may sleep |
| kprobe `pre_handler` | n/a | Your code at the probe point | **Atomic**: no sleep, preemption disabled |
| `trace_printk()` | `<linux/kernel.h>` | Fast debug print into the ftrace buffer (debug only; prints a warning banner at boot/load) | Any context |

### Code example

Minimal kprobe module (excerpt). Full file and `Makefile`: [`examples/kprobe_demo/`](examples/kprobe_demo/). **Built on 6.8 x86_64** (not loaded).

```c
static int handler_pre(struct kprobe *p, struct pt_regs *regs)	/* called just before the probed instruction runs */
{									/* start of handler_pre(): atomic context, must not sleep */
	pr_info_ratelimited("kprobe_demo: %s hit by %s (pid %d)\n",	/* rate-limited so a busy probe cannot flood the log */
			    p->symbol_name, current->comm,		/* probed symbol name and the calling task's name */
			    task_pid_nr(current));			/* calling task's PID */
	return 0;							/* 0 = continue and execute the probed instruction normally */
}									/* end of handler_pre() */
static struct kprobe kp = {		/* the probe descriptor, registered in init */
	.pre_handler = handler_pre,	/* run handler_pre() on every hit */
	.symbol_name = "kernel_clone",	/* probe the fork/clone path (the demo takes this from a module parameter) */
};					/* end of kp */
/* in init: ret = register_kprobe(&kp);  in exit: unregister_kprobe(&kp); */
```

### Commands / debugging

kprobe without writing any code, using tracefs (needs root; ask first on the test box):

```bash
cd /sys/kernel/tracing                                          # tracefs control directory
echo 'p:myclone kernel_clone' >> kprobe_events                  # define kprobe event "myclone" at kernel_clone entry
echo 'r:myopen do_sys_openat2 ret=$retval' >> kprobe_events     # define kretprobe event recording the return value
echo 1 > events/kprobes/enable                                  # enable all kprobe events
cat trace_pipe                                                  # stream events live (Ctrl-C to stop)
echo 0 > events/kprobes/enable                                  # disable the events again
echo > kprobe_events                                            # delete all dynamic kprobe events
```

Same idea with eBPF, plus `trace_marker`:

```bash
sudo bpftrace -e 'kprobe:kernel_clone { printf("%s %d\n", comm, pid); }'        # print every fork/clone caller
sudo bpftrace -e 'kretprobe:do_sys_openat2 { @ret[retval < 0] = count(); }'     # count failed vs successful opens
echo "hello from user space" | sudo tee /sys/kernel/tracing/trace_marker        # write a marker into the ftrace buffer
sudo cat /sys/kernel/tracing/trace | grep tracing_mark_write                    # find it in the trace
sudo trace-cmd record -e sched_switch -e ftrace:print sleep 1                   # record scheduler events plus markers
sudo cat /sys/kernel/debug/kprobes/list                                         # probes currently registered ([OPTIMIZED], [FTRACE] flags)
```

### Pitfalls

- **Sleeping in a handler** (`kmalloc(GFP_KERNEL)`, `mutex_lock()`, `copy_from_user()`): "scheduling while atomic" or a deadlock.
- **Forgetting `unregister_kprobe()`** in `module_exit`: the breakpoint stays and jumps into freed module memory, so the next hit crashes the kernel.
- **Probing a hot path** (`schedule`, `kmalloc`) with `pr_info()` floods the log and slows the machine. Use rate-limiting, counters, or bpftrace maps.
- **Relying on function names/arguments:** kprobes attach to internal, unstable functions. They can be renamed, inlined or change signature between kernel versions (e.g. `_do_fork` became `kernel_clone` in 5.10). Prefer stable **tracepoints** where one exists.
- Leaving `kprobe_events` defined after an experiment: clear them with `echo > kprobe_events`.

### Revision questions

1. How does a kprobe get control at the probed address on x86, and what makes an "optimised" kprobe cheaper?
2. What restrictions apply to code in a kprobe `pre_handler`, and why?
3. How does Android atrace get app events onto the same timeline as scheduler events?
4. Why might a kprobe-based tool break after a kernel upgrade, and what is the more stable alternative?
5. What is the difference between the `function` and `function_graph` tracers, and why is function tracing cheap when disabled?
6. Why is `strace` slow, and why can `strace -p` fail on Ubuntu even for your own process?
7. How is a uprobe placed, and why does probing `malloc` in libc affect every process unless you filter?

<details>
<summary>Answers</summary>

1. The first byte of the instruction is replaced with `int3`. The trap handler runs `pre_handler`, single-steps the saved original instruction, then resumes. An optimised probe replaces the instruction with a `jmp` to a detour buffer, which avoids the trap.
2. It runs in atomic context (from a trap, with preemption disabled), so it must not sleep or take sleeping locks, and it should be short.
3. The framework writes `B|pid|name` / `E|pid` strings to `/sys/kernel/tracing/trace_marker`. These land in the ftrace ring buffer alongside kernel events, with the same clock.
4. kprobes hook internal functions, which can be renamed, inlined or change arguments. Static tracepoints (and `raw_tp` in BPF) are the more stable interface.
5. `function` records each function entry; `function_graph` also hooks the exit, giving a call tree with durations. With dynamic ftrace, the `__fentry__` call sites are patched to NOPs until tracing is enabled.
6. ptrace stops the tracee at every syscall entry and exit, costing context switches. Yama `ptrace_scope = 1` only allows tracing your own descendants, so attaching to an unrelated process needs `sudo` / `CAP_SYS_PTRACE`.
7. The kernel writes a breakpoint into the file's page-cache page at the symbol's offset. Every process mapping libc shares that (inode, offset), so all of them trap into the handler.

</details>

### Source pointers

- `kernel/kprobes.c`, `arch/x86/kernel/kprobes/` (`core.c`, `opt.c`), `arch/arm64/kernel/probes/`
- `kernel/trace/trace_kprobe.c`, `kernel/trace/trace.c` (`tracing_mark_write()`), `kernel/trace/ftrace.c`, `kernel/trace/trace_functions_graph.c`
- `kernel/events/uprobes.c`, `arch/x86/kernel/uprobes.c`, `kernel/trace/trace_uprobe.c`, `Documentation/trace/uprobetracer.rst`
- `kernel/ptrace.c`, `arch/x86/kernel/ptrace.c`, `security/yama/yama_lsm.c`
- `samples/kprobes/kprobe_example.c`, `samples/kprobes/kretprobe_example.c`
- `Documentation/trace/kprobes.rst`, `Documentation/trace/kprobetrace.rst`, `Documentation/trace/ftrace.rst`, `Documentation/admin-guide/LSM/Yama.rst`, `man 2 ptrace`

---

## 11. Pages and Page Size

### Overview

A **page** is the smallest unit of memory the **MMU** maps: every virtual-to-physical translation, permission bit and page fault works on whole pages. The kernel's memory management (page tables, page cache, `struct page`, allocation) is built on it. The page size is 4 KiB on x86_64 but configurable on ARM64 (4/16/64 KiB), and that choice affects performance, memory use and even user-space ABI.

### Key concepts

- **Page** (virtual) vs **page frame** (the physical page it maps to). A **PFN** (page frame number) is `phys_addr >> PAGE_SHIFT`.
- `PAGE_SIZE` = 4096 by default; `PAGE_SHIFT` = log2(`PAGE_SIZE`) = 12. `PAGE_SIZE` is defined as `1UL << PAGE_SHIFT`, so the shift is the fundamental constant.
- An address splits into **page number** (upper bits) and **offset in page** (low `PAGE_SHIFT` bits). `PAGE_MASK` = `~(PAGE_SIZE - 1)` clears the offset.
- Memory is still **byte-addressable**; the page is the granularity of *mapping and protection*, not of addressing. *Raw notes said "minimal unit that can be addressed"; more precisely it is the minimal unit the MMU can map.*
- The kernel keeps one `struct page` (64 bytes) per physical page frame: about 1.6% of RAM with 4 KiB pages, less with larger pages.
- **Huge pages** map a larger block at a higher page-table level (x86_64: 2 MiB and 1 GiB) via hugetlbfs or **THP** (Transparent Huge Pages).
- Page size is fixed at **kernel build time** (ARM64 `CONFIG_ARM64_4K_PAGES` / `_16K_PAGES` / `_64K_PAGES`); a running kernel cannot switch it.

### How it works: splitting a virtual address

```text
x86_64, 4 KiB pages, 4-level paging (48-bit VA)
 47      39 38      30 29      21 20      12 11           0
+----------+----------+----------+----------+--------------+
| PGD idx  | PUD idx  | PMD idx  | PTE idx  | page offset  |
|  9 bits  |  9 bits  |  9 bits  |  9 bits  |   12 bits    |
+----------+----------+----------+----------+--------------+
   each table = 512 entries x 8 bytes = exactly one 4 KiB page
   stop at PMD -> 2 MiB huge page; stop at PUD -> 1 GiB huge page

ARM64, 16 KiB pages (TCR_EL1.TGx = 16K granule), 47-bit VA, 3 levels
 46  36 35        25 24        14 13               0
+------+------------+------------+------------------+
| L1   |  L2 idx    |  L3 idx    |   page offset    |
|11 bit|  11 bits   |  11 bits   |     14 bits      |
+------+------------+------------+------------------+
   each table = 2048 entries x 8 bytes = one 16 KiB page
```

Bigger pages give a bigger offset field, more entries per table, and so **fewer levels** for the same VA size, meaning shorter page walks.

### Page sizes by architecture

| Architecture | Base page | Selected by | Huge/block sizes |
| ------------ | --------- | ----------- | ---------------- |
| x86_64 | 4 KiB only | fixed by the architecture | 2 MiB (PMD), 1 GiB (PUD) |
| ARM64, 4K granule | 4 KiB | `TCR_EL1.TG0/TG1` + `CONFIG_ARM64_4K_PAGES` | 64 KiB (contiguous), 2 MiB, 1 GiB |
| ARM64, 16K granule | 16 KiB | `CONFIG_ARM64_16K_PAGES` | 2 MiB (contiguous), 32 MiB |
| ARM64, 64K granule | 64 KiB | `CONFIG_ARM64_64K_PAGES` | 2 MiB (contiguous), 512 MiB |

- **ARM64 control register:** the translation granule is set in `TCR_EL1`: field `TG0` for the lower (user, `TTBR0_EL1`) half and `TG1` for the upper (kernel, `TTBR1_EL1`) half. The CPU advertises which granules it supports in `ID_AA64MMFR0_EL1`. *Raw notes said "TCR_EL1/SCR_EL1"; there is no `SCR_EL1`. `SCR_EL3` is the Secure Configuration Register and has nothing to do with page size. The register is `TCR_EL1`.*
- **Apple silicon:** iPhone/iPad (A-series) and M1+ Macs use 16 KiB pages. Asahi Linux therefore runs a 16K-page kernel on M-series Macs. ⚠️ Verify: raw notes said "A12"; Apple moved to 16K pages well before the A12 (iOS on 64-bit devices), so the A12 may have been just an example.
- **Android 15+:** Android 15 is the first release that *supports* 16 KiB-page devices (ARM64 16K kernels); Google Play requires apps targeting Android 15+ to be 16 KiB-compatible (from November 2025). *Raw notes said "16k by default"; the default depends on the device/vendor: 4 KiB is still common, and 16K is opt-in per device (e.g. a developer option on Pixel 8+).* ⚠️ Verify what the instructor meant by "default".
- **Servers / AI workloads (64K):** large-memory workloads such as LLM inference/training, databases and HPC benefit from 64 KiB pages: far fewer TLB misses and page faults over tens or hundreds of GiB. Ubuntu ships an ARM64 64K-page kernel as a separate flavour (`linux-generic-64k`, `sudo apt install linux-generic-64k`, since 22.04); RHEL 9 has `kernel-64k`, and NVIDIA Grace (GH200) systems commonly run 64K kernels. The default Ubuntu ARM64 kernel remains 4K.
- **Raspberry Pi 5:** the note-taking machine runs `6.12.x+rpt-rpi-2712`, a **16K-page** kernel (`getconf PAGESIZE` → `16384`). The test box (x86_64) reports `4096`.

### Why page size matters (trade-offs)

| Larger pages (16K/64K) | Smaller pages (4K) |
| ---------------------- | ------------------ |
| More memory covered per **TLB** entry ("TLB reach") → fewer TLB misses | More TLB misses on large working sets |
| Fewer page-table levels and pages → faster walks, less page-table memory | Deeper walks, more page-table memory |
| Fewer page faults for sequential access; typically a few % to ~10% faster on Android/Apple workloads | Less waste per allocation |
| More **internal fragmentation** (a 1-byte file still uses a whole page in the page cache) → higher memory use | Better for many small files/mappings |
| Breaks software that assumes 4096 (ELF segment alignment, `mmap` offsets, hard-coded constants) | The long-standing default: most software assumes it |

### Key APIs / structures

| API / macro | Header | Purpose | Context |
| ----------- | ------ | ------- | ------- |
| `PAGE_SIZE`, `PAGE_SHIFT`, `PAGE_MASK` | `<asm/page.h>` (via `<linux/mm.h>`) | Page size, log2 of it, mask that clears the offset | Any |
| `PAGE_ALIGN(x)` | `<linux/mm.h>` | Round `x` up to the next page boundary | Any |
| `offset_in_page(p)` | `<linux/mm.h>` | `(unsigned long)p & ~PAGE_MASK` | Any |
| `get_order(size)` | `<asm/page.h>` | Smallest *order* (log2 of page count) that holds `size` bytes | Any |
| `alloc_pages(gfp, order)` / `__free_pages(page, order)` | `<linux/gfp.h>` | Allocate/free 2^order contiguous physical pages (`struct page *`) | Sleeps with `GFP_KERNEL`; `GFP_ATOMIC` in atomic context |
| `__get_free_page(gfp)` / `free_page(addr)` | `<linux/gfp.h>` | Same, returning a kernel virtual address | As above |
| `virt_to_page()`, `page_address()`, `page_to_pfn()`, `pfn_to_page()` | `<linux/mm.h>` | Convert between address, `struct page` and PFN (linear-map addresses only) | Any |
| `struct page` | `<linux/mm_types.h>` | Per-physical-frame metadata (flags, refcount, mapping) | n/a |
| `getconf PAGESIZE`, `sysconf(_SC_PAGESIZE)`, `getpagesize()` | user space | Query page size at run time | n/a |

### Code example

Minimal module that prints the page constants and allocates one page (not built; source only):

```c
// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>	/* module_init/exit, MODULE_* macros */
#include <linux/mm.h>		/* PAGE_SIZE, PAGE_SHIFT, PAGE_MASK, page_address() */
#include <linux/gfp.h>		/* alloc_pages(), __free_pages(), GFP_KERNEL */

static struct page *pg;		/* the page we allocate in init and free in exit */

static int __init pagedemo_init(void)			/* runs at insmod, process context */
{
	pr_info("PAGE_SIZE=%lu PAGE_SHIFT=%d PAGE_MASK=%#lx\n",	/* print the constants */
		PAGE_SIZE, PAGE_SHIFT, PAGE_MASK);		/* 4096 / 12 / 0xfffffffffffff000 on x86_64 */

	pg = alloc_pages(GFP_KERNEL, 0);		/* order 0 = one page; may sleep */
	if (!pg)					/* allocation can fail */
		return -ENOMEM;				/* abort load with "out of memory" */

	pr_info("pfn=%#lx vaddr=%px\n",			/* show frame number and kernel address */
		page_to_pfn(pg), page_address(pg));	/* %px prints the raw pointer (demo only) */
	return 0;					/* success: module stays loaded */
}

static void __exit pagedemo_exit(void)			/* runs at rmmod */
{
	__free_pages(pg, 0);				/* free the order-0 page (else it leaks) */
}

module_init(pagedemo_init);				/* register the init function */
module_exit(pagedemo_exit);				/* register the exit function */
MODULE_LICENSE("GPL");					/* GPL: no taint, GPL-only symbols usable */
MODULE_DESCRIPTION("Print page-size constants and allocate one page");	/* shown by modinfo */
```

### Commands / debugging

```bash
getconf PAGESIZE                                   # base page size: 4096 on the test box, 16384 on the Pi 5
grep -i huge /proc/meminfo                         # Hugepagesize (2048 kB on x86_64), HugePages_*, AnonHugePages (THP)
cat /sys/kernel/mm/transparent_hugepage/enabled    # THP mode: test box shows "always [madvise] never"
grep -E 'KernelPageSize|MMUPageSize' /proc/self/smaps | sort | uniq -c   # page size backing each mapping
grep -E 'CONFIG_ARM64_(4K|16K|64K)_PAGES=' /boot/config-$(uname -r)     # ARM64: which granule the kernel was built with
readelf -lW /bin/ls | grep LOAD                    # "Align" column: 0x1000 = 4K-only ELF, 0x4000+ also runs on 16K
```

### Pitfalls

- **Hard-coding 4096** (in C, `mmap` sizes, buffer maths): breaks on 16K/64K ARM64. Use `PAGE_SIZE` in the kernel and `sysconf(_SC_PAGESIZE)` in user space.
- **ELF alignment:** binaries and `.so` files linked with 4 KiB `LOAD` alignment cannot load on a 16K kernel. Link with `-Wl,-z,max-page-size=16384` (Android NDK r28+ does this by default).
- **Confusing order with size:** `alloc_pages(gfp, 3)` allocates 2^3 = 8 pages (32 KiB on x86_64), not 3.
- **`virt_to_page()` on `vmalloc()` memory** is wrong: vmalloc memory is not in the linear map. Use `vmalloc_to_page()`.
- Allocating with `GFP_KERNEL` in atomic context: it may sleep. Use `GFP_ATOMIC`.

### Corrections to raw notes

- *"TCR_EL1/SCR_EL1"*: the granule is set in `TCR_EL1` (`TG0`/`TG1`). There is no `SCR_EL1`; `SCR_EL3` is unrelated.
- *"minimal physical memory unit that can be addressed"*: memory is byte-addressable; the page is the minimal unit the MMU *maps*.
- *"Android 15+ 16k page size by default"*: Android 15 *adds support* for 16K devices; 4K remains common.
- *"can modify the page size"*: only at kernel build time (Kconfig), not at run time.

### Revision questions

1. If `PAGE_SHIFT` is 14, what are `PAGE_SIZE`, `PAGE_MASK` and the page offset of address `0x12345`?
2. Why can a 16K-page ARM64 kernel cover a 47-bit address space with only 3 page-table levels, whereas x86_64 needs 4 levels for 48 bits?
3. Give two benefits and two costs of moving from 4 KiB to 16 KiB pages.
4. Where is the ARM64 translation granule configured in hardware, and how does Linux choose it?

<details>
<summary>Answers</summary>

1. `PAGE_SIZE` = 2^14 = 16384; `PAGE_MASK` = `~0x3fff`; offset = `0x12345 & 0x3fff` = `0x2345`.
2. A 16 KiB table holds 2048 8-byte entries (11 bits per level), and the offset is 14 bits: 14 + 3×11 = 47. With 4 KiB pages each level gives 9 bits and the offset 12: 12 + 4×9 = 48.
3. Benefits: greater TLB reach (fewer misses), fewer/shallower page tables, fewer page faults. Costs: internal fragmentation and higher memory use; software/ELF files assuming 4 KiB break.
4. `TCR_EL1.TG0` (user half) and `TG1` (kernel half); supported granules appear in `ID_AA64MMFR0_EL1`. Linux picks one at build time with `CONFIG_ARM64_{4K,16K,64K}_PAGES`.

</details>

### Source pointers

- `arch/x86/include/asm/page_types.h` (`PAGE_SHIFT`, `PAGE_SIZE`, `PAGE_MASK`), `arch/x86/include/asm/pgtable_64_types.h`
- `arch/arm64/include/asm/page-def.h`, `arch/arm64/Kconfig` (`ARM64_4K_PAGES` etc.), `arch/arm64/include/asm/pgtable-hwdef.h` (`TCR_TG0_*`, `TCR_TG1_*`)
- `include/linux/mm.h`, `include/linux/mm_types.h` (`struct page`), `include/linux/gfp.h`, `mm/page_alloc.c`
- `Documentation/arch/arm64/memory.rst`, `Documentation/admin-guide/mm/hugetlbpage.rst`, `Documentation/admin-guide/mm/transhuge.rst`, `Documentation/mm/page_tables.rst`

---

## 12. Synchronisation: Spinlocks, RW Locks and RCU

> **Remember**
> - A **spinlock** busy-waits and disables preemption: hold it briefly and **never sleep** while holding it. Use `spin_lock_irqsave()` if an interrupt handler also takes the lock.
> - A **reader-writer lock** (`rwlock_t`) lets many readers in at once, or one writer. It is rarely the right choice: readers bounce the lock's cache line, and writers can be starved.
> - **RCU** readers take no lock and do no atomic writes: `rcu_read_lock()`, then `rcu_dereference()`. Writers publish a new copy with `rcu_assign_pointer()` and free the old one only after a **grace period** (`synchronize_rcu()` / `kfree_rcu()`).
> - RCU suits read-mostly data. Writers still need their own lock (usually a spinlock) to keep out other writers.
> - With `CONFIG_PREEMPT_RT`, `spinlock_t` and `rwlock_t` become sleeping locks; `raw_spinlock_t` is always a true spinning lock.

*Raw notes so far: only the heading "Synchronization RCU, RWLocks and Spinlocks". The detail below is background to support the lecture; it will be extended as the notes grow.*

### Overview

The kernel is fully **preemptible** and runs on many CPUs at once, and interrupts can arrive at any moment. Any data that more than one of these can reach is shared, and needs protection to avoid a **race condition**. Spinlocks, reader-writer locks and RCU are the main tools for code that may not sleep, or for hot read paths. Sleeping locks (`mutex`, `rw_semaphore`) are for process context only.

### Key concepts

- **Critical section:** code that touches shared data and must not run at the same time as another user of that data.
- **Concurrency sources:** true parallelism (SMP), preemption, interrupts (hardirq), softirqs/tasklets, and sleeping in the middle of an update.
- **Atomic context:** hardirq, softirq, or holding a spinlock / preemption disabled. Code here must not sleep (`kmalloc(GFP_KERNEL)`, `mutex_lock()`, `copy_from_user()` and `msleep()` can all sleep).
- **Spinlock:** a waiter spins (busy-waits) until the holder releases it. Taking one disables preemption on that CPU. On a uniprocessor non-preempt kernel it compiles to almost nothing.
- **Ticket / queued spinlocks:** modern x86 uses **qspinlock** (MCS-based) so waiters spin on their own cache line and get the lock in FIFO order.
- **RW lock:** many concurrent readers or one writer. Readers can starve writers; every reader still writes to the lock word (cache-line bouncing).
- **RCU (Read-Copy-Update):** readers run lock-free; a writer copies, modifies, publishes the new version, then waits for all pre-existing readers to finish (a **grace period**) before freeing the old one.
- **Quiescent state:** a point where a CPU cannot be in an RCU read-side critical section (e.g. a context switch, idle, user mode). A grace period ends once every CPU has passed through one.

### How it works: choosing a lock variant

```text
Who else takes this lock?            Lock call to use (process ctx side)
-----------------------------------  ------------------------------------
only process context                 spin_lock()          (or a mutex if you may sleep)
+ softirq / tasklet / timer          spin_lock_bh()       (disables bottom halves)
+ hardirq handler                    spin_lock_irqsave()  (disables local IRQs, saves flags)
inside the hardirq handler itself    spin_lock()          (IRQs already off on this CPU)
```

Why `irqsave`: if process context holds lock L and an interrupt on the **same CPU** tries to take L, the handler spins forever, because the holder cannot run until the handler returns. That is a self-deadlock.

### How it works: an RCU update

```text
 readers:   [--- see old ---]       [-- see old or new --]  [--- see new ---]
                     |                         |
 writer:  copy+modify old -> rcu_assign_pointer(gp, new) -> synchronize_rcu() -> kfree(old)
                                      ^ publish                ^ waits for every reader that
                                                                 might still hold "old"
 grace period:                        |<------------------------>|
```

Readers never block the writer, and the writer never blocks readers. The cost is that the writer waits (or defers the free), and readers may briefly see the old version.

### Comparison

| | Spinlock | RW lock | RCU |
| --- | --- | --- | --- |
| Readers in parallel | No | Yes | Yes, lock-free |
| Reader cost | Atomic op + cache-line bounce | Atomic op + bounce | Almost zero (preemption disable, or nothing, depending on config) |
| Writer cost | Low | Low, but can be starved | High: copy plus grace period |
| Readers may sleep | No | No | No (use **SRCU** if they must) |
| Best for | Short critical sections, any mix | Rarely; legacy code | Read-mostly data: routing tables, module lists, `struct file` tables |

### Key APIs / structures

| API | Header | Purpose | Context |
| --- | --- | --- | --- |
| `DEFINE_SPINLOCK(l)` / `spin_lock_init(&l)` | `<linux/spinlock.h>` | Declare / initialise a `spinlock_t` | Any |
| `spin_lock()` / `spin_unlock()` | `<linux/spinlock.h>` | Take / release; disables preemption | Any; must not sleep while held |
| `spin_lock_bh()` / `spin_unlock_bh()` | `<linux/spinlock.h>` | Also disables softirqs on this CPU | Process / softirq |
| `spin_lock_irqsave(&l, flags)` / `spin_unlock_irqrestore()` | `<linux/spinlock.h>` | Also disables local IRQs and saves the previous IRQ state | Any, including hardirq |
| `spin_trylock()` | `<linux/spinlock.h>` | Take the lock if free, never spin; returns 1 on success | Any |
| `raw_spinlock_t`, `raw_spin_lock()` | `<linux/spinlock.h>` | Always spins, even on `PREEMPT_RT` | Any; very short sections only |
| `DEFINE_RWLOCK(l)`, `read_lock()` / `write_lock()` (+ `_bh`, `_irqsave`) | `<linux/rwlock.h>` (via `spinlock.h`) | Reader-writer spinlock | Any; must not sleep |
| `seqlock_t`, `read_seqbegin()` / `read_seqretry()` | `<linux/seqlock.h>` | Writer-priority alternative: readers retry if a write happened | Any |
| `rcu_read_lock()` / `rcu_read_unlock()` | `<linux/rcupdate.h>` | Mark an RCU read-side critical section | Any; must not sleep inside |
| `rcu_dereference(p)` | `<linux/rcupdate.h>` | Load an RCU-protected pointer safely | Inside a read-side section |
| `rcu_assign_pointer(p, v)` | `<linux/rcupdate.h>` | Publish a new pointer (with a release barrier) | Writer, under the update lock |
| `synchronize_rcu()` | `<linux/rcupdate.h>` | Block until a grace period has elapsed | Process context only (**sleeps**) |
| `call_rcu(&head, fn)` / `kfree_rcu(ptr, field)` | `<linux/rcupdate.h>` | Free after a grace period without blocking | Any |
| `list_add_rcu()`, `list_del_rcu()`, `list_for_each_entry_rcu()` | `<linux/rculist.h>` | RCU-safe linked lists | Writers under lock; readers in RCU section |

### Code example: spinlock-protected writer, RCU reader

```c
// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>	/* module_init(), module_exit(), MODULE_*() */
#include <linux/slab.h>	/* kmalloc(), kfree() */
#include <linux/spinlock.h>	/* DEFINE_SPINLOCK(), spin_lock(), spin_unlock() */
#include <linux/rcupdate.h>	/* rcu_read_lock(), rcu_dereference(), rcu_assign_pointer(), kfree_rcu() */

struct cfg {	/* the shared, read-mostly data */
	int value;	/* the payload readers want */
	struct rcu_head rcu;	/* needed by kfree_rcu() to defer the free */
};	/* end of struct cfg */

static struct cfg __rcu *cur_cfg;	/* RCU-protected pointer to the current version */
static DEFINE_SPINLOCK(cfg_lock);	/* serialises writers only; readers never take it */

static int cfg_read(void)	/* reader: lock-free, may run on many CPUs at once */
{	/* start of cfg_read() */
	struct cfg *c;	/* local copy of the pointer */
	int v;	/* value to return */

	rcu_read_lock();	/* start read-side critical section (must not sleep inside) */
	c = rcu_dereference(cur_cfg);	/* safely load the current pointer */
	v = c ? c->value : -1;	/* use the data; -1 if nothing published yet */
	rcu_read_unlock();	/* end read-side section; c must not be used after this */
	return v;	/* return the value read */
}	/* end of cfg_read() */

static int cfg_update(int value)	/* writer: copy, publish, free old after grace period */
{	/* start of cfg_update() */
	struct cfg *new, *old;	/* the new version and the version it replaces */

	new = kmalloc(sizeof(*new), GFP_KERNEL);	/* allocate before taking the spinlock (GFP_KERNEL may sleep) */
	if (!new)	/* allocation failed? */
		return -ENOMEM;	/* report out of memory */
	new->value = value;	/* fill in the new version completely before publishing */

	spin_lock(&cfg_lock);	/* keep other writers out */
	old = rcu_dereference_protected(cur_cfg, lockdep_is_held(&cfg_lock));	/* read pointer as the lock holder */
	rcu_assign_pointer(cur_cfg, new);	/* publish: readers now see new (release barrier) */
	spin_unlock(&cfg_lock);	/* writers may proceed */

	if (old)	/* was there a previous version? */
		kfree_rcu(old, rcu);	/* free it once all current readers are done */
	return 0;	/* success */
}	/* end of cfg_update() */

static int __init rcu_demo_init(void)	/* runs at insmod */
{	/* start of rcu_demo_init() */
	int ret;	/* return code from cfg_update() */

	ret = cfg_update(42);	/* publish the first version */
	if (ret)	/* did it fail? */
		return ret;	/* abort the load with the error */
	pr_info("rcu_demo: value=%d\n", cfg_read());	/* read it back through the RCU reader */
	return 0;	/* load succeeded */
}	/* end of rcu_demo_init() */

static void __exit rcu_demo_exit(void)	/* runs at rmmod */
{	/* start of rcu_demo_exit() */
	struct cfg *old = rcu_dereference_protected(cur_cfg, 1);	/* no readers left: plain access is safe */

	RCU_INIT_POINTER(cur_cfg, NULL);	/* unpublish (no barrier needed for NULL) */
	synchronize_rcu();	/* wait out any reader that started before the unpublish */
	kfree(old);	/* now nothing can reference it */
	rcu_barrier();	/* wait for pending kfree_rcu() callbacks before the module text goes away */
}	/* end of rcu_demo_exit() */

module_init(rcu_demo_init);	/* register the entry point */
module_exit(rcu_demo_exit);	/* register the exit point */
MODULE_LICENSE("GPL");	/* GPL: RCU and lockdep helpers are GPL-only symbols */
MODULE_DESCRIPTION("RCU reader with spinlock-serialised writer demo");	/* shown by modinfo */
```

⚠️ Verify: not yet built on the test box. `kfree(NULL)` is safe, so the exit path needs no `NULL` check.

### Config and version dependencies

- `CONFIG_SMP=n`: spinlocks reduce to preemption disable (or nothing on a non-preemptible kernel). Races with interrupts still exist, so `_irqsave` still matters.
- `CONFIG_PREEMPT_RT` (merged into mainline in **6.12**): `spinlock_t` and `rwlock_t` become sleeping, priority-inheriting **rt_mutex**-based locks, and most interrupt handlers run in threads. Code that must truly spin (e.g. in the scheduler or low-level IRQ code) uses `raw_spinlock_t`.
- RCU flavours: `CONFIG_PREEMPT_RCU` (preemptible kernels: readers can be preempted, so `rcu_read_lock()` keeps a nesting count) vs `TREE_RCU` on non-preemptible kernels (`rcu_read_lock()` just disables preemption). Since 4.20 the old rcu-bh and rcu-sched flavours have been folded into one, so `synchronize_rcu()` covers them all.

### Commands / debugging

```bash
grep -E 'CONFIG_(PROVE_LOCKING|DEBUG_SPINLOCK|PREEMPT_RT|PREEMPT_RCU|DEBUG_ATOMIC_SLEEP)=' /boot/config-$(uname -r)   # which lock-debug options this kernel has
sudo cat /proc/lockdep_stats                      # lockdep counters (only with CONFIG_PROVE_LOCKING)
sudo cat /proc/lock_stat                          # per-lock contention stats (CONFIG_LOCK_STAT; echo 0 > to reset)
sudo perf lock record -- sleep 5                  # record lock events system-wide for 5 s
sudo perf lock report                             # show contended locks from the recording
sudo bpftrace -e 'kprobe:queued_spin_lock_slowpath { @[kstack(5)] = count(); }'   # who hits the contended spinlock slow path
dmesg | grep -E 'BUG: sleeping function|BUG: scheduling while atomic|rcu_.*stall|possible circular locking'   # typical lock/RCU bug reports
```

- **lockdep** (`CONFIG_PROVE_LOCKING`) proves lock-ordering and IRQ-safety bugs the first time a bad pattern *could* happen, not only when it deadlocks.
- `CONFIG_DEBUG_ATOMIC_SLEEP` reports "sleeping function called from invalid context".
- **RCU CPU stall warnings** ("rcu: INFO: rcu_preempt detected stalls") mean a CPU has not reached a quiescent state for ~21 s, often a loop holding a lock or sitting in a read-side section.
- The Ubuntu `-generic` test-box kernel does not enable lockdep; boot a debug kernel with `vng` to try it.

### Pitfalls

- **Sleeping while holding a spinlock** or inside `rcu_read_lock()`: `kmalloc(GFP_KERNEL)`, `mutex_lock()`, `copy_to_user()`, `msleep()`. Allocate first, or use `GFP_ATOMIC`.
- **Using `spin_lock()` when an IRQ handler also takes the lock** causes self-deadlock on one CPU. Use `spin_lock_irqsave()` in process context.
- **Lock-order inversion (ABBA):** CPU0 holds A and wants B while CPU1 holds B and wants A. Fix it with one documented global order.
- **Recursion:** Linux spinlocks are not recursive; taking the same lock twice deadlocks.
- **Using an RCU pointer after `rcu_read_unlock()`**, or freeing the old version before a grace period: use-after-free.
- **Plain loads/stores of RCU pointers:** always use `rcu_dereference()` / `rcu_assign_pointer()`, otherwise the compiler or CPU can reorder the initialisation after the publish.
- **Unloading a module with `call_rcu()`/`kfree_rcu()` callbacks pending:** call `rcu_barrier()` in `module_exit`.
- **Holding a spinlock for a long time:** other CPUs burn cycles and latency rises. Keep sections short or use a mutex.

### Revision questions

1. Why must process context use `spin_lock_irqsave()` for a lock that the device's interrupt handler also takes?
2. Why can't `synchronize_rcu()` be called while holding a spinlock, and what can you use instead?
3. When would you choose RCU over an `rwlock_t`, and what is RCU's main cost?
4. What happens to `spinlock_t` on a `CONFIG_PREEMPT_RT` kernel, and when must you use `raw_spinlock_t`?

<details>
<summary>Answers</summary>

1. If the interrupt arrives on the same CPU while process context holds the lock, the handler spins forever: the holder cannot run until the handler returns. Disabling local IRQs while holding the lock prevents this. `irqsave` also restores the previous IRQ state, so it is safe even if IRQs were already off.
2. `synchronize_rcu()` sleeps until a grace period ends, and sleeping in atomic context is a bug (it can also deadlock). Use `call_rcu()` or `kfree_rcu()` to defer the free without blocking.
3. For read-mostly data on hot paths: RCU readers take no lock and write no shared cache line, so they scale perfectly and never block writers. The cost is on the update side: copying, waiting for a grace period (or deferring frees), and readers may see stale data briefly.
4. It becomes a sleeping, priority-inheriting lock based on `rt_mutex`, so holders can be preempted. Use `raw_spinlock_t` where the code truly cannot sleep even on RT: scheduler internals, low-level interrupt and timer code, and very short sections called with IRQs disabled.

</details>

### Source pointers

- `include/linux/spinlock.h`, `include/linux/spinlock_types.h`, `include/linux/rwlock.h`, `kernel/locking/spinlock.c`, `kernel/locking/qspinlock.c`
- `include/linux/spinlock_rt.h`, `kernel/locking/spinlock_rt.c`, `kernel/locking/rtmutex.c` (PREEMPT_RT)
- `include/linux/rcupdate.h`, `include/linux/rculist.h`, `kernel/rcu/tree.c`, `kernel/rcu/update.c`
- `kernel/locking/lockdep.c`
- `Documentation/locking/spinlocks.rst`, `Documentation/locking/locktypes.rst`, `Documentation/locking/lockdep-design.rst`, `Documentation/RCU/whatisRCU.rst`, `Documentation/RCU/checklist.rst`, `Documentation/kernel-hacking/locking.rst`

---

## Labs & Exercises

*None yet.*

---

## Quick Reference

### Boot and images (§1)

| Item | Meaning |
| ---- | ------- |
| `uname -r` / `-m` / `-v` | Running release / architecture / build string |
| `/boot/vmlinuz-<ver>` | Compressed bootable kernel (`bzImage` on x86) |
| `/boot/initrd.img-<ver>` | initramfs: mounts the real root |
| `/boot/System.map-<ver>` | Static, link-time symbol table |
| `/boot/config-<ver>` | Build config (`CONFIG_*`) |
| `vmlinux` | Uncompressed ELF for debuggers |
| `/proc/cmdline` | Boot command line |
| `/proc/kallsyms` | Live symbol addresses (real values for root only) |
| `kernel.kptr_restrict` | 0 = no restriction, 1 = `CAP_SYSLOG` only (Ubuntu), 2 = nobody (Android) |
| `CONFIG_EFI_STUB` | bzImage is also a PE32+ EFI application |
| `CONFIG_KERNEL_{XZ,LZ4,ZSTD}` | Smallest / fastest / best trade-off (Ubuntu default) |
| `journalctl -k -b \| grep Memory:` | Kernel image size in RAM |

### Kernel origins (§2)

| Item | Meaning |
| ---- | ------- |
| mainline → stable/LTS → distro / BSP | Where every kernel comes from |
| GKI + KMI | Android: one core kernel + vendor modules against a stable interface |
| GPL-2.0 | Distributing a modified kernel → must provide the source |

### Memory layout and vDSO (§3, §4)

| Item | Meaning |
| ---- | ------- |
| x86_64 user / kernel (4-level) | `0x0`–`0x7fff_ffff_ffff` / from `0xffff_8000_0000_0000` |
| `copy_{from,to}_user()` | Only safe way to touch user memory; may sleep |
| `/proc/<pid>/maps` | range, perms (`p`/`s`), offset, dev, inode, path |
| `/proc/meminfo` | Kernel usage: `Slab`, `KernelStack`, `PageTables`, `VmallocUsed` |
| `vm.mmap_min_addr` | Lowest mappable address (65536): NULL deref always faults |
| `[vdso]` / `[vvar]` | Kernel-supplied library + data page: syscall-free `clock_gettime` |
| `[vsyscall]` `0xffffffffff600000` | Legacy x86_64 fixed page; emulated |
| x86_64 syscall entry | `syscall` instruction, number in `rax`; `__NR_read` = 0 (i386: 3) |

### Processes and kernel threads (§5)

| Item | Meaning |
| ---- | ------- |
| PID 0 / 1 / 2 | idle (`swapper`) / `init` (systemd) / `kthreadd` |
| `ps --ppid 2` | List all kernel threads |
| `[name]`, `VSZ 0` | Kernel thread: no cmdline, `mm == NULL` |
| `/proc/<pid>/task/<tid>` | Per-thread entries |
| `kthread_run()` / `kthread_should_stop()` / `kthread_stop()` | Kernel thread lifecycle |

### Tasks (§5)

| Item | Meaning |
| ---- | ------- |
| `struct task_struct` (`<linux/sched.h>`) | One per thread; process-wide state shared via `mm`, `files`, `fs`, `signal`, `sighand` pointers |
| `task->pid` / `task->tgid` | Thread ID (TID) / thread-group ID (user-space PID); main thread: `pid == tgid` |
| `getpid()` / `gettid()` | Returns TGID / TID |
| `init_task` | Static `task_struct` of PID 0; head of the circular `tasks` list |
| `for_each_process(p)` / `for_each_thread(p, t)` | Walk all processes / one process's threads under `rcu_read_lock()` |
| `ps -eLf`, `/proc/<pid>/task/`, `Tgid:` in `/proc/<pid>/status` | See threads and their IDs |

### Headers and modules (§6, §7)

| Item | Meaning |
| ---- | ------- |
| `/lib/modules/$(uname -r)/build` | Headers package used for module builds |
| `/usr/include/linux/` | UAPI headers: user space only |
| `make -C /lib/modules/$(uname -r)/build M=$PWD modules` | Build an out-of-tree module |
| `module_init()` / `module_exit()` / `MODULE_LICENSE("GPL")` | Module skeleton essentials |
| GPL-compatible licences | `"GPL"`, `"GPL v2"`, `"GPL and additional rights"`, `"Dual BSD/GPL"`, `"Dual MIT/GPL"`, `"Dual MPL/GPL"` |
| `EXPORT_SYMBOL_GPL` | Usable only by GPL-compatible modules (~60 % of exports) |
| `insmod` / `rmmod` | Load file / unload: **no** dependency handling |
| `modprobe` / `modprobe -r` | Load/unload with dependencies (`modules.dep`, `depmod -a`) |
| `lsmod` = `/proc/modules` | Loaded modules, refcounts, users |
| `modinfo` / `.modinfo` | Metadata: license, parm, vermagic, depends, alias |
| `modinfo -F depends <m>`; `filename: (builtin)` | Dependencies; built-in check |
| `/sys/module/<name>/parameters/` | Module (incl. built-in) parameters |
| `insmod m.ko p=42 s=text` / `options m p=42` in `/etc/modprobe.d/` | Set parameters at load / persistently |
| `module_param(name, type, perm)`; types `int`, `bool`, `charp`, `byte`, … | `perm` 0 = hidden, 0444 = read-only, 0644 = root-writable at runtime |
| `/proc/sys/kernel/tainted` | Taint flags (0 = clean, `P` = proprietary module) |
| `/proc/sys/kernel/` = `sysctl kernel.*` | Core-kernel run-time tunables; persist in `/etc/sysctl.d/` |
| `echo 1 \| sudo tee /proc/sys/kernel/modules_disabled` | Block module load **and** unload until reboot (one-way) |
| sched_ext (`CONFIG_SCHED_CLASS_EXT`, 6.12+) | Custom scheduling via BPF, not modules |

### Capabilities (§8)

| Item | Meaning |
| ---- | ------- |
| `grep ^Cap /proc/$$/status` / `capsh --decode=<hex>` | Show / decode capability sets |
| `getcap` / `setcap` | Read / set file capabilities |
| `capable(CAP_X)` / `ns_capable()` | Kernel-side checks; return `-EPERM` if missing |
| `CAP_SYS_MODULE` (16) | Load/unload modules |

### Kernel architecture (§9)

| Item | Meaning |
| ---- | ------- |
| Monolithic (Linux) | All services in one kernel address space; direct calls; fast, no isolation |
| x86_64 split (4-level) | 2^47 (128 TiB) user + 2^47 kernel; the rest of 2^64 is a non-canonical hole (#GP) |
| Microkernel (QNX, seL4, MINIX 3) | IPC + sched + basic mm in kernel; servers in user space |
| Hybrid (NT, XNU) | Microkernel structure, mostly monolithic in practice |
| FUSE / UIO / VFIO / eBPF | Ways Linux moves or sandboxes work outside core kernel code |

### Tracing (§10)

| Item | Meaning |
| ---- | ------- |
| `/sys/kernel/tracing` | tracefs: ftrace control files (`trace`, `trace_pipe`, `events/`) |
| `current_tracer` / `available_tracers` | Select tracer: `nop`, `function`, `function_graph`, … |
| `set_ftrace_filter` / `set_graph_function` / `set_ftrace_pid` | Limit what is traced (always filter) |
| `trace-cmd record -p function_graph -g <fn> <cmd>` | Record a call graph from the command line |
| `kprobe_events`: `p:name sym` / `r:name sym $retval` | Define a kprobe / kretprobe event without code |
| `register_kprobe()` / `unregister_kprobe()` | Module API (GPL-only); handler is atomic |
| `bpftrace -e 'kprobe:sym { … }'` | kprobe via eBPF |
| `bpftrace -e 'kprobe:tcp_v4_connect { … arg1 … }'` | Who connects where (IPv4); `argN` = Nth function arg; `curl -4` to test |
| `bpftrace -l 'kprobe:tcp*'` / `-lv 'tracepoint:…'` | List available probes (wildcards); `-v` shows arguments |
| `bpftrace -e 'uprobe:/path/bin:sym { … }'` / `uprobe_events` | uprobe (3.5+): probe user-space functions by file + symbol/offset |
| `/sys/kernel/debug/kprobes/{list,blacklist}` | Active probes / unprobeable functions |
| `trace_marker` | User space writes text into the ftrace buffer |
| atrace format | `B\|pid\|name`, `E\|pid`, `C\|pid\|name\|value` |
| `strace -f -e trace=… / -c / -p <pid>` | Syscall tracing via ptrace (slow: 2 stops per syscall) |
| `kernel.yama.ptrace_scope` | 0 classic, 1 descendants only (Ubuntu), 2 `CAP_SYS_PTRACE` only, 3 disabled |
| `perf trace` | Low-overhead strace alternative |
| `process_vm_readv()` / `process_vm_writev()` | Cross-memory attach (3.2+): one-copy read/write of another process's memory; ptrace-attach permission |


### Pages (§11)

| Item | Meaning |
| ---- | ------- |
| `PAGE_SIZE` / `PAGE_SHIFT` / `PAGE_MASK` | 4096 / 12 / `~0xfff` on x86_64; `PAGE_SIZE = 1UL << PAGE_SHIFT` |
| `getconf PAGESIZE` / `sysconf(_SC_PAGESIZE)` | Page size at run time (never hard-code 4096) |
| x86_64 page sizes | 4 KiB base; 2 MiB / 1 GiB huge |
| ARM64 granule | `TCR_EL1.TG0/TG1`; 4K/16K/64K chosen by `CONFIG_ARM64_*_PAGES` at build |
| `alloc_pages(gfp, order)` | 2^order contiguous pages; `get_order(size)` to compute the order |
| `/proc/meminfo` `Hugepagesize`, `/sys/kernel/mm/transparent_hugepage/enabled` | Huge page size and THP mode |

### Synchronisation (§12)

| Item | Meaning |
| ---- | ------- |
| `spin_lock()` / `_bh()` / `_irqsave(&l, flags)` | Process-only / + softirq users / + hardirq users; never sleep while held |
| `raw_spinlock_t` | Always spins, even on `PREEMPT_RT` (where `spinlock_t` sleeps) |
| `read_lock()` / `write_lock()` | `rwlock_t`: many readers or one writer; prefer RCU or seqlock |
| `rcu_read_lock()` → `rcu_dereference()` → `rcu_read_unlock()` | Lock-free reader; no sleeping inside |
| `rcu_assign_pointer()` + `synchronize_rcu()` / `kfree_rcu()` | Publish new version; free old after a grace period |
| `rcu_barrier()` | In `module_exit`: wait for pending `call_rcu()`/`kfree_rcu()` callbacks |
| `CONFIG_PROVE_LOCKING`, `CONFIG_DEBUG_ATOMIC_SLEEP`, `perf lock` | Lockdep, sleep-in-atomic checks, contention analysis |

**Gotchas:** installed ≠ running; never hard-code a 4096 page size; kprobe handlers must not sleep and must be unregistered in `module_exit`; build modules against `uname -r`; KASLR means `System.map` ≠ runtime addresses; distro/BSP kernels ≠ mainline of the same version; never dereference `__user` pointers; always stop your kthreads in `module_exit`; `modules_disabled=1` cannot be undone without a reboot; never sleep under a spinlock or in an RCU read section; take an IRQ-shared lock with `spin_lock_irqsave()`.

---

## Glossary

| Term | Definition |
| ---- | ---------- |
| **Atomic context** | Code that must not sleep: hardirq, softirq, or with a spinlock held / preemption disabled. |
| **atrace** | Android tracing tool/API: framework code writes begin/end/counter markers to `trace_marker`; collected with kernel events and viewed in Perfetto. |
| **`/boot`** | Directory (often a separate partition) holding kernel images, initramfs, symbol maps and configs. |
| **Boot image (Android)** | `boot.img` in the raw `boot` partition: `ANDROID!` header, kernel and ramdisk. |
| **Boot protocol** | Architecture-specific contract for how a bootloader loads the kernel and passes it control and parameters. |
| **Bootloader** | Program started by the firmware (e.g. GRUB) that loads the kernel and initramfs and passes the command line. |
| **bpftrace** | High-level tracing language that compiles one-liners to eBPF and attaches them to kprobes, uprobes and tracepoints. |
| **BSP** | Board Support Package: an SoC vendor's kernel tree, Device Trees, drivers and bootloader. |
| **BTF** | BPF Type Format: compact kernel type information (`/sys/kernel/btf/vmlinux`) that lets bpftrace/BPF use kernel structs without headers. |
| **bzImage** | x86 "big zImage" format: setup code plus a self-decompressing compressed kernel. |
| **Canonical address** | 64-bit address whose unused top bits all equal the highest implemented bit; any other address faults. |
| **`CAP_SYSLOG`** | Capability needed to see real kernel addresses when `kptr_restrict=1`. |
| **Capability** | One independent slice of root's privileges (`CAP_*`), checked by the kernel per operation. |
| **Capability sets** | Per-thread bitmasks: Effective, Permitted, Inheritable, Bounding, Ambient. |
| **`charp`** | `module_param` type for a string parameter (`char *`); the kernel stores a copy of the value. |
| **Critical section** | Code accessing shared data that must not run concurrently with other users of that data. |
| **Cross-memory attach (CMA)** | `process_vm_readv()`/`process_vm_writev()`: single-copy transfer between two processes' address spaces (Linux 3.2+). |
| **`current_tracer`** | tracefs file selecting the active ftrace tracer (`nop`, `function`, `function_graph`, …). |
| **Demand paging** | Allocating or loading a physical page only when a mapped virtual page is first accessed. |
| **`depmod`** | Tool that generates `modules.dep` (the module dependency list) for `modprobe`. |
| **Distribution kernel** | Kernel built and patched by a distro (Ubuntu, Fedora, …) from a stable/LTS release. |
| **EFI stub** | Code linked into the kernel image that makes it a PE/COFF EFI application that UEFI can run directly. |
| **`EXPORT_SYMBOL_GPL`** | Export macro restricting a symbol to GPL-compatible modules. |
| **File capabilities** | Capabilities attached to an executable (`security.capability` xattr), granted at `exec`. |
| **Fork (of the kernel)** | Private branch of patched kernel source that must be rebased on every upstream release. |
| **ftrace** | The kernel's built-in function and event tracer, writing to a per-CPU ring buffer controlled via tracefs. |
| **`function_graph`** | ftrace tracer hooking function entry and exit, showing an indented call tree with durations. |
| **GKI** | Generic Kernel Image: Android's single common kernel binary; vendor code goes in modules against a stable KMI. |
| **GPL-2.0** | The kernel's licence: distributing modified binaries requires providing the source. |
| **Grace period** | RCU interval after which every reader that started before it has finished. |
| **Granule (translation)** | ARM64 term for the base page size used by the MMU (4, 16 or 64 KiB), set in `TCR_EL1`. |
| **Headers package** | Distro package with the headers, Kbuild files, `.config` and `Module.symvers` for building modules against one kernel. |
| **Huge page** | A page mapped at a higher page-table level (e.g. 2 MiB or 1 GiB on x86_64) to cut TLB misses; via hugetlbfs or THP. |
| **Hybrid kernel** | Kernel with a microkernel-style structure but most services in kernel mode (Windows NT, macOS XNU). |
| **Idle task (PID 0)** | Static `init_task` (`swapper`); per-CPU idle loop; parent of PIDs 1 and 2. |
| **`init` (PID 1)** | First user-space process (systemd); adopts orphans; its exit panics the kernel. |
| **initramfs** | Compressed `cpio` archive unpacked into RAM as the early root filesystem; mounts the real root. |
| **KASLR** | Kernel Address Space Layout Randomisation: a random kernel base address at each boot. |
| **Kernel space** | Upper part of every virtual address space: shared, accessible only in kernel mode. |
| **Kernel thread** | Task that runs only in kernel mode, with no user address space (`mm == NULL`). |
| **KMI** | Kernel Module Interface: the stable symbol/ABI set GKI guarantees to vendor modules. |
| **kprobe** | Dynamic breakpoint-based probe on almost any kernel instruction, with a handler run in atomic context. |
| **`kptr_restrict`** | Sysctl controlling whether kernel pointers are shown (`/proc/kallsyms`, `%pK`). |
| **kretprobe** | kprobe variant that runs a handler when the probed function returns. |
| **`kthreadd` (PID 2)** | Kernel thread that creates all other kernel threads. |
| **kworker** | Workqueue worker kernel thread. |
| **Lazy TLB** | Kernel threads borrowing the previous task's page tables (`active_mm`) to avoid a switch. |
| **libc** | C runtime library (glibc on Ubuntu): standard C functions and system-call wrappers. |
| **Loadable kernel module (LKM)** | `.ko` object loaded into the running kernel at predefined extension points. |
| **lockdep** | Kernel lock validator (`CONFIG_PROVE_LOCKING`) that reports lock-order and IRQ-safety bugs before they deadlock. |
| **Mainline** | Linus Torvalds' upstream kernel tree. |
| **Mapped** | A virtual page backed by a page-table entry pointing to a physical frame; access to an unmapped page faults. |
| **Microkernel** | Kernel that keeps only IPC, scheduling and basic memory management in kernel mode; other services run as user-space servers. |
| **`mmap()`** | System call that creates a virtual memory mapping (VMA) in a process. |
| **`.modinfo`** | ELF section of a `.ko` holding `key=value` module metadata. |
| **Module parameter** | Module variable settable at load time (`name=value`) and exposed in `/sys/module/<mod>/parameters/`. |
| **`modules_disabled`** | One-way sysctl (`kernel.modules_disabled`) that blocks all module loading and unloading until reboot. |
| **`Module.symvers`** | Build output listing exported symbols and their CRCs, used by modpost. |
| **modversions** | Per-symbol CRC checking of a module's imports against the kernel (`CONFIG_MODVERSIONS`). |
| **Monolithic kernel** | Kernel whose services (syscalls, mm, filesystems, networking, drivers) all run in one privileged address space and call each other directly. |
| **Page** | Smallest unit of memory the MMU maps and protects (4 KiB on x86_64; 4/16/64 KiB on ARM64). |
| **Page fault** | CPU exception on access to an unmapped or protected page, handled by the kernel. |
| **Page frame / PFN** | A physical page, and its number (`phys >> PAGE_SHIFT`). |
| **PCB** | Process control block: textbook per-process descriptor (address space, open files, credentials); in Linux, the shared structs a `task_struct` points to. |
| **PE32+** | 64-bit Portable Executable format used by UEFI applications (and Windows). |
| **`PREEMPT_RT`** | Real-time preemption model (mainline since 6.12): spinlocks become sleeping rt_mutex locks, IRQs are threaded. |
| **`/proc/kallsyms`** | Live kernel (and module) symbol table with runtime addresses. |
| **`ptrace()`** | System call letting a tracer process stop, inspect and modify a tracee; the basis of gdb and strace. |
| **qspinlock** | Queued (MCS-based) spinlock implementation used on x86 and ARM64: FIFO, each waiter spins locally. |
| **Quiescent state** | Point where a CPU cannot be inside an RCU read-side section (context switch, idle, user mode). |
| **Race condition** | Bug where the result depends on the timing of concurrent accesses to shared data. |
| **RCU** | Read-Copy-Update: lock-free readers; writers publish a new copy and free the old after a grace period. |
| **Reader-writer lock** | `rwlock_t`: spinning lock allowing many readers or one writer. |
| **RELRO** | Relocation Read-Only: ELF data made read-only after dynamic linking. |
| **Rescuer thread** | Per-workqueue `kworker/R-*` thread that guarantees progress under memory pressure. |
| **sched_ext** | Extensible scheduling class (6.12+) whose policy is a BPF program. |
| **SMAP / PAN** | x86 / ARM64 feature that blocks kernel access to user pages except via the user-copy routines. |
| **Spinlock** | Busy-waiting lock that disables preemption while held; for short critical sections that cannot sleep. |
| **SRCU** | Sleepable RCU: an RCU variant whose readers may sleep. |
| **strace** | Tool that prints every system call of a process, using `ptrace()`. |
| **`syscall` instruction** | x86_64 system-call entry instruction; `sysenter`/`int 0x80` are the 32-bit equivalents. |
| **sysctl** | Run-time kernel tunable exposed under `/proc/sys/`; set with `sysctl -w` or `/etc/sysctl.d/`. |
| **`System.map`** | Link-time kernel symbol table (address, type, name). |
| **Taint** | Kernel flag recording conditions (e.g. a proprietary module loaded) that affect debugging and support. |
| **Task list** | Circular doubly-linked list of all processes through `task->tasks`, headed by `init_task`; walked with `for_each_process()`. |
| **`TASK_SIZE`** | Top of the user-space address range. |
| **`task_struct`** | Kernel structure describing every task (thread, process or kernel thread). |
| **`tasklist_lock`** | Global rwlock protecting the task lists; readers usually use RCU instead. |
| **TCB** | Thread control block: textbook per-thread descriptor (state, registers, scheduling); in Linux, part of `task_struct`. |
| **TGID** | Thread-group ID: what user space calls the PID; each thread has its own TID. |
| **TID** | Thread ID: the kernel's `task->pid`; returned by `gettid()`. |
| **`trace_marker`** | tracefs file through which user space writes text events into the ftrace ring buffer. |
| **tracefs** | Pseudo-filesystem (`/sys/kernel/tracing`) exposing ftrace controls and output. |
| **Tracepoint** | Static, named trace hook compiled into kernel source; a more stable interface than kprobes. |
| **UAPI** | User-space API headers (`include/uapi/`), exported to `/usr/include`; a stable ABI. |
| **uprobe** | Dynamic probe (Linux 3.5+) on an instruction in a user-space binary or library, placed by file + offset; affects every process mapping that file. |
| **Upstreaming** | Getting a change merged into mainline so that the community maintains it. |
| **USDT** | User Statically Defined Tracing: static probe markers in user programs, activated via uprobes. |
| **User space** | Lower, per-process part of the virtual address space. |
| **vDSO** | Virtual Dynamic Shared Object: kernel-provided ELF library mapped into every process for syscall-free calls. |
| **vermagic** | Module string recording the kernel version and key config; must match the running kernel. |
| **VMA** | `struct vm_area_struct`: one contiguous virtual memory region of a process. |
| **`vmlinux`** | Uncompressed ELF kernel image with symbols, used for debugging. |
| **`vmlinuz`** | Compressed bootable kernel image installed in `/boot`. |
| **vsyscall** | Legacy x86_64 fixed-address page for fast time calls; now emulated. |
| **Yama** | LSM that restricts `ptrace()` scope (`kernel.yama.ptrace_scope`). |
| **zstd** | Zstandard compression: good ratio and fast decompression; the common default for kernel, initramfs and modules. |

---

## Open Questions

- `file` shows `swap_dev 0XE` for the bzImage: which setup-header field is that exactly, and is it meaningful today?
- "android pe32 embedded in the /boot partition": did the instructor mean the Android boot image (`boot.img`) in the `boot` partition, or an EFI-stub `Image`?
- "User space on the order of 40 bits": which architecture/config was meant (47 on x86_64, 39 on some ARM64)?
- "memset may be HW accelerated" under the vDSO: was this about the vDSO, or about glibc IFUNC / kernel alternatives choosing a CPU-optimal `memset`?
- vsyscall as "wrapper for all syscalls, choosing int/syscall/sysenter": is this the 32-bit `__kernel_vsyscall` (vDSO) rather than the x86_64 vsyscall page? And is `syscall` (not `sysenter`) the modern x86_64 instruction?
- Licensing of out-of-tree proprietary modules: what is the course's position (derivative work or not)?
- Modules "cannot affect low-level scheduling (until 7.1/7.2)": what changed in 7.1/7.2? sched_ext (BPF) has existed since 6.12.
- "Kernel capabilities and where to find them": POSIX capabilities (`CAP_*`), or kernel features/config options?
- Does the course expect us to boot custom kernels via `vng` only, or also install them into `/boot` on the test box?
- Page size, "AAPL/A12/M1 16k by default": why A12 specifically? Apple used 16K pages on earlier A-series chips too.
- "Android 15+ 16k page size by default": did the instructor mean 16K is *supported* from Android 15 (and required of apps on Play), or that specific devices ship 16K by default?
- Synchronisation (§12): raw notes so far are only the heading "Synchronization RCU, RWLocks and Spinlocks". Section 12 is background material; check it against the lecture as notes come in.
