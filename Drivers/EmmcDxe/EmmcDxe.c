/** @file
  EmmcDxe.c — Internal eMMC (SDMMC2) block-I/O driver for Nintendo Switch Erista

  The Nintendo Switch internal eMMC is connected to the Tegra210 SDMMC2
  controller at MMIO base 0x700b0200.  This driver is a separate binary from
  SdMmcDxe (SD card / SDMMC1).  Because each UEFI driver is an independent
  PE image with its own data segment, including the same low-level source
  files here gives this binary its own private copy of all MMC globals
  (mMmcInstance, mPriv, mBlkDesc, mConfig), configured for SDMMC2.

  Key differences from SdMmcDxe (SDMMC1 / microSD):
    - MMIO base  : 0x700b0200  (TEGRA_SDMMC2_BASE)
    - Peripheral : PERIPH_ID_SDMMC2
    - Bus width  : 8-bit (eMMC spec)
    - Voltages   : 1.8 V (MMC_VDD_165_195); eMMC is not 3.3 V removable media
    - Card detect: no GPIO detect — eMMC is soldered and always present
    - Media flags: non-removable, read-only until writes are implemented

  NOTE: The Switch eMMC contains Nintendo system partitions (PRODINFO, BCPKG,
  etc.) at the start of the raw USER DATA area.  Writing to the raw eMMC
  without care can destroy the Switch OS.  Boot payloads should be placed in
  a dedicated FAT32 partition at the end of the eMMC, or in a GPT layout that
  does not overlap Nintendo partitions.

  Copyright (c) 2024, NintendoSwitchPkg2 Contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <PiDxe.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseLib.h>
#include <Library/IoLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/DevicePath.h>

#include <Protocol/UBootClockManagement.h>
#include <Protocol/Utc/Clock.h>
#include <Protocol/Utc/ErrNo.h>
#include <Protocol/Utc/Mmc.h>
#include <Protocol/Utc/Tegra210/Clock-Tables.h>

#include <Foundation/Types.h>
#include <Device/NvAddressMap.h>
#include <Protocol/Pmic.h>
#include <Shim/DebugLib.h>
#include <Shim/UBootIo.h>
#include <Shim/TimerLib.h>

#include "../SdMmcDxe/Include/SdMmc.h"
#include "../SdMmcDxe/Include/HostOp.h"
#include "../SdMmcDxe/Include/EfiProto.h"

/*
 * The globals below are DEFINED in SdMmc.c (which is also compiled into this
 * binary via the INF).  Declare them here so EmmcDxe.c can configure them.
 */
extern TEGRA210_UBOOT_CLOCK_MANAGEMENT_PROTOCOL *mClkProtocol;
extern PMIC_PROTOCOL                            *mPmicProtocol;
extern MMC_CONFIG                                mConfig;
extern TEGRA_MMC_PRIV                            mPriv;
extern struct mmc                                mMmcInstance;
extern struct blk_desc                           mBlkDesc;
extern BOOLEAN                                   mForceMmcOnlyInit;

STATIC CONST EFI_GUID gEmmcBlockIoDevicePathGuid = {
  0xf7361c2a, 0x0968, 0x4ee6, { 0xa5, 0x74, 0x62, 0x48, 0x48, 0x1f, 0xec, 0xc3 }
};

/**
  Configure the SDMMC2 controller and the shared MMC globals for
  internal eMMC access.

  @retval EFI_SUCCESS       Controller probed and clock running.
  @retval EFI_DEVICE_ERROR  Clock rate configuration failed.
**/
STATIC EFI_STATUS
EmmcControllerProbe (
  VOID
  )
{
  int Ret;

  /* Zero all shared state — this binary's private copy, not SdMmcDxe's. */
  ZeroMem (&mBlkDesc,    sizeof (struct blk_desc));
  ZeroMem (&mMmcInstance, sizeof (struct mmc));
  ZeroMem (&mPriv,       sizeof (TEGRA_MMC_PRIV));
  ZeroMem (&mConfig,     sizeof (MMC_CONFIG));

  /*
   * eMMC voltage: 1.8 V (MMC_VDD_165_195).
   * 8-bit bus width for maximum throughput; HS 52 MHz mode.
   */
  mConfig.voltages = MMC_VDD_165_195;
  mConfig.host_caps = MMC_MODE_8BIT | MMC_MODE_HS_52MHz | MMC_MODE_HS;
  mConfig.f_min    = 375000;
  mConfig.f_max    = 48000000;
  mConfig.b_max    = CONFIG_SYS_MMC_MAX_BLK_COUNT;

  mMmcInstance.cfg   = &mConfig;
  mMmcInstance.clock = mConfig.f_min;
  mForceMmcOnlyInit = TRUE;

  /* SDMMC2 MMIO base — internal eMMC on Switch Erista. */
  mPriv.reg      = (VOID *)(UINTN)TEGRA_SDMMC2_BASE;
  mPriv.periph_id = PERIPH_ID_SDMMC2;

  /* Assert, enable, rate-set, de-assert clock for SDMMC2. */
  mClkProtocol->AssertRst   (PERIPH_ID_SDMMC2);
  mClkProtocol->EnableClk   (PERIPH_ID_SDMMC2);

  Ret = mClkProtocol->SetRate (PERIPH_ID_SDMMC2, 20000000);
  if (IS_ERR_VALUE (Ret)) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: SDMMC2 SetRate failed (%d)\n", Ret));
    return EFI_DEVICE_ERROR;
  }

  mClkProtocol->DeassertRst (PERIPH_ID_SDMMC2);

  /* eMMC has no card-detect pin — always present; no GPIO check needed. */
  DEBUG ((EFI_D_INFO, "EmmcDxe: SDMMC2 controller probed (base=0x%08x)\n",
          (UINT32)TEGRA_SDMMC2_BASE));
  return EFI_SUCCESS;
}

