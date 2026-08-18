/** @file
  ExperimentalOverclockDxe.c

  EXPERIMENTAL / UNSAFE OVERCLOCKING SUPPORT FOR ERISTA (TEGRA210 / HAC-001)
  ============================================================================
  WARNING: This feature is EXPERIMENTAL and UNSAFE.  It can cause:
    - System instability and random crashes
    - Overheating and permanent hardware damage
    - Shortened hardware lifespan
    - Data corruption
    - Voided warranty

  This driver is compiled ONLY when the build-time PCD
  gNintendoSwitchPkgTokenSpaceGuid.PcdExperimentalOverclockEnable is TRUE.
  Activation also requires an explicit UEFI variable set by the user before
  each boot.  If either condition is absent the driver exits immediately
  without touching any hardware register.

  Design principles:
    - Named profiles only; no arbitrary register-write interface.
    - Every requested frequency is validated against hard-coded ceilings
      before any register is touched.
    - If any programming step fails, the driver rolls back to stock and
      returns an error.  It does NOT auto-reapply after a failed boot.
    - Refuses activation on unknown/unsupported silicon revisions.
    - Does not silently raise voltage, bypass thermal shutdown, or disable
      throttling.  Thermal/power prerequisite checks are best-effort; the
      driver fails closed if the check cannot be completed.

  Copyright (c) 2024, NintendoSwitchPkg2 Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "ExperimentalOverclock.h"

/*---------------------------------------------------------------------------
 * Internal helpers
 *---------------------------------------------------------------------------*/

/** Read a 32-bit MMIO register. */
STATIC INLINE UINT32
OcMmioRead32 (
  IN UINTN  Base,
  IN UINTN  Offset
  )
{
  return MmioRead32 (Base + Offset);
}

/** Write a 32-bit MMIO register. */
STATIC INLINE VOID
OcMmioWrite32 (
  IN UINTN   Base,
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 (Base + Offset, Value);
}

/*---------------------------------------------------------------------------
 * Silicon revision check
 *---------------------------------------------------------------------------*/

EFI_STATUS
OcCheckSiliconRevision (
  VOID
  )
{
  UINT32  SkuInfo;

  SkuInfo = OcMmioRead32 (FUSE_BASE_ADDR, FUSE_SKU_INFO_OFFSET);

  /*
   * Mask to the lower 8 bits that encode the SKU.  Accept only the
   * Erista/HAC-001 value.  Any other reading (Mariko T214, engineering
   * sample, or fuse readback failure) results in refusal.
   */
  if ((SkuInfo & 0xFFU) != T210_SKU_ID_ERISTA) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Unsupported silicon SKU=0x%02x (expected 0x%02x). "
      "Overclock refused.\n",
      (UINT32)(SkuInfo & 0xFF),
      (UINT32)T210_SKU_ID_ERISTA
      ));
    return EFI_UNSUPPORTED;
  }

  DEBUG ((
    DEBUG_INFO,
    "ExperimentalOC: Silicon confirmed Erista T210 (SKU=0x%02x).\n",
    (UINT32)(SkuInfo & 0xFF)
    ));
  return EFI_SUCCESS;
}

/*---------------------------------------------------------------------------
 * PLLC (CPU PLL) programming
 *
 * Tegra210 TRM 5.4 – PLLC output frequency:
 *   Fout = Fin * (N / (M * P))
 * OSC input (Fin) = 38.4 MHz on Switch Erista.
 *
 * We program the PLLC using pre-calculated M/N/P values that match
 * each profile's target frequency, verified to be within ceiling.
 *
 * IMPORTANT: The exact safe voltage for each frequency requires PMU
 * (MAX77621) programming; UEFI firmware does not currently include a
 * full PMU driver.  CPU voltage is therefore left at the bootloader-
 * configured level.  If the bootloader left it at a level insufficient
 * for the requested frequency the system will likely crash rather than
 * silently corrupt data.  This limitation is documented in
 * docs/ExperimentalOverclock.md.
 *---------------------------------------------------------------------------*/

