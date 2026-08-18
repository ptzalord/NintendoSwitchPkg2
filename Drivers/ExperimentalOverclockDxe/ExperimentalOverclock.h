/** @file
  ExperimentalOverclock.h

  EXPERIMENTAL / HARDWARE-RISKY OVERCLOCKING SUPPORT FOR ERISTA (TEGRA210)

  This header is only compiled when
  gNintendoSwitchPkgTokenSpaceGuid.PcdExperimentalOverclockEnable is TRUE.
  Runtime activation still requires a separate one-shot UEFI variable opt-in.

  Copyright (c) 2024, NintendoSwitchPkg2 Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef EXPERIMENTAL_OVERCLOCK_H_
#define EXPERIMENTAL_OVERCLOCK_H_

#include <Uefi.h>
#include <Library/BaseLib.h>

#define FUSE_BASE_ADDR          0x7000F800UL
#define FUSE_SKU_INFO_OFFSET    0x010
#define T210_SKU_ID_ERISTA      0x83U

/*
 * Tegra210 CPU clocks are driven by PLLX, not PLLC.
 * The register layout below matches the Tegra210 clock code already present in
 * this repository under Drivers/ClockManagementDxe/Tegra210/clock.c.
 */
#define CLOCK_BASE_ADDR         0x60006000UL
#define CLK_RST_PLLX_BASE       0x0E0
#define PLLX_BASE_BYPASS        BIT31
#define PLLX_BASE_ENABLE        BIT30
#define PLLX_BASE_LOCK          BIT27

#define OC_CPU_FREQ_STOCK_KHZ        1020000UL
#define OC_CPU_FREQ_MILD_KHZ         1530000UL
#define OC_CPU_FREQ_MAX_CEILING_KHZ  1530000UL

typedef enum {
  OcProfileStock = 0,
  OcProfileMild  = 1,
  OcProfileMax   = OcProfileMild
} OC_PROFILE_ID;

/*
 * One-shot runtime opt-in. The driver consumes and deletes the variable before
 * applying any non-stock profile, so a crash cannot silently reapply it.
 */
#define OC_VARIABLE_NAME    L"NintendoSwitchOcProfile"
#define OC_VARIABLE_GUID    { 0x1900628e, 0x0a8a, 0x4099, { 0x8d, 0xe5, 0xf2, 0x08, 0xff, 0x80, 0xc4, 0xbf } }

EFI_STATUS
OcCheckSiliconRevision (
  VOID
  );

EFI_STATUS
OcApplyProfile (
  IN OC_PROFILE_ID  Profile
  );

EFI_STATUS
OcRestoreStock (
  VOID
  );

#endif /* EXPERIMENTAL_OVERCLOCK_H_ */
