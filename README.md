# UniXNU-tb128fu

> **Work in progress.** iPadOS 17.7.11 reaches the home screen on the tablet and answers touch; it still draws on the CPU.
> The Adreno 610 runs under the kernel driver; the Metal user space is the next step.

Device repository for the Lenovo Xiaoxin Pad 2022 (TB128FU, Snapdragon 680 / SM6225), laid out like
[UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng) and [UniXNU-pipa](https://github.com/xVanTuring/UniXNU-pipa).
Firmware: `uefi-tb128fu`, a separate checkout of the Mu-Silicium tree with `tb128fuPkg` (not published yet).

The UEFI build is **Mu-only**: the stock ABL carries one statically linked AArch64 `LinuxLoader` PE, not separable vendor
DXE modules, so there is no `Binaries/tb128fu` and no vendor clock, PMIC, UFS, touch or button driver. The display relies
on ABL leaving the panel scanning out `cont_splash_region`; `SimpleFbDxe` publishes that buffer, and XNU keeps drawing
into it.

## State on the tablet

| Area | State |
| --- | --- |
| Firmware, UniXNU menu, DarwinBoot | Works; `fastboot flash boot_a` + reboot (the ABL `fastboot boot` buffer is too small) |
| XNU and the root | iPadOS 17.7.11 (21H461) kernel on the 8 cores (4× Cortex-A53, 4× Cortex-A73); the verified OS/SystemOS root and the Data volume on UFS (NativeUfs SM6225) |
| Interrupts and timer | GICv3; the EL1 virtual timer is INTID 19 here; SGIs do not arrive, so the IPIs are software-pended SPIs 504..511 |
| CPU frequency | Both clusters at their top level (1.9 / 2.4 GHz) |
| Display | The home screen appears 70–95 s after boot; QuartzCore draws on the CPU into the firmware frame buffer (1200×2000) |
| Touch | Himax HX83102E on QUP SE0 SPI; the kernel downloads the zero-flash firmware and publishes events through NativeHidInput |
| GPU | Adreno 610 powered at 320 MHz, its SMMU context, zap shader, CP, interrupt and NativeAdreno published; the user-client path passes its self test. The Metal user space (Turnip, metal2vulkan, plugin) is not built for it yet |
| Diagnostics | The boot log goes to the end of userdata and is read over USB by a small Linux reader image; the kernel returns the tablet to fastboot after a set time |
| Not working | Metal and everything that needs the GPU (Settings opens white), audio, Wi-Fi, battery, buttons, sleep |

## What is here

| File | Contents |
| --- | --- |
| `board.toml` | Board, IPSW, firmware, payload and ADT settings |
| `configs/tb128fu_defconfig` | Kernel configuration of the first image |
| `scripts/build-arm-driver.py` | The generic driver archive builder, copied unchanged from UniXNU-sheng |
| `scripts/build-platform-power.py`, `package/ShengPlatformPower.cpp` | The original-PMGR bridge of UniXNU-sheng, which accesses no hardware |
| `scripts/make-tb128fu-adt.py` | Apple device tree for SM6225: GICv3 0x0f200000/0x0f300000, stride 0x20000, A53/A73 cores numbered by cluster, 19.2 MHz timebase |
| `scripts/check-tb128fu.py` | Firmware preflight: UniXNU integration, FD and stack against the memory map, Mu-only inputs, splash region |
| `scripts/patch-xnu-entry-probe.py`, `scripts/xnu-panic-screen.c` | Copied unchanged from UniXNU-sheng; the payload uses them for the on-screen panic text (there is no UART) |
| `scripts/build-native-legacy-display.py`, `package/LegacyDisplayProbe.cpp` | The display, key store stand-in and HID input of UniXNU-qemu on the firmware frame buffer, with the board's touch and GPU probes |
| `package/TouchProbe.cpp`, `package/HimaxProtocol.[ch]`, `package/TouchBoard.h` | The Himax HX83102E touch driver (GENI SPI, zero-flash firmware download, event loop); host tests in `package/HimaxProtocolTests.c` |
| `package/GpuProbe.cpp`, `scripts/build-native-adreno-a6xx.py` | Adreno 610 bring-up (`tb128fu-gpu=<stage>`) and the a6xx build of NativeAdreno without a GMU |
| `package/BootLog.c`, `package/BootReset.c`, `package/linux-log/`, `scripts/read-tb128fu-log.py` | The boot log written to userdata, the PMIC reset back to fastboot, and the USB log reader image |
| `package/CpuFreq.c` | CPU frequency of both clusters |
| `scripts/setup-gpu-userspace-tools.sh`, `scripts/fetch-gpu-userspace-crates.sh` | Host tools and Rust crates for the GPU user space; run them yourself |

Firmware extracted from the tablet (the touch firmware, `a630_sqe.fw`, the zap shader) and the platform seed stay in
`.cache/` and are never committed; `package/linux-log/build-log-reader.py --vendor-file` copies them off the tablet.

The UEFI checkout is not part of this repository. `uefi-tb128fu` is a copy of the Mu-Silicium tree with:

| File | Change |
| --- | --- |
| `Platforms/Lenovo/tb128fuPkg/tb128fu.dsc` | Divar SM6225 platform, UniXNU, UniXNUDxe, the ramdisk block device driver, DarwinBoot, UniXNULib and FdtLib |
| `Platforms/Lenovo/tb128fuPkg/tb128fu.fdf` | UniXNU and UniXNUDxe in the firmware volume; no vendor raw files |
| `Platforms/Lenovo/tb128fuPkg/Include/DXE.inc` | Mu drivers only, `ShengBootMemoryDxe` and DarwinBoot |
| `Platforms/Lenovo/tb128fuPkg/Library/MemoryMapLib` | The TB128FU reserved-memory table and the free DRAM runs |
| `Resources/Configs/tb128fu.toml` | FD base 0x5fc00000, size 3 MiB; header v1 boot image |
| `Resources/DTBs/tb128fu.dtb` | The merged Khaje IDP tree, with `/chosen/bootargs` and the stale initrd properties removed |

## Host tools

The unixnu driver needs Python 3.11 or newer and the tools of the [requirements page](https://unixnu.com/guide/requirements).
On this Mac these were installed for the pipa work:

```sh
brew install llvm lld cmake ninja mtools dtc
```

## Stage 1 to 3: firmware, menu and payload volume

```sh
export UNIXNU_UEFI=$PWD/uefi-tb128fu      # the preflight reads the checkout from here
cd UniXNU                                # cloned beside this repository
git submodule update --init --recursive

./unixnu fetch -D ../UniXNU-tb128fu
./unixnu build package -D ../UniXNU-tb128fu
./unixnu build firmware -D ../UniXNU-tb128fu
./unixnu bootimg -D ../UniXNU-tb128fu --dtb "$UNIXNU_UEFI/Resources/DTBs/tb128fu.dtb" --ramdisk none

fastboot boot out/tb128fu/boot/boot.img
```

`fastboot boot` writes nothing to the tablet. Expected: the Mu-Silicium boot log on the panel, then the UniXNU menu
with its built-in configuration. A firmware stall leaves the tablet in the bootloader; start the same image again.

## Stage 4 and 5: kernel entry and the root image

`configs/tb128fu_defconfig` builds the CPU, GIC, timer, the device-tree platform expert, the source corecrypto and
pthread providers, NativeAES, the NativeUfs SM6225 root with the owner policy, and the owner root chain of sheng
(root authorization, SystemOS cache mount, boot manifest, scratch Data mount). The kernel carries only the 395-byte
owner key; the signed OS/SystemOS envelopes live in the userdata image, and the Data seed is a packed APFS volume in
the payload.

```sh
export UNIXNU_IPSW=$PWD/out/ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw   # a local copy of [ipsw] url
./unixnu defconfig tb128fu_defconfig -D ../UniXNU-tb128fu
./unixnu ipsw fetch -D ../UniXNU-tb128fu
./unixnu build kernel -D ../UniXNU-tb128fu

# The Data seed: the initial /private/var tree of the OS image, packed and grown.
python3 scripts/prepare-owner-data-fixture.py --output build/owner-data-fixture
python3 - <<'EOF'
import sys; sys.path.insert(0, 'drivers/DarwinBoot/Test')
import packed_ramdisk
packed_ramdisk.pack('build/owner-data-fixture/owner-data-32.cdr',
                    'build/owner-data-fixture/owner-data-32.dbr')
EOF
python3 scripts/seed-data-grow.py --gib 8 build/owner-data-fixture/owner-data-32.dbr \
  build/owner-data-fixture/owner-data-8g.dbr
# The unchanged original static + System trust modules of the same IPSW.
python3 scripts/build-original-system-trust.py
./unixnu build payload -D ../UniXNU-tb128fu --unique-chip-id <16 hex digits> \
  --data-ramdisk build/owner-data-fixture/owner-data-8g.dbr --skip-setup \
  --trust-cache build/original-static-system-trust/original-static-system.bundle
./unixnu bootimg -D ../UniXNU-tb128fu --dtb "$UNIXNU_UEFI/Resources/DTBs/tb128fu.dtb"

# The root image: the unchanged OS and SystemOS disks plus the owner-signed envelopes.
./unixnu build userdata -D ../UniXNU-tb128fu \
  --root-policy OS=$PWD/out/tb128fu/tree/build/verified-root-os/manifest.bin \
  --root-policy SystemOS=$PWD/out/tb128fu/tree/build/verified-root-systemos/manifest.bin
fastboot flash userdata out/tb128fu/userdata/userdata-root.img
```

The Data seed's volume UUID is recorded in `configs/tb128fu_defconfig` as `XNU_OWNER_DATA_VOLUME_UUID`; changing the
seed means updating that value. `prepare-owner-data-fixture.py` needs the OS image mounted at
`.cache/firmware/target-volumes/os-mount` and creates `owner-data-32.cdr`; pack it with
`drivers/DarwinBoot/Test/packed_ramdisk.pack` before growing, or reuse an existing packed seed.
`build-original-system-trust.py` reads the IPSW from `refer/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw` and the original
static trust cache from `.cache/firmware/target-volumes/static-trustcache.bin`; both are local inputs and are not
committed.

The platform seed is any nonzero 64-bit value kept for this tablet, for example from `openssl rand -hex 8`;
`.cache/platform-seed` holds the one of this checkout and is not committed.

The volume policies are produced once by the UniXNU owner-root tools (`build/verified-root-os` and
`build/verified-root-systemos` in the build tree). `[kernel] archives` builds `build/verified-root-key/libVerifiedRootKey.a`
from the pinned owner key.

On a new tree the PMGR bridge has no reference kernel yet: the first `build kernel` links a no-op in its place and says
so, and the second links the bridge.

Expected: DarwinBoot hands over, the entry probe bars appear at the top of the screen, the kernel reads the policy
directory, publishes the OS/SystemOS block devices, authorizes the root, mounts the SystemOS cache and the Data
volume, and starts the first user process. `boot-progress=2` draws a live bring-up page (arm_vm_init, bsd_init, root
authentication, SystemOS and Data mount stages) on the frame buffer; without display and HID drivers the visible result
may still be that page or a frozen frame buffer.

## Boot-input preflight

`scripts/check-boot-inputs.py` reads the final images and refuses a mismatch before anything is flashed:

```sh
python3 scripts/check-boot-inputs.py \
  --trust-cache ../UniXNU/build/original-static-system-trust/original-static-system.bundle
```

It checks the SHNGROOT windows against the signed policy envelopes (RSA-3072 signature with the pinned
anchor, hash table, block count, signed image size and image hash), the real volume UUID of the OS
(`04769D26-…`) and SystemOS (`8846FE78-…`) policies, the Data seed UUID (`73F1FA24-…`), the kernel
symbols of the owner chain and boot progress, the 96 MiB boot limit and the trust-cache pin. This is the
check that catches a policy signed with the wrong volume identity, which would otherwise only fail on the
tablet as `rootvp not authenticated`.

## Values taken from the tablet

Read under Android 13 (TrebleDroid GSI, vendor 15.0.619) with `android-devinfo` on 2026-10-21. The live merged device
tree is the source of the memory and SoC values; `boot_a` is the stock Lenovo image.

| Value | Read | In this repository |
| --- | --- | --- |
| SoC | SM6225 (KHAJE / SM_DIVAR, soc_id 518, platform bengal) | `[board] soc = "SM6225"` |
| CPU | 4 × Cortex-A53 (reg 0x0..0x3) + 4 × Cortex-A73 (reg 0x100..0x103), ARMv8.0 | `make-tb128fu-adt.py`; only the boot CPU in the first ADT (`cpu_regs = "0"`) |
| GIC | GICv3 distributor 0x0f200000+0x10000, redistributor 0x0f300000+0x100000, stride 0x20000 | `make-tb128fu-adt.py`, `MemoryMapLib` |
| Timer | 19.2 MHz (DT `clock-frequency = 0x124f800`) | ADT timebase |
| DRAM | `/memory`: 0x40000000+0x3cc00000, 0x80000000+0x40000000, 0xc0000000+0x100000000; 5.95 GiB total, 0x7cc00000..0x80000000 is a hole | `dram_size = 0x180000000` (span to 0x1c0000000); `MemoryMapLib` exposes 0x80000000+0x140000000 |
| DarwinBoot window | the free 0x80000000..0x1c0000000 run | `owned_memory_size = 0x80000000` (2 GiB first; 4 GiB can be tried later) |
| userdata | sda18, LBA 0x222f31..0x1c457fa, 4096-byte blocks (104.5 GiB) | `[userdata]` |
| Storage | Samsung UFS 2.2 KM2L9001CM-B518, ufshc 0x04804000, 6 LUNs, GCC UFS 0x45000, apps SMMU 0x0c600000, stream 0x60 | `drivers/NativeUfs` (`--sm6225`), `GccUfs.h` |
| Boot image of the stock `boot` | header v2, page 4096, base 0, kernel 0x8000, ramdisk 0x1000000, tags 0x100, no dtb fields; no `vendor_boot`; slot `_a` | this repository uses header v1, base 0x80000000, kernel 0x80008000, ramdisk 0x81000000, tags 0x80000100, page 4096 |
| DTB | 18 DTBs appended to the stock boot image; `dtb_idx=8` is the Khaje SoC base and `dtbo_idx=12` is the Lenovo overlay; the merged live tree is "KHAJE IDP nopmi", board-id 0x10022 | `Resources/DTBs/tb128fu.dtb` is the merged live tree, sanitized |
| Panel | `mdss_dsi_hx83102p_boe_2k_video`, 1200×2000, 60 Hz, video mode; `cont_splash_region` 0x5c000000+0xf00000 | SimpleFbDxe 1200×2000×32; "Display Reserved" descriptor |
| Touch | Himax `himax,hxcommon`, SPI 0x04a80000, 10 MHz, IRQ GPIO 80, reset GPIO 86 | for a later touch driver |
| GPU | Adreno 610 v2 (chip ID 0x6010001), no GMU | |
| SPMI keys | PMIC arbiter `qcom,spmi@1c40000`, PM6125 | not wired: `ShengKeyInputDxe` reads sheng's 0x0c400000 arbiter and is not included |

## Known risks

- **No vendor drivers.** The stock ABL is a 32-bit ARM ELF wrapping one AArch64 `LinuxLoader` PE; `imagefv` holds only
  boot logos, and `xbl`/`uefisecapp` carry no usable 64-bit DXE modules. UFS, clocks, PMIC, charger, touch and the
  hardware keys are absent. If ABL blanks the panel when it hands over, the Mu-only firmware has no way to bring it
  back; the menu and panic screen would be invisible.
- **ABL fastboot buffer.** `fastboot boot` returned to fastboot for 192 MiB and 89 MiB images; a 57 MiB image failed
  with `Failed to load/authenticate boot image: Load Error`. The buffer is smaller than the 96 MiB `boot_a` partition,
  so the working path is `fastboot flash boot` + reboot. The payload is 56 MiB and holds the 49.6 MiB kernel, the ADT,
  the packed Data seed and the trust caches.
- **Owner root chain.** The root authorization, SystemOS cache mount, boot manifest and Data mount are the sheng
  ladder. They are new on this board; the SystemOS volume UUID is configured (`8846FE78-…`, the real APFS volume of
  the cryptex image). A failed authorization panics at `rootvp not authenticated`, not at a block read.
- **No console input.** The UniXNU menu has a 10 s timeout and boots its default entry, so the first test proceeds
  without keys. `Tb128fuKeyInputDxe` maps volume-up (TLMM GPIO 5) to Enter for the menu; the address has not been
  checked on hardware. A USB keyboard cannot work until a USB controller driver is present.
- **DarwinBoot's window.** `owned_memory_size = 2 GiB` must be found as one free run. MemoryMapLib publishes
  0x80000000..0x1c0000000 as one conventional region and the DXE heap stays below 0x60000000, but a fragmented map
  would produce `NO WINDOW`.
- **`sheng-pmgr=2` bridge.** The original-PMGR bridge is generic and accesses no hardware, but it has only been
  exercised on sheng and QEMU.
- **GIC spurious patch.** `Resources/MuPatches/Gic-Spurious.patch` is applied for the build; it was found on sheng with
  a leftover vendor interrupt. A Mu-only build may not need it.
- **ARMv8.0.** The CPU has no LSE atomics, FP16 or RCpc. A scan of the iPad7,1 kernel cache and the system libraries
  found no v8.1+ instructions outside dispatch paths that select on `ID_AA64ISAR0_EL1`; residual risk remains.
- **No ACPI tables.** XNU uses the device-tree platform expert; the firmware ships no ACPI tables for this board.

## Hardware bring-up notes

- `fastboot boot` sets `bootonce-bootloader` at the start of the `misc` partition. If the payload resets, ABL can
  re-enter fastboot and mark the active slot unbootable, switching to the other slot; the result is a reboot loop even
  though nothing was flashed. Recovery with the tablet in fastboot:

  ```sh
  fastboot --set-active=a
  fastboot flash boot_a boot_a-backup.img      # the Magisk-patched image that was running
  fastboot flash boot_b boot_a-backup.img
  fastboot reboot
  ```

  If it still loops, boot Android and zero the first 64 bytes of `/dev/block/by-name/misc` as root, then reboot. The
  boot-once command is only 32 bytes; the rest of the partition is left alone.
- NativeUfs SM6225 facts are in `drivers/NativeUfs/GccUfs.h` of the UniXNU checkout (GCC UFS block 0x45000, host
  0x04804000, apps SMMU 0x0c600000, UFS stream 0x60, userdata LBA 0x222f31..0x1c457fa). The full verified block class is
  linked through `build-arm-driver.py NativeUfs --sm6225`; `build-native-virtio-bindings.py` binds the original
  `IOBlockStorageDevice` symbols to the prelinked IOStorageFamily. The kernel reads `SHNGROOT` and `SHNGPOL1` from
  userdata, loads the signed OS/SystemOS envelopes into read-only kernel memory and publishes one block device per
  window. `native-ufs-root-noverify=1` (in `boot-root.img`) publishes the same windows without the policy check as a
  control. The QEMU root-path test (`drivers/NativeUfs/tests/check_qemu_root_commands.py`) covers the layout, policy
  directory, signature and block reads on the host.
- `boot-green.img`, the SEC-only green-screen diagnostic, is in `out/tb128fu/boot/stale/` with the other images of the
  first kernel. A green screen proves the panel is still scanning out and the frame-buffer address/geometry are usable.
- The first hardware boot reached the BDS and printed `GetPlatformBootGraphic Status: NotFound` (the Mu-only FV has no
  `BootLogo.bmp`; the message is benign). The platform boot manager only registered MsBootPolicy/USB/FFU/MassStorage,
  none of which are in this FV, so UniXNU was not in the boot order. `MsBootOptionsLib` now registers the UniXNU app
  (`9B665204-A133-47A2-9396-9E2B370A4E25`) first.
- `fastboot boot` returned to fastboot for 192 MiB and 89 MiB images; a 57 MiB image failed with
  `Failed to load/authenticate boot image: Load Error`. The ABL fastboot buffer is smaller than the 96 MiB `boot_a`
  partition. The working path is to flash the 57 MiB image to `boot_a` and reboot.

## Next steps

1. Build Turnip, metal2vulkan and the Metal plugin for the A610 and deliver them through the GPU addon, so QuartzCore
   and CoreImage draw on the GPU.
2. GPU frequency: CX/MX votes to the RPM over GLINK, then the levels above 320 MHz.
3. Buttons (SPMI PM6125), battery, audio and Wi-Fi.

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE). `make-tb128fu-adt.py` is derived from `make-sheng-adt.py` of
UniXNU-sheng and `make-pipa-adt.py`; `build-arm-driver.py` and the two panic-screen files are copies from it.
`ShengPlatformPower.cpp` comes from it under MIT (its SPDX header).