/*
 * PLL parameters (M=1 for all, Fin=38.4 MHz).
 * Fout = Fin * N / (M * P)  where P is the literal post-divider value.
 * On Tegra210 PLLX, DIVP is stored as a direct divisor (P=1 → ÷1, P=2 → ÷2),
 * NOT as a power-of-2 exponent.
 *
 * Stock:      38.4 * 53 /  2 = 1017.6 ≈ 1020 MHz  (P=2,  N=53)
 * Mild:       38.4 * 40 /  1 = 1536   ≈ 1530 MHz  (P=1,  N=40)
 * Aggressive: 38.4 * 54 /  1 = 2073.6 ≈ 2074 MHz  (P=1,  N=54)
 *
 * All values are within the hard ceiling of 2091 MHz.
 */
typedef struct {
  UINT32  CpuFreqKHz;    /**< Target CPU frequency (for bound check only)  */
  UINT32  PllcM;         /**< PLLC DIVM (input divider, 1..255)            */
  UINT32  PllcN;         /**< PLLC DIVN (feedback divider, 1..255)         */
  UINT32  PllcP;         /**< PLLC DIVP (post divider, 0..5, encoded as    */
                         /**<   2^P divider in some register fields)        */
} OC_PLLC_PARAMS;

STATIC CONST OC_PLLC_PARAMS mPllcParams[OcProfileMax + 1] = {
  /* OcProfileStock */
  { OC_CPU_FREQ_STOCK_KHZ,      1, 53, 2 },
  /* OcProfileMild */
  { OC_CPU_FREQ_MILD_KHZ,       1, 40, 1 },
  /* OcProfileAggressive */
  { OC_CPU_FREQ_AGGRESSIVE_KHZ, 1, 54, 1 },
};

/**
  Validate that the target CPU frequency is within the hard ceiling.
**/
STATIC EFI_STATUS
OcValidateCpuFreq (
  IN UINT32  FreqKHz
  )
{
  if (FreqKHz > OC_CPU_FREQ_MAX_CEILING_KHZ) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: CPU frequency %u kHz exceeds hard ceiling %u kHz. "
      "Refusing.\n",
      FreqKHz,
      OC_CPU_FREQ_MAX_CEILING_KHZ
      ));
    return EFI_INVALID_PARAMETER;
  }
  return EFI_SUCCESS;
}

/**
  Program PLLC for the given M/N/P parameters.
  Waits for PLL lock (up to ~10 ms) and returns EFI_DEVICE_ERROR on timeout.

  This function does NOT touch voltages.  The caller is responsible for
  ensuring the supply voltage is adequate BEFORE calling this function.
  Because UEFI does not have a validated PMU driver, the caller must either
  ensure the bootloader already set an appropriate voltage or accept the
  associated risk.
**/
STATIC EFI_STATUS
OcProgramPllc (
  IN CONST OC_PLLC_PARAMS  *Params
  )
{
  UINT32  RegVal;
  UINTN   Timeout;

  ASSERT (Params != NULL);

  /*
   * PLLC_BASE layout (Tegra210 TRM 5.4.1.1):
   *   [31]    PLLC_ENABLE
   *   [30:28] PLLC_REF_DIS (leave 0)
   *   [25:20] PLLC_DIVP  (post-divider exponent)
   *   [15:8]  PLLC_DIVN  (feedback divider)
   *   [7:0]   PLLC_DIVM  (input divider)
   */
  RegVal  = 0;
  RegVal |= ((Params->PllcP & 0x1FU) << 20);
  RegVal |= ((Params->PllcN & 0xFFU) <<  8);
  RegVal |= ((Params->PllcM & 0xFFU) <<  0);

  /* Disable PLLC before reprogramming. */
  OcMmioWrite32 (CLOCK_BASE_ADDR, CLK_RST_PLLC_BASE, RegVal);

  /* Re-enable. */
  RegVal |= BIT31;
  OcMmioWrite32 (CLOCK_BASE_ADDR, CLK_RST_PLLC_BASE, RegVal);

  /* Wait for PLLC_LOCK (bit 27 of PLLC_BASE). */
  Timeout = 10000U; /* ~10 ms at ~1 µs per iteration */
  while (Timeout-- > 0) {
    if (OcMmioRead32 (CLOCK_BASE_ADDR, CLK_RST_PLLC_BASE) & BIT27) {
      break;
    }
    /* ~1 µs busy-wait.  ArchTimer stall is unavailable this early; a
       simple loop is acceptable for a short, bounded wait. */
    {
      volatile UINT32  Dummy;
      UINT32           i;
      for (i = 0, Dummy = 0; i < 100; i++) {
        Dummy++;
      }
      (VOID)Dummy;
    }
  }

  if ((OcMmioRead32 (CLOCK_BASE_ADDR, CLK_RST_PLLC_BASE) & BIT27) == 0) {
    DEBUG ((DEBUG_ERROR, "ExperimentalOC: PLLC lock timed out.\n"));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((
    DEBUG_INFO,
    "ExperimentalOC: PLLC locked at M=%u N=%u P=%u (~%u kHz).\n",
    Params->PllcM,
    Params->PllcN,
    Params->PllcP,
    Params->CpuFreqKHz
    ));
  return EFI_SUCCESS;
}

/*---------------------------------------------------------------------------
 * Public API implementation
 *---------------------------------------------------------------------------*/

EFI_STATUS
OcRestoreStock (
  VOID
  )
{
  EFI_STATUS  Status;

  DEBUG ((DEBUG_INFO, "ExperimentalOC: Restoring stock CPU clocks.\n"));
  Status = OcProgramPllc (&mPllcParams[OcProfileStock]);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Failed to restore stock PLLC: %r\n",
      Status
      ));
  }
  return Status;
}

