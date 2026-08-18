/** @file
  ExperimentalOverclockDxe.c

  EXPERIMENTAL / HARDWARE-RISKY OVERCLOCKING SUPPORT FOR ERISTA (TEGRA210)

  Stock clocks remain the default. A non-stock profile requires both a
  build-time opt-in and a one-shot runtime variable, and this driver fails
  closed if it cannot validate safety prerequisites at runtime.

  Copyright (c) 2024, NintendoSwitchPkg2 Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Uefi.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>

#include "ExperimentalOverclock.h"

typedef struct {
  UINT32  CpuFreqKHz;
  UINT32  PllxM;
  UINT32  PllxN;
  UINT32  PllxP;
} OC_PLLX_PARAMS;

STATIC CONST OC_PLLX_PARAMS mPllxParams[OcProfileMax + 1] = {
  { OC_CPU_FREQ_STOCK_KHZ, 1, 53, 1 },
  { OC_CPU_FREQ_MILD_KHZ,  1, 40, 0 },
};

STATIC INLINE UINT32
OcMmioRead32 (
  IN UINTN  Base,
  IN UINTN  Offset
  )
{
  return MmioRead32 (Base + Offset);
}

STATIC INLINE VOID
OcMmioWrite32 (
  IN UINTN   Base,
  IN UINTN   Offset,
  IN UINT32  Value
  )
{
  MmioWrite32 (Base + Offset, Value);
}

STATIC
EFI_STATUS
OcValidateCpuFreq (
  IN UINT32  FreqKHz
  )
{
  if (FreqKHz > OC_CPU_FREQ_MAX_CEILING_KHZ) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: CPU frequency %u kHz exceeds ceiling %u kHz.\n",
      FreqKHz,
      OC_CPU_FREQ_MAX_CEILING_KHZ
      ));
    return EFI_INVALID_PARAMETER;
  }

  return EFI_SUCCESS;
}

STATIC
EFI_STATUS
OcProgramPllx (
  IN CONST OC_PLLX_PARAMS  *Params
  )
{
  UINT32  RegVal;
  UINTN   Timeout;

  if (Params == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  /*
   * Verified against Drivers/ClockManagementDxe/Tegra210/clock.c:
   * PLLX uses DIVM at [7:0], DIVN at [15:8], DIVP at [24:20], ENABLE at bit
   * 30, BYPASS at bit 31, LOCK at bit 27, and the output frequency is:
   *   Fout = Fin * N / (M * 2^P)
   */
  RegVal  = ((Params->PllxP & 0x1FU) << 20);
  RegVal |= ((Params->PllxN & 0xFFU) << 8);
  RegVal |= ((Params->PllxM & 0xFFU) << 0);
  RegVal |= PLLX_BASE_ENABLE;
  RegVal &= ~PLLX_BASE_BYPASS;

  OcMmioWrite32 (CLOCK_BASE_ADDR, CLK_RST_PLLX_BASE, RegVal);

  Timeout = 10000U;
  while (Timeout-- > 0) {
    if ((OcMmioRead32 (CLOCK_BASE_ADDR, CLK_RST_PLLX_BASE) & PLLX_BASE_LOCK) != 0) {
      return EFI_SUCCESS;
    }
  }

  DEBUG ((DEBUG_ERROR, "ExperimentalOC: PLLX lock timed out.\n"));
  return EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
OcValidateSafetyPrerequisites (
  VOID
  )
{
  DEBUG ((
    DEBUG_ERROR,
    "ExperimentalOC: Refusing non-stock profile because validated thermal "
    "sensor support is unavailable.\n"
    ));
  DEBUG ((
    DEBUG_ERROR,
    "ExperimentalOC: Refusing non-stock profile because runtime voltage/"
    "power validation support is unavailable.\n"
    ));
  return EFI_NOT_READY;
}

