#include <PiDxe.h>
#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/DebugLib.h>
#include <Library/PcdLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/DxeServicesTableLib.h>
#include <Library/BaseLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/DevicePathLib.h>
#include <Library/IoLib.h>
#include <Protocol/BlockIo.h>
#include <Protocol/BlockIo2.h>
#include <Protocol/DevicePath.h>
#include <Device/StaticDevices.h>
#include <Foundation/Types.h>

#include "Include/SdMmc.h"
#include "Include/HostOp.h"
#include "Include/EfiProto.h"

STATIC BIO_INSTANCE mBioTemplate = {
    BIO_INSTANCE_SIGNATURE,
    NULL, // Handle
    { 
        // BlockIo
        EFI_BLOCK_IO_INTERFACE_REVISION,   // Revision
        NULL,                              // *Media
        MMCHSReset,                        // Reset
        MMCHSReadBlocks,                   // ReadBlocks
        MMCHSWriteBlocks,                  // WriteBlocks
        MMCHSFlushBlocks                   // FlushBlocks
    },
    { 
        // BlockMedia
        BIO_INSTANCE_SIGNATURE,                   // MediaId
        TRUE,                                     // RemovableMedia
        TRUE,                                     // MediaPresent
        FALSE,                                    // LogicalPartition
        TRUE,                                     // ReadOnly
        FALSE,                                    // WriteCaching
        0,                                        // BlockSize
        4,                                        // IoAlign
        0,                                        // Pad
        0                                         // LastBlock
    },
    {   // DevicePath
        {
            {
                HARDWARE_DEVICE_PATH, HW_VENDOR_DP,
                { (UINT8) (sizeof(VENDOR_DEVICE_PATH)), (UINT8) ((sizeof(VENDOR_DEVICE_PATH)) >> 8) },
            },
            // Hardware Device Path for Bio
            EFI_CALLER_ID_GUID // Use the driver's GUID
        },
        {
            END_DEVICE_PATH_TYPE,
            END_ENTIRE_DEVICE_PATH_SUBTYPE,
            { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0 }
        }
    }
};

EFI_STATUS
EFIAPI
MMCHSReset(
    IN EFI_BLOCK_IO_PROTOCOL          *This,
    IN BOOLEAN                        ExtendedVerification
)
{
    return EFI_SUCCESS;
}

EFI_STATUS
BioConfigureInstance (
    IN OUT BIO_INSTANCE   *Instance,
    IN UINT32             BlockSize,
    IN UINT64             BlockCount,
    IN BOOLEAN            RemovableMedia,
    IN BOOLEAN            ReadOnly,
    IN CONST EFI_GUID     *DevicePathGuid
)
{
    if ((Instance == NULL) || (DevicePathGuid == NULL) || (BlockSize == 0) || (BlockCount == 0)) {
        return EFI_INVALID_PARAMETER;
    }

    Instance->BlockMedia.BlockSize = BlockSize;
    Instance->BlockMedia.LastBlock = (EFI_LBA)(BlockCount - 1);
    Instance->BlockMedia.RemovableMedia = RemovableMedia;
    Instance->BlockMedia.ReadOnly = ReadOnly;
    CopyMem (&Instance->DevicePath.Mmc.Guid, DevicePathGuid, sizeof (*DevicePathGuid));

    return EFI_SUCCESS;
}

/*
 * Function: MmcReadInternal
 * Arg     : Data address on card, o/p buffer & data length
 * Return  : 0 on Success, non zero on failure
 * Flow    : Read data from the card to out
 */