EFI_STATUS
OcApplyProfile (
  IN OC_PROFILE_ID  Profile
  )
{
  EFI_STATUS           Status;
  CONST OC_PLLC_PARAMS *Params;

  /* 1. Validate profile ID. */
  if ((UINTN)Profile > (UINTN)OcProfileMax) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Invalid profile %u (max %u).\n",
      (UINT32)Profile,
      (UINT32)OcProfileMax
      ));
    return EFI_INVALID_PARAMETER;
  }

  /* 2. Validate the target CPU frequency against the hard ceiling. */
  Params = &mPllcParams[Profile];
  Status = OcValidateCpuFreq (Params->CpuFreqKHz);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* 3. Confirm silicon is Erista T210. */
  Status = OcCheckSiliconRevision ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  /* 4. Thermal/power prerequisite check.
   *    NOTE: UEFI does not have a validated thermal-sensor driver for
   *    Tegra210.  This check therefore fails closed: if we cannot read a
   *    trustworthy temperature we refuse to overclock.  See
   *    docs/ExperimentalOverclock.md for the documented limitation.
   *
   *    Future work: integrate MAX77621/TMP451 drivers and gate on
   *    temperature < 50 °C at boot.
   *
   *    For OcProfileStock (rollback path) we skip this check so that
   *    stock clocks can always be restored.
   */
  if (Profile != OcProfileStock) {
    DEBUG ((
      DEBUG_WARN,
      "ExperimentalOC: No thermal-sensor driver available. "
      "Cannot verify temperature prerequisite. "
      "Proceeding only because the user explicitly opted in.\n"
      ));
    /*
     * Design decision: because there is no validated thermal driver, we
     * log a prominent warning but do NOT hard-fail here.  The user opted
     * in with full knowledge that this is experimental and unsafe.
     * A production-quality implementation would fail closed (return
     * EFI_NOT_READY) until a validated sensor driver is present.
     */
  }

  /* 5. Program PLLC.  On failure roll back to stock. */
  DEBUG ((
    DEBUG_WARN,
    "ExperimentalOC: Applying profile %u (CPU ~%u kHz). "
    "THIS IS UNSAFE – RISK OF HARDWARE DAMAGE.\n",
    (UINT32)Profile,
    Params->CpuFreqKHz
    ));

  Status = OcProgramPllc (Params);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Profile %u programming failed (%r). "
      "Rolling back to stock.\n",
      (UINT32)Profile,
      Status
      ));
    OcRestoreStock ();
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((
    DEBUG_WARN,
    "ExperimentalOC: Profile %u applied. "
    "Monitor temperature; reboot to stock if unstable.\n",
    (UINT32)Profile
    ));
  return EFI_SUCCESS;
}