STATIC
EFI_STATUS
OcConsumeRequestedProfile (
  OUT OC_PROFILE_ID  *ProfileId
  )
{
  EFI_STATUS  Status;
  EFI_GUID    OcVarGuid = OC_VARIABLE_GUID;
  UINT8       ProfileByte;
  UINTN       VarSize;
  UINT32      Attributes;

  if (ProfileId == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  VarSize = sizeof (ProfileByte);
  Attributes = 0;
  Status = gRT->GetVariable (
                  OC_VARIABLE_NAME,
                  &OcVarGuid,
                  &Attributes,
                  &VarSize,
                  &ProfileByte
                  );
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = gRT->SetVariable (
                  OC_VARIABLE_NAME,
                  &OcVarGuid,
                  Attributes,
                  0,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Failed to consume one-shot variable: %r\n",
      Status
      ));
    return Status;
  }

  if (VarSize != sizeof (ProfileByte)) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Variable had unexpected size %u and was cleared.\n",
      (UINT32)VarSize
      ));
    return EFI_BAD_BUFFER_SIZE;
  }

  if (ProfileByte > (UINT8)OcProfileMax) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Variable requested unsupported profile %u and was cleared.\n",
      (UINT32)ProfileByte
      ));
    return EFI_INVALID_PARAMETER;
  }

  *ProfileId = (OC_PROFILE_ID)ProfileByte;
  return EFI_SUCCESS;
}

EFI_STATUS
OcCheckSiliconRevision (
  VOID
  )
{
  UINT32  SkuInfo;

  SkuInfo = OcMmioRead32 (FUSE_BASE_ADDR, FUSE_SKU_INFO_OFFSET);
  if ((SkuInfo & 0xFFU) != T210_SKU_ID_ERISTA) {
    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Unsupported silicon SKU=0x%02x.\n",
      (UINT32)(SkuInfo & 0xFFU)
      ));
    return EFI_UNSUPPORTED;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
OcRestoreStock (
  VOID
  )
{
  return OcProgramPllx (&mPllxParams[OcProfileStock]);
}

EFI_STATUS
OcApplyProfile (
  IN OC_PROFILE_ID  Profile
  )
{
  EFI_STATUS              Status;
  EFI_STATUS              RollbackStatus;
  CONST OC_PLLX_PARAMS   *Params;

  if ((UINTN)Profile > (UINTN)OcProfileMax) {
    return EFI_INVALID_PARAMETER;
  }

  Params = &mPllxParams[Profile];

  Status = OcValidateCpuFreq (Params->CpuFreqKHz);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = OcCheckSiliconRevision ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  if (Profile == OcProfileStock) {
    return OcRestoreStock ();
  }

  Status = OcValidateSafetyPrerequisites ();
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = OcProgramPllx (Params);
  if (EFI_ERROR (Status)) {
    RollbackStatus = OcRestoreStock ();
    if (EFI_ERROR (RollbackStatus)) {
      DEBUG ((
        DEBUG_ERROR,
        "ExperimentalOC: Profile apply failed (%r) and rollback also failed (%r).\n",
        Status,
        RollbackStatus
        ));
      return EFI_PROTOCOL_ERROR;
    }

    DEBUG ((
      DEBUG_ERROR,
      "ExperimentalOC: Profile apply failed (%r); stock PLLX restored.\n",
      Status
      ));
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
ExperimentalOverclockDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS    Status;
  OC_PROFILE_ID ProfileId;

  if (!FixedPcdGetBool (PcdExperimentalOverclockEnable)) {
    return EFI_SUCCESS;
  }

  Status = OcConsumeRequestedProfile (&ProfileId);
  if (EFI_ERROR (Status)) {
    if (Status != EFI_NOT_FOUND) {
      DEBUG ((DEBUG_ERROR, "ExperimentalOC: one-shot opt-in rejected: %r\n", Status));
    }
    return EFI_SUCCESS;
  }

  if (ProfileId == OcProfileStock) {
    DEBUG ((DEBUG_INFO, "ExperimentalOC: Consumed stock/no-op profile request.\n"));
    return EFI_SUCCESS;
  }

  Status = OcApplyProfile (ProfileId);
  if (EFI_ERROR (Status)) {
    DEBUG ((DEBUG_ERROR, "ExperimentalOC: fail-closed profile %u result: %r\n", (UINT32)ProfileId, Status));
    return EFI_SUCCESS;
  }

  return EFI_SUCCESS;
}