STATIC UINT32 MmcReadInternal
(
    BIO_INSTANCE *Instance, 
    UINT64 DataAddr, 
    UINT32 *Buf, 
    UINT32 DataLen
)
{
    UINT32 Ret = 0;
    UINT32 BlockSize = Instance->BlockMedia.BlockSize;
    /* Operation block size. Will decrement until reaches 1 */
    UINT32 OpBlkSize = 64;
    UINT32 ReadSize;
    UINT8 *Sptr = (UINT8 *) Buf;

    if ((BlockSize == 0) || ((DataAddr % BlockSize) != 0) || ((DataLen % BlockSize) != 0)) {
        return 0;
    }

    /*
    * DMA onto write back memory is unsafe/nonportable,
    * but callers to this routine normally provide
    * write back buffers. Invalidate cache
    * before read data from MMC.
    */
    WriteBackInvalidateDataCacheRange(Buf, DataLen);

    /* TODO: This function is aware of max data that can be
    * tranferred using sdhci adma mode, need to have a cleaner
    * implementation to keep this function independent of sdhci
    * limitations
    */

    /* Set size */
    ReadSize = BlockSize * OpBlkSize;

read:
    while (DataLen > ReadSize) 
    {
        Ret = mmc_bread((DataAddr / BlockSize), (ReadSize / BlockSize), (VOID *) Sptr);
        if (Ret == 0)
        {
            // Attempt to reduce the block size
            if (OpBlkSize > 1)
            {
                // Decrement op size
                OpBlkSize = OpBlkSize / 2;
                ReadSize = BlockSize * OpBlkSize;
                goto read;
            }
            else
            {
                DEBUG((EFI_D_ERROR, "Failed Reading block @ %x\n",(UINTN) (DataAddr / BlockSize)));
                goto fail;
            }
        }
        Sptr += ReadSize;
        DataAddr += ReadSize;
        DataLen -= ReadSize;
    }

    if (DataLen)
    {
        Ret = mmc_bread((DataAddr / BlockSize), (DataLen / BlockSize), (VOID *) Sptr);
        if (Ret == 0)
        {
            // Attempt to reduce the block size
            if (OpBlkSize > 1)
            {
                // Decrement op size
                OpBlkSize = OpBlkSize / 2;
                ReadSize = BlockSize * OpBlkSize;
                goto read;
            }
            else
            {
                DEBUG((EFI_D_ERROR, "Failed Reading block @ %x\n",(UINTN) (DataAddr / BlockSize)));
                goto fail;
            }
        }
    }

    return 1;
fail:
    return 0;
}

EFI_STATUS
EFIAPI
MMCHSReadBlocks(
    IN EFI_BLOCK_IO_PROTOCOL          *This,
    IN UINT32                         MediaId,
    IN EFI_LBA                        Lba,
    IN UINTN                          BufferSize,
    OUT VOID                          *Buffer
)
{
    BIO_INSTANCE              *Instance;
    EFI_BLOCK_IO_MEDIA        *Media;
    UINTN                     BlockSize;
    UINTN                     NumBlocks;
    UINTN                     rc;
    UINT64                    DataAddr;

    Instance  = BIO_INSTANCE_FROM_BLOCKIO_THIS(This);
    Media     = &Instance->BlockMedia;
    BlockSize = Media->BlockSize;

    if (MediaId != Media->MediaId) {
        return EFI_MEDIA_CHANGED;
    }

    if (BufferSize == 0) {
        return EFI_SUCCESS;
    }

    if (Buffer == NULL) {
        return EFI_INVALID_PARAMETER;
    }

    if (BlockSize == 0) {
        return EFI_DEVICE_ERROR;
    }

    if (BufferSize % BlockSize != 0) {
        return EFI_BAD_BUFFER_SIZE;
    }

    NumBlocks = BufferSize / BlockSize;
    if (NumBlocks == 0) {
        return EFI_SUCCESS;
    }

    if (Lba > Media->LastBlock) {
        return EFI_INVALID_PARAMETER;
    }

    if (((EFI_LBA)(NumBlocks - 1)) > (Media->LastBlock - Lba)) {
        return EFI_INVALID_PARAMETER;
    }

    if ((UINT64)Lba > (MAX_UINT64 / BlockSize)) {
        return EFI_INVALID_PARAMETER;
    }

    DataAddr = (UINT64)Lba * BlockSize;
    rc = MmcReadInternal(Instance, DataAddr, Buffer, BufferSize);
    if (rc == 1)
        return EFI_SUCCESS;
    else
        return EFI_DEVICE_ERROR;
}

EFI_STATUS
EFIAPI
MMCHSWriteBlocks(
    IN EFI_BLOCK_IO_PROTOCOL          *This,
    IN UINT32                         MediaId,
    IN EFI_LBA                        Lba,
    IN UINTN                          BufferSize,
    IN VOID                           *Buffer
)
{
    BIO_INSTANCE       *Instance;
    EFI_BLOCK_IO_MEDIA *Media;

    Instance = BIO_INSTANCE_FROM_BLOCKIO_THIS (This);
    Media = &Instance->BlockMedia;

    if (Media->ReadOnly) {
        return EFI_WRITE_PROTECTED;
    }

    return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
MMCHSFlushBlocks(
    IN EFI_BLOCK_IO_PROTOCOL  *This
)
{
    return EFI_SUCCESS;
}

EFI_STATUS
BioInstanceContructor(
    OUT BIO_INSTANCE** NewInstance
)
{
    BIO_INSTANCE* Instance;

    Instance = AllocateCopyPool(sizeof(BIO_INSTANCE), &mBioTemplate);
    if (Instance == NULL) {
        return EFI_OUT_OF_RESOURCES;
    }

    Instance->BlockIo.Media = &Instance->BlockMedia;

    *NewInstance = Instance;
    return EFI_SUCCESS;
}
