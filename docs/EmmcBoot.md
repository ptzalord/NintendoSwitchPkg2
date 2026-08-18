# Internal eMMC boot support (SDMMC2)

This repository now includes an `EmmcDxe` driver for the Nintendo Switch
internal eMMC on **SDMMC2**, but the path is currently **read-only** and
**not hardware-validated**.

## Current status

| Property | Status |
|---|---|
| Controller | Tegra210 SDMMC2 |
| Media type | Internal eMMC |
| Block I/O writes | **Unsupported** |
| Reported medium state | **Read-only** |
| Hardware validation | **Not performed** |
| Safe for production flashing/booting | **No** |

`EmmcDxe` and `SdMmcDxe` use separate binaries, separate controller setup, and
distinct device-path GUIDs so SDMMC1 and SDMMC2 state stay isolated.

## What this driver does today

- Initializes the SDMMC2 controller for MMC/eMMC protocol rather than SD-only
  ACMD flow.
- Reads and validates `EXT_CSD` data needed for capacity, partition metadata,
  timing capability discovery, and bus-width negotiation.
- Exposes the internal eMMC through `EFI_BLOCK_IO_PROTOCOL` as
  **non-removable, read-only media**.

## Important limitations

- **Writes are not implemented or validated.** Do not use this firmware to
  repartition, reformat, or write the internal eMMC.
- **Host-side CI is not hardware validation.** Python tests only check source
  logic and constants.
- **Real Erista validation is still required** before anyone should rely on
  this path for boot or storage access.

## Backup and safety guidance

Do **not** use raw `dd` commands from this document for internal NAND backups.
Use established Nintendo Switch NAND/eMMC backup tooling such as Hekate or an
equivalent workflow that understands the platform layout, and verify the backup
before making any storage changes elsewhere.

## Validation status

| Check | Result |
|---|---|
| Source builds/tests in host-side CI | Partial host-only coverage |
| Full firmware build | Must succeed locally before any flashing |
| Controlled Erista hardware boot/read testing | **Not performed** |

## Do not proceed yet

No user should flash firmware or enable internal eMMC boot paths from this
repository until:

1. a full firmware build succeeds from source, and
2. controlled validation is performed on expendable **Erista** hardware.
