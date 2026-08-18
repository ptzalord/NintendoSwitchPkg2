/** @file
  ExperimentalOverclock.h

  EXPERIMENTAL / UNSAFE OVERCLOCKING SUPPORT FOR ERISTA (TEGRA210 / HAC-001)
  ============================================================================
  WARNING: This feature is EXPERIMENTAL and UNSAFE.  It can cause:
    - System instability and random crashes
    - Overheating and permanent hardware damage
    - Shortened hardware lifespan
    - Data corruption
    - Voided warranty

  This header is ONLY compiled when PcdExperimentalOverclockEnable is TRUE.
  It must NEVER be activated by merely building or booting the firmware.
  A separate, explicit runtime UEFI variable opt-in is required in addition
  to the build-time PCD.

  Copyright (c) 2024, NintendoSwitchPkg2 Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef EXPERIMENTAL_OVERCLOCK_H_
#define EXPERIMENTAL_OVERCLOCK_H_

#include <Uefi.h>
#include <Library/BaseLib.h>

/*---------------------------------------------------------------------------
 * Silicon / board revision constants
 *---------------------------------------------------------------------------*/

/*
 * Tegra FUSE registers – used for silicon identification.
 * Base address from Tegra210 TRM Section 9.
 */
#define FUSE_BASE_ADDR        0x7000F800UL
#define FUSE_SKU_INFO_OFFSET  0x010       /* FUSE_SKU_INFO */
#define FUSE_OPT_FT_REV_OFFSET 0x028     /* FUSE_OPT_FT_REV (stepping) */

/*
 * Known-good FUSE_SKU_INFO values for Tegra210 Erista (HAC-001).
 * Any other value causes the driver to refuse activation.
 */
#define T210_SKU_ID_ERISTA    0x83U

/*---------------------------------------------------------------------------
 * PLLC (CPU PLL) / PLLM (Memory) / PLLP (Peripheral) registers
 *
 * All register offsets are relative to CLOCK_BASE (0x60006000).
 * See Tegra210 TRM Chapter 5 (Clock and Reset Controller).
 *---------------------------------------------------------------------------*/
#define CLOCK_BASE_ADDR       0x60006000UL

/* PLLC */
#define CLK_RST_PLLC_BASE     0x080       /* PLLC_BASE */
#define CLK_RST_PLLC_MISC     0x088       /* PLLC_MISC */

/* CCLKG (cluster clock gate) */
#define CLK_RST_CCLKG_BURST_POLICY 0x368  /* CCLKG_BURST_POLICY */
#define CLK_RST_CCLKG_OVERRIDE     0x36C  /* CCLKG_OVERRIDE */
#define CLK_RST_CCLKG_CPU_CSR      0x370  /* CPU_CMPLX_STATUS */

/* GPCPLL (GPU PLL – actually exposed through NV-private registers) */
/* NOTE: The GPU PLL on Tegra210 is GPCPLL inside the GPU clock domain.
   Direct MMIO access from firmware is not reliable without a full NV driver;
   we therefore keep GPU overclocking out of scope for the initial
   implementation and document that limitation.  The reserved slots below
   remain for future use only. */

/*---------------------------------------------------------------------------
 * Hard absolute frequency/voltage ceilings (compile-time enforced)
 * All values in kHz unless otherwise noted.
 *---------------------------------------------------------------------------*/

/* CPU (Denver + A57 cluster) – Tegra210 datasheet max is ~2.1 GHz on binned
 * silicon; we cap at 2.091 GHz.  Values beyond this ceiling
 * are REJECTED unconditionally by the driver, even if a caller supplies them.
 */
#define OC_CPU_FREQ_STOCK_KHZ    1020000UL   /* 1.020 GHz – factory default   */
#define OC_CPU_FREQ_MILD_KHZ     1530000UL   /* 1.530 GHz – Mild OC profile    */
#define OC_CPU_FREQ_AGGRESSIVE_KHZ 2073600UL /* 2.0736 GHz – Aggressive profile */
#define OC_CPU_FREQ_MAX_CEILING_KHZ 2091000UL /* HARD CEILING – never exceed   */

/* GPU – direct PLLC/GPCPLL programming from UEFI is not currently
 * implemented (see note above).  These constants document the intended
 * future limits only. */
#define OC_GPU_FREQ_STOCK_KHZ    307200UL    /* 307.2 MHz – factory default   */
#define OC_GPU_FREQ_MAX_CEILING_KHZ 998000UL /* HARD CEILING (future)         */

/*---------------------------------------------------------------------------
 * Profile identifiers
 *---------------------------------------------------------------------------*/
typedef enum {
  OcProfileStock      = 0,  /**< Factory clocks – always safe to apply.      */
  OcProfileMild       = 1,  /**< CPU 1.530 GHz.  Moderate risk.              */
  OcProfileAggressive = 2,  /**< CPU 1.785 GHz.  High risk of instability.   */
  OcProfileMax        = OcProfileAggressive
} OC_PROFILE_ID;

/*---------------------------------------------------------------------------
 * UEFI Variable used for runtime opt-in
 *
 * The firmware reads this variable during ExperimentalOverclockDxeInitialize.
 * If it is absent or has an unrecognised value the driver exits without
 * touching any hardware register.
 *
 * Variable format: single UINT8 matching OC_PROFILE_ID.
 * Variable must be set by the user before the next boot; it is not
 * auto-created or auto-reapplied after a failed boot.
 *---------------------------------------------------------------------------*/
#define OC_VARIABLE_NAME    L"NintendoSwitchOcProfile"
#define OC_VARIABLE_GUID    { 0x1900628e, 0x0a8a, 0x4099, { 0x8d, 0xe5, 0xf2, 0x08, 0xff, 0x80, 0xc4, 0xbf } }

/*---------------------------------------------------------------------------
 * Public API – only visible within this driver
 *---------------------------------------------------------------------------*/

/**
  Read the silicon FUSE registers and confirm the running chip is a
  Tegra210 Erista with a known SKU.  Returns EFI_UNSUPPORTED on any
  unrecognised silicon.

  @retval EFI_SUCCESS        Chip confirmed as Erista T210.
  @retval EFI_UNSUPPORTED    Unknown or unsupported silicon detected.
**/
EFI_STATUS
OcCheckSiliconRevision (
  VOID
  );

/**
  Apply the named frequency profile.  Performs full validation of the
  profile ID and every intermediate frequency value against the hard
  ceilings before touching any hardware register.  On any failure the
  function rolls back to stock frequencies before returning.

  @param[in]  Profile   Profile to apply (OcProfileStock, OcProfileMild, or
                        OcProfileAggressive).

  @retval EFI_SUCCESS           Profile applied successfully.
  @retval EFI_INVALID_PARAMETER Profile ID is out of range.
  @retval EFI_UNSUPPORTED       Running on unsupported silicon.
  @retval EFI_DEVICE_ERROR      Hardware programming failed; stock settings
                                restored.
**/
EFI_STATUS
OcApplyProfile (
  IN OC_PROFILE_ID  Profile
  );

/**
  Restore stock (factory) clock frequencies.  Safe to call at any time.
  Does not require a prior successful OcApplyProfile call.

  @retval EFI_SUCCESS        Stock clocks restored.
  @retval EFI_DEVICE_ERROR   Hardware communication failure.
**/
EFI_STATUS
OcRestoreStock (
  VOID
  );

#endif /* EXPERIMENTAL_OVERCLOCK_H_ */