/**
  EmmcDxe driver entry point.

  Initialises the internal eMMC (SDMMC2), probes the medium, and installs
  an EFI_BLOCK_IO_PROTOCOL handle so the BDS / boot manager can discover
  and boot from the internal eMMC.

  @param[in]  ImageHandle   EFI image handle.
  @param[in]  SystemTable   Pointer to EFI system table.

  @retval EFI_SUCCESS         eMMC initialised and BlockIo installed.
  @retval EFI_NOT_FOUND       eMMC did not enumerate (no device present or
                              initialisation timed out).
  @retval EFI_DEVICE_ERROR    Low-level hardware initialisation failed.
  @retval other               Protocol install or memory allocation failed.
**/
EFI_STATUS
EFIAPI
EmmcDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS    Status;
  BIO_INSTANCE *Instance;
  INT32         Ret;

  /* Locate required protocols. */
  Status = gBS->LocateProtocol (
                  &gTegraUBootClockManagementProtocolGuid,
                  NULL,
                  (VOID **)&mClkProtocol
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: cannot locate ClockManagement: %r\n", Status));
    return Status;
  }

  Status = gBS->LocateProtocol (
                  &gPmicProtocolGuid,
                  NULL,
                  (VOID **)&mPmicProtocol
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: cannot locate Pmic: %r\n", Status));
    return Status;
  }

  /* Configure SDMMC2 hardware and shared globals for eMMC. */
  Status = EmmcControllerProbe ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: EmmcControllerProbe failed: %r\n", Status));
    return Status;
  }

  /* Software-reset the SDMMC2 controller and enable interrupts. */
  Status = TegraMmcInit ();
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: TegraMmcInit failed: %r\n", Status));
    return Status;
  }

  /* Run the eMMC identification / initialisation sequence. */
  Ret = SdFxInit ();
  if (Ret != 0) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: SdFxInit failed (%d)\n", Ret));
    return EFI_NOT_FOUND;
  }

  Ret = SdFxInitFinalize ();
  if (Ret != 0) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: SdFxInitFinalize failed (%d)\n", Ret));
    return EFI_NOT_FOUND;
  }

  if (mMmcInstance.has_init != 1) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: eMMC did not initialise (has_init=%d)\n",
            mMmcInstance.has_init));
    return EFI_NOT_FOUND;
  }

  /* Sanity-check the block descriptor populated by the MMC stack. */
  if (mBlkDesc.lba == 0 || mBlkDesc.blksz == 0) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: invalid block descriptor (lba=%llu blksz=%u)\n",
            (UINT64)mBlkDesc.lba, (UINT32)mBlkDesc.blksz));
    return EFI_DEVICE_ERROR;
  }

  DEBUG ((EFI_D_INFO,
          "EmmcDxe: eMMC ready — %llu blocks of %u bytes "
          "(capacity ~%llu MiB)\n",
          (UINT64)mBlkDesc.lba,
          (UINT32)mBlkDesc.blksz,
          (UINT64)mBlkDesc.lba * mBlkDesc.blksz / (1024 * 1024)));

  /* Allocate and populate the BlockIo instance. */
  Status = BioInstanceContructor (&Instance);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: BioInstanceContructor failed: %r\n", Status));
    return Status;
  }

  Status = BioConfigureInstance (
             Instance,
             (UINT32)mBlkDesc.blksz,
             (UINT64)mBlkDesc.lba,
             FALSE,
             TRUE,
             &gEmmcBlockIoDevicePathGuid
             );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: invalid BlockIo media geometry: %r\n", Status));
    FreePool (Instance);
    return Status;
  }

  /* Install BlockIo and DevicePath on a new handle. */
  Status = gBS->InstallMultipleProtocolInterfaces (
                  &Instance->Handle,
                  &gEfiBlockIoProtocolGuid,    &Instance->BlockIo,
                  &gEfiDevicePathProtocolGuid, &Instance->DevicePath,
                  NULL
                  );
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "EmmcDxe: InstallMultipleProtocolInterfaces failed: %r\n",
            Status));
    FreePool (Instance);
    return Status;
  }

  DEBUG ((EFI_D_INFO, "EmmcDxe: BlockIo installed for internal eMMC.\n"));
  return EFI_SUCCESS;
}