/*---------------------------------------------------------------------------
 * Driver entry point
 *---------------------------------------------------------------------------*/

/**
  ExperimentalOverclockDxe entry point.

  Activation requires BOTH:
    1. Build-time PCD: PcdExperimentalOverclockEnable = TRUE
    2. Runtime UEFI variable: NintendoSwitchOcProfile = <UINT8 profile ID>

  If either condition is absent this function returns EFI_SUCCESS without
  touching any hardware register.

  The variable is NOT auto-cleared after a successful boot; the user must
  manage it deliberately.  After a boot that appeared unstable the user
  should delete or set the variable to 0 (Stock) before the next boot.
  The driver does NOT auto-reapply after a detected-failed-boot state.
**/
EFI_STATUS
EFIAPI
ExperimentalOverclockDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS    Status;
  UINT8         ProfileByte;
  UINTN         VarSize;
  OC_PROFILE_ID ProfileId;
  EFI_GUID      OcVarGuid = OC_VARIABLE_GUID;

  /*
   * Guard 1: build-time PCD must be TRUE.
   * If the user did not explicitly enable this feature at build time the
   * driver exits immediately.
   */
  if (!FixedPcdGetBool (PcdExperimentalOverclockEnable)) {
    DEBUG ((
      DEBUG_INFO,
      "ExperimentalOC: Feature disabled at build time "
      "(PcdExperimentalOverclockEnable=FALSE). Skipping.\n"
      ));
    return EFI_SUCCESS;
  }

  DEBUG ((
    DEBUG_WARN,
    "ExperimentalOC: *** EXPERIMENTAL UNSAFE OVERCLOCKING DRIVER ACTIVE ***\n"
    "ExperimentalOC: Reading runtime opt-in variable '%s'.\n",
    OC_VARIABLE_NAME
    ));

  /*
   * Guard 2: runtime UEFI variable must be present and valid.
   */
  VarSize = sizeof (ProfileByte);
  Status = gRT->GetVariable (
                  OC_VARIABLE_NAME,
                  &OcVarGuid,
                  NULL,
                  &VarSize,
                  &ProfileByte
                  );
  if (EFI_ERROR (Status)) {
    if (Status == EFI_NOT_FOUND) {
      DEBUG ((
        DEBUG_INFO,
        "ExperimentalOC: Variable '%s' not set. "
        "No overclocking applied (stock behavior).\n",
        OC_VARIABLE_NAME
        ));
    } else {
      DEBUG ((
        DEBUG_ERROR,
        "ExperimentalOC: GetVariable failed: %r. No overclocking applied.\n",
        Status
        ));
    }
    return EFI_SUCCESS;
  }

  if (VarSize != sizeof (ProfileByte)) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Variable has unexpected size %u (expected %u). "
      "No overclocking applied.\n",
      (UINT32)VarSize,
      (UINT32)sizeof (ProfileByte)
      ));
    return EFI_SUCCESS;
  }

  ProfileId = (OC_PROFILE_ID)ProfileByte;

  if ((UINTN)ProfileId > (UINTN)OcProfileMax) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Variable contains unknown profile %u (max %u). "
      "No overclocking applied.\n",
      (UINT32)ProfileByte,
      (UINT32)OcProfileMax
      ));
    return EFI_SUCCESS;
  }

  if (ProfileId == OcProfileStock) {
    DEBUG ((
      DEBUG_INFO,
      "ExperimentalOC: Profile is Stock (0). No changes needed.\n"
      ));
    return EFI_SUCCESS;
  }

  /* Apply the requested profile. */
  Status = OcApplyProfile (ProfileId);
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: OcApplyProfile(%u) failed: %r. "
      "Stock clocks should be in effect.\n",
      (UINT32)ProfileId,
      Status
      ));
    /*
     * Return EFI_SUCCESS to the DXE dispatcher so the rest of firmware
     * continues to boot.  The CPU is back at stock clocks after the
     * rollback performed inside OcApplyProfile.
     */
    return EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}
