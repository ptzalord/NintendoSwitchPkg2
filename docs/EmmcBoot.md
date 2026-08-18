# Booting from Internal eMMC (SDMMC2)

This document describes the firmware support for the Nintendo Switch internal
eMMC storage and how to prepare the device for booting from it.

---

## Hardware overview

| Property | Value |
|---|---|
| Controller | Tegra210 SDMMC2 |
| MMIO base | `0x700b0200` |
| Clock peripheral | `PERIPH_ID_SDMMC2` |
| Bus width | 8-bit (eMMC spec) |
| Voltage | 1.8 V |
| Card detect | None — eMMC is soldered in, always present |
| Removable | No |

The microSD card uses **SDMMC1** (`0x700b0000`). The two controllers are
completely independent and registered as separate `EFI_BLOCK_IO_PROTOCOL`
handles. The UEFI boot manager can see and boot from both simultaneously.

---

## Driver: EmmcDxe

`Drivers/EmmcDxe/EmmcDxe.inf` is built unconditionally and included in the
default firmware image. It reuses the low-level MMC source files from
`SdMmcDxe` but compiles them into a separate PE binary with its own private
global state, so the SD card and eMMC paths cannot interfere with each other
at runtime.

The driver:
1. Asserts reset on SDMMC2, enables the clock, sets rate to 20 MHz, de-asserts reset.
2. Software-resets the SDHCI controller and enables interrupts.
3. Runs the eMMC identification/initialisation sequence (CMD0 → CMD1 → CMD2 → CMD3 → CMD9 → CMD7 → ACMD6/CMD6 for bus-width negotiation).
4. Logs the device capacity on success.
5. Installs `EFI_BLOCK_IO_PROTOCOL` + `EFI_DEVICE_PATH_PROTOCOL` on a new handle.

---

## eMMC partition layout (Switch Erista)

> **⚠ WARNING**: The raw eMMC USER DATA area begins at LBA 0 and contains
> Nintendo system partitions at its start.  Writing to these partitions can
> **permanently brick the console** or destroy the Nintendo OS.

Typical Erista eMMC layout (approximate, may vary by firmware version):

| Partition | Approx. location | Contents |
|-----------|-----------------|----------|
| PRODINFO | raw, early LBAs | Console-unique cryptographic keys |
| PRODINFOF | raw | Backup of PRODINFO |
| BCPKG2-1-Normal-Main | raw | Boot package (TrustZone, CBoot) |
| … | … | Other Nintendo system partitions |
| USER | GPT partition | Horizon OS user data, settings |
| **EFI System Partition** | GPT partition (user-managed) | Your bootloader / UEFI payloads |

To boot custom firmware, add a **FAT32 EFI System Partition** (GPT type
`C12A7328-F81F-11D2-BA4B-00A0C93EC93B`) that does **not** overlap any of the
raw Nintendo partitions.  Tools such as `gdisk` or the Hekate payload manager
can create this layout safely.

---

## Boot flow

1. CBoot (Nintendo bootloader, in BCPKG) hands off to TrustZone + UEFI.
2. UEFI DXE dispatcher loads `EmmcDxe.efi`, which installs the eMMC BlockIo.
3. BDS (boot manager) scans all BlockIo handles for EFI System Partitions.
4. The boot manager tries entries in the EFI boot order (stored in UEFI
   variables).  If no entries exist it falls back to `\EFI\BOOT\BOOTAA64.EFI`
   on each ESP it finds.
5. Your bootloader (e.g., GRUB, systemd-boot, or a chainloader) takes over.

---

## Preparing the eMMC for UEFI boot

> These steps require an existing working SD card boot environment or a
> Linux system with raw eMMC access through another method.

1. **Back up PRODINFO** before any partition changes:
   ```
   dd if=/dev/mmcblk0 of=prodinfo.bin bs=512 count=<size>
   ```
   (Use Hekate or another tool that knows the exact partition offsets.)

2. **Create a GPT ESP** at the end of the eMMC (after Nintendo partitions).
   Recommended size: ≥ 256 MiB.

3. **Format** the new partition as FAT32.

4. **Copy** your UEFI payload to `\EFI\BOOT\BOOTAA64.EFI` on the ESP.

5. **Add a UEFI boot entry** from a UEFI Shell or your existing OS:
   ```
   bcfg boot add 0 <device_path>\EFI\BOOT\BOOTAA64.EFI "Internal eMMC Boot"
   ```

6. Reboot.  UEFI will find the ESP on eMMC and launch your payload.

---

## Troubleshooting

| Symptom | Likely cause | Action |
|---------|-------------|--------|
| `EmmcDxe: SdFxInit failed` | eMMC did not respond | Check clock/voltage config; ensure SDMMC2 is not held in reset by TrustZone |
| eMMC BlockIo not visible in shell | Driver did not load | Check UEFI shell `drivers` and `devices` output |
| Booting from SD card instead of eMMC | Boot order | Use `bcfg` to set eMMC entry first, or remove SD card |
| Nintendo partitions corrupted | Wrote to wrong LBAs | Restore from PRODINFO backup |

---

## Validation status

| Test | Status |
|------|--------|
| EmmcDxe compiles without errors | ✅ Build-time (requires full EDK2 toolchain) |
| SDMMC2 clock/reset sequence | ❌ Not validated on real hardware |
| eMMC identification (CMD0/1/2/3) | ❌ Not validated on real hardware |
| Block read from FAT32 ESP | ❌ Not validated on real hardware |
| Boot from BOOTAA64.EFI on eMMC ESP | ❌ Not validated on real hardware |

**Do not assume hardware validation has been performed.**
