/** @QspiDxe.c

  Driver for installing BlockIo protocol over QSPI

  Based on NorFlash implementation available in
  ArmPlatformPkg/Drivers/NorFlashDxe/NorFlashDxe.c

  Copyright (c) 2011 - 2014, ARM Ltd. All rights reserved.<BR>
  Copyright 2016- 2017 NXP

  This program and the accompanying materials
  are licensed and made available under the terms and conditions of the BSD
  License which accompanies this distribution. The full text of the license
  may be found at
http://opensource.org/licenses/bsd-license.php

THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.

 **/
#include <Library/BaseMemoryLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Bitops.h>

#include "QspiDxe.h"

STATIC EFI_EVENT mQspiVirtualAddrChangeEvent;
EFI_EVENT EfiExitBootServicesEvent      = (EFI_EVENT)NULL;

//
// Global variable declarations
//
QSPI_FLASH_INSTANCE **mQspiInstances;
UINT32               mQspiDeviceCount;

QSPI_FLASH_INSTANCE  mQspiInstanceTemplate = {
  QSPI_FLASH_SIGNATURE,              // Signature
  NULL,                              // Handle ... NEED TO BE FILLED

  FALSE,                             // Initialized
  NULL,                              // Initialize

  0,                                 // DeviceBaseAddress ... NEED TO BE FILLED
  0,                                 // RegionBaseAddress ... NEED TO BE FILLED
  0,                                 // Size ... NEED TO BE FILLED
  0,                                 // StartLba

  {
    EFI_BLOCK_IO_INTERFACE_REVISION, // Revision
    NULL,                            // Media ... NEED TO BE FILLED
    QspiBlockIoReset,                // Reset
    QspiBlockIoReadBlocks,           // ReadBlocks
    QspiBlockIoWriteBlocks,          // WriteBlocks
    QspiBlockIoFlushBlocks           // FlushBlocks
  },                                 // BlockIoProtocol

  {
    0,                               // MediaId ... NEED TO BE FILLED
    FALSE,                           // RemovableMedia
    TRUE,                            // MediaPresent
    FALSE,                           // LogicalPartition
    FALSE,                           // ReadOnly
    FALSE,                           // WriteCaching
    0,                               // BlockSize ... NEED TO BE FILLED
    2,                               // IoAlign
    0,                               // LastBlock ... NEED TO BE FILLED
    0                                // Pad
  },                                 //Media;

  FALSE,                             // SupportFvb ... NEED TO BE FILLED
  {
    FvbGetAttributes,                // GetAttributes
    FvbSetAttributes,                // SetAttributes
    FvbGetPhysicalAddress,           // GetPhysicalAddress
    FvbGetBlockSize,                 // GetBlockSize
    FvbRead,                         // Read
    FvbWrite,                        // Write
    FvbEraseBlocks,                  // EraseBlocks
    NULL,                            // ParentHandle
  },                                 // FvbProtoccol
  NULL,                              // ShadowBuffer
  // Device path for SemiHosting. It contains our autogenerated Caller ID GUID.
  {
    {
      {
        HARDWARE_DEVICE_PATH,
        HW_VENDOR_DP,
        {
	  (UINT8)sizeof(VENDOR_DEVICE_PATH),
	  (UINT8)((sizeof(VENDOR_DEVICE_PATH)) >> 8)
        }
      },
      EFI_CALLER_ID_GUID
    },
    {
      END_DEVICE_PATH_TYPE,
      END_ENTIRE_DEVICE_PATH_SUBTYPE,
      { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0}
    }
  } // DevicePath
};

  EFI_STATUS
QspiCreateInstance (
    IN UINTN                  QspiDeviceBase,
    IN UINTN                  QspiRegionBase,
    IN UINTN                  QspiSize,
    IN UINT32                 MediaId,
    IN UINT32                 BlockSize,
    IN BOOLEAN                SupportFvb,
    OUT QSPI_FLASH_INSTANCE**  QspiInstance
    )
{
  EFI_STATUS Status;
  QSPI_FLASH_INSTANCE* Instance;

  ASSERT(QspiInstance != NULL);

  Instance = AllocateRuntimeCopyPool (sizeof(QSPI_FLASH_INSTANCE),\
      &mQspiInstanceTemplate);
  if (Instance == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Instance->DeviceBaseAddress = QspiDeviceBase;
  Instance->RegionBaseAddress = QspiRegionBase;
  Instance->Size = QspiSize;

  Instance->BlockIoProtocol.Media = &Instance->Media;
  Instance->Media.MediaId = MediaId;
  Instance->Media.BlockSize = BlockSize;
  Instance->Media.LastBlock = (QspiSize / BlockSize)-1;

  Instance->ShadowBuffer = AllocateRuntimePool (BlockSize);
  if (Instance->ShadowBuffer == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  if (SupportFvb) {
    Instance->SupportFvb = TRUE;
    Instance->Initialize = QspiFvbInitialize;

    Status = gBS->InstallMultipleProtocolInterfaces (
        &Instance->Handle,
        &gEfiDevicePathProtocolGuid, &Instance->DevicePath,
        &gEfiBlockIoProtocolGuid,  &Instance->BlockIoProtocol,
        &gEfiFirmwareVolumeBlockProtocolGuid, &Instance->FvbProtocol,
        NULL
        );
    if (EFI_ERROR(Status)) {
      FreePool (Instance);
      return Status;
    }
  } else {
    Instance->Initialized = TRUE;

    Status = gBS->InstallMultipleProtocolInterfaces (
        &Instance->Handle,
        &gEfiDevicePathProtocolGuid, &Instance->DevicePath,
        &gEfiBlockIoProtocolGuid,  &Instance->BlockIoProtocol,
        NULL
        );
    if (EFI_ERROR(Status)) {
      FreePool (Instance);
      return Status;
    }
  }

  *QspiInstance = Instance;
  return Status;
}

  EFI_STATUS
QspiRead (
    IN    QSPI_FLASH_INSTANCE            *Instance,
    IN    EFI_LBA                        Lba,
    IN    UINTN                          Offset,
    IN    UINTN                          BufferSizeInBytes,
    OUT   VOID                           *Buffer
    )
{
  EFI_STATUS  Status;
  UINTN       StartOffset;

  // The buffer must be valid
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  // Return if we have not any byte to read
  if (BufferSizeInBytes == 0) {
    return EFI_SUCCESS;
  }

  if (((Lba * Instance->Media.BlockSize) + Offset + BufferSizeInBytes) >
      Instance->Size) {
    DEBUG ((EFI_D_ERROR, "QspiRead: ERROR - Read will exceed device size.\n"));
    return EFI_INVALID_PARAMETER;
  }

  // Get the Offset to start reading from
  StartOffset = GET_BLOCK_OFFSET(Lba) + Offset;
  Status = QspiFlashRead(StartOffset, BufferSizeInBytes, Buffer);

  return Status;
}

/*
   Write a full or portion of a block.
   It must not span block boundaries; that is,
   Offset + NumBytes <= Instance->Media.BlockSize.
   */
  EFI_STATUS
QspiWrite (
    IN        QSPI_FLASH_INSTANCE   *Instance,
    IN        EFI_LBA               Lba,
    IN        UINTN                 Offset,
    IN OUT    UINTN                 NumBytes,
    IN        UINT8                 *Buffer
    )
{
  EFI_STATUS               Status = EFI_SUCCESS;
  UINTN                    BlockSize;
  BOOLEAN                  DoErase;
  VOID*                    Source = NULL;

  if (!Instance->Initialized && Instance->Initialize) {
    Instance->Initialize(Instance);
  }

  DEBUG ((DEBUG_BLKIO, "%a(Parameters: Lba=%ld, Offset=0x%x, NumBytes=0x%x, "
        "Buffer @ 0x%08x)\n",__FUNCTION__,
        Lba, Offset, NumBytes, Buffer));

  // The buffer must be valid
  if (Buffer == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  // Detect WriteDisabled state
  if (Instance->Media.ReadOnly == TRUE) {
    DEBUG ((EFI_D_ERROR, "QspiWrite: ERROR - Can not write: "
          "Device is in WriteDisabled state.\n"));
    // It is in WriteDisabled state, return an error right away
    return EFI_ACCESS_DENIED;
  }

  // Cache the block size to avoid de-referencing pointers all the time
  BlockSize = Instance->Media.BlockSize;

  // We must have some bytes to write
  if ((NumBytes == 0) || (NumBytes > BlockSize)) {
    DEBUG ((EFI_D_ERROR, "QspiWrite: ERROR - EFI_BAD_BUFFER_SIZE: "
          "(Offset=0x%x + NumBytes=0x%x) > BlockSize=0x%x\n",\
          Offset, NumBytes, BlockSize ));
    return EFI_BAD_BUFFER_SIZE;
  }

  if (((Lba * BlockSize) + Offset + NumBytes) > Instance->Size) {
    DEBUG ((EFI_D_ERROR, "%a: ERROR - Write will exceed device size.\n",
          __FUNCTION__));
    return EFI_INVALID_PARAMETER;
  }

  // Check we did get some memory. Buffer is BlockSize.
  if (Instance->ShadowBuffer == NULL) {
    DEBUG ((EFI_D_ERROR, "FvbWrite: ERROR - Buffer not ready\n"));
    return EFI_DEVICE_ERROR;
  }

  // Pick 128bytes as a good start for word operations as opposed to erasing the
  // block and writing the data regardless if an erase is really needed.
  // It looks like most individual NV variable writes are smaller than 128bytes.
  if (NumBytes <= 128) {
    Source = Instance->ShadowBuffer;
    //First Read the data into shadow buffer from location where data is to be written
    Status = QspiRead (
               Instance,
               Lba,
               Offset,
               NumBytes,
               Source
             );
    if (EFI_ERROR (Status)) {
      DEBUG((EFI_D_ERROR, "%a: ERROR - Failed to "
             "Read @ Offset 0x%x Status=%d\n",__FUNCTION__,
             Offset + GET_BLOCK_OFFSET(Lba),Status));
      return Status;
    }
    // Check to see if we need to erase before programming the data into QSPI.
    // If the destination bits are only changing from 1s to 0s we can
    // just write. After a block is erased all bits in the block is set to 1.
    // If any byte requires us to erase we just give up and rewrite all of it.
    DoErase = TestBitSetClear(Source, Buffer, NumBytes, TRUE);

    // if we got here then write all the data. Otherwise do the
    // Erase-Write cycle.
    if (!DoErase) {
      Status = QspiFlashWrite (
                 Offset + GET_BLOCK_OFFSET(Lba),
                 NumBytes,
                 Buffer
               );
      if (EFI_ERROR (Status)) {
        DEBUG((EFI_D_ERROR, "%a: ERROR - Failed to "
              "Write @ Offset 0x%x Status=%d\n",__FUNCTION__,
              Offset + GET_BLOCK_OFFSET(Lba),Status));
        return Status;
      }
      return EFI_SUCCESS;
    }
  }

  // If we are going to write full block, no need to read block and then update bytes in it
  if (NumBytes != BlockSize) {
    // Read QSPI Flash data into shadow buffer
    Status = QspiBlockIoReadBlocks (
               &(Instance->BlockIoProtocol),
               Instance->Media.MediaId,
               Lba,
               BlockSize,
               Instance->ShadowBuffer
             );
    if (EFI_ERROR (Status)) {
      // Return one of the pre-approved error statuses
      return EFI_DEVICE_ERROR;
    }
    // Put the data at the appropriate location inside the buffer area
    CopyMem ((VOID*)((UINTN)Instance->ShadowBuffer + Offset), Buffer, NumBytes);
  }
  //Erase Block
  Status = QspiFlashErase(GET_BLOCK_OFFSET(Lba), BlockSize);
  if (EFI_ERROR (Status)) {
    // Return one of the pre-approved error statuses
    return EFI_DEVICE_ERROR;
  }
  if (NumBytes != BlockSize) {
    // Write the modified shadow buffer back to the QspiFlash
    Status = QspiFlashWrite (
               GET_BLOCK_OFFSET(Lba),
               BlockSize,
               Instance->ShadowBuffer
             );
  } else {
    // Write the Buffer to an entire block in QspiFlash
    Status = QspiFlashWrite (
               GET_BLOCK_OFFSET(Lba),
               BlockSize,
               Buffer
             );
  }
  if (EFI_ERROR (Status)) {
    // Return one of the pre-approved error statuses
    return EFI_DEVICE_ERROR;
  }

  return EFI_SUCCESS;
}

/**
  Shutdown our hardware

  @param[in]  Event     The Event that is being processed
  @param[in]  Context   Event Context
 **/
VOID
  EFIAPI
QspiExitBootServicesEvent (
    IN EFI_EVENT  Event,
    IN VOID       *Context
    )
{
  EFI_HANDLE                    ImageHandle = NULL;
  QspiUnload(ImageHandle);
  QspiFlashFree();
}

/**
  Fixup internal data so that EFI can be call in virtual mode.
  Call the passed in Child Notify event and convert any pointers in
  lib to virtual mode.

  @param[in]    Event   The Event that is being processed
  @param[in]    Context Event Context
 **/
VOID
  EFIAPI
QspiVirtualNotifyEvent (
    IN EFI_EVENT        Event,
    IN VOID             *Context
    )
{
  UINTN Index;

  for (Index = 0; Index < mQspiDeviceCount; Index++) {
    EfiConvertPointer (0x0, (VOID**)&mQspiInstances[Index]->DeviceBaseAddress);
    EfiConvertPointer (0x0, (VOID**)&mQspiInstances[Index]->RegionBaseAddress);

    // Convert BlockIo protocol
    EfiConvertPointer (0x0,
        (VOID**)&mQspiInstances[Index]->BlockIoProtocol.FlushBlocks);
    EfiConvertPointer (0x0,
        (VOID**)&mQspiInstances[Index]->BlockIoProtocol.ReadBlocks);
    EfiConvertPointer (0x0,
        (VOID**)&mQspiInstances[Index]->BlockIoProtocol.Reset);
    EfiConvertPointer (0x0,
        (VOID**)&mQspiInstances[Index]->BlockIoProtocol.WriteBlocks);

    // Convert Fvb
    EfiConvertPointer (0x0,
        (VOID**)&mQspiInstances[Index]->FvbProtocol.EraseBlocks);
    EfiConvertPointer (0x0,
        (VOID**)&mQspiInstances[Index]->FvbProtocol.GetAttributes);
    EfiConvertPointer (0x0,
        (VOID**)&mQspiInstances[Index]->FvbProtocol.GetBlockSize);
    EfiConvertPointer (0x0,
        (VOID**)&mQspiInstances[Index]->FvbProtocol.GetPhysicalAddress);
    EfiConvertPointer (0x0,
        (VOID**)&mQspiInstances[Index]->FvbProtocol.Read);
    EfiConvertPointer (0x0,
        (VOID**)&mQspiInstances[Index]->FvbProtocol.SetAttributes);
    EfiConvertPointer (0x0, (VOID**)&mQspiInstances[Index]->FvbProtocol.Write);

    if (mQspiInstances[Index]->ShadowBuffer != NULL) {
      EfiConvertPointer (0x0, (VOID**)&mQspiInstances[Index]->ShadowBuffer);
    }
    QspiFlashRelocate();
  }
  return;
}

EFI_STATUS
  EFIAPI
QspiInitialize (
    IN EFI_HANDLE         ImageHandle,
    IN EFI_SYSTEM_TABLE   *SystemTable
    )
{
  EFI_STATUS              Status;
  UINT32                  Index;
  QSPI_FLASH_DESCRIPTION*  QspiDevices;
  BOOLEAN                 ContainVariableStorage;
  Status = QspiDetect ();

  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR,"QspiInitialize: "
          "Fail to initialize Qspi Flash devices\n"));
    return Status;
  }

  Status = QspiPlatformGetDevices (&QspiDevices, &mQspiDeviceCount);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR,"QspiInitialize: Fail to get Qspi Flash devices\n"));
    return Status;
  }

  Status = QspiPlatformFlashGetAttributes (QspiDevices, mQspiDeviceCount);
  if (EFI_ERROR(Status)) {
    DEBUG((EFI_D_ERROR,"QspiInitialise: Fail to get Qspi device attributes\n"));
    /* System becomes unusable if QSPI flash is not detected */
    ASSERT_EFI_ERROR (Status); 
    return Status;
  }

  mQspiInstances = AllocateRuntimePool (
      sizeof(QSPI_FLASH_INSTANCE*) * mQspiDeviceCount
      );

  for (Index = 0; Index < mQspiDeviceCount; Index++) {
    // Check if this Qspi Flash device contain the variable storage region
    ContainVariableStorage =
      (QspiDevices[Index].RegionBaseAddress <=
       PcdGet64 (PcdFlashNvStorageVariableBase64)) &&
      (PcdGet64 (PcdFlashNvStorageVariableBase64) +
       PcdGet32 (PcdFlashNvStorageVariableSize) <=
       QspiDevices[Index].RegionBaseAddress + QspiDevices[Index].Size);

    Status = QspiCreateInstance (
        QspiDevices[Index].DeviceBaseAddress,
        QspiDevices[Index].RegionBaseAddress,
        QspiDevices[Index].Size,
        Index,
        QspiDevices[Index].BlockSize,
        ContainVariableStorage,
        &mQspiInstances[Index]
        );
    if (EFI_ERROR(Status)) {
      DEBUG((EFI_D_ERROR,"QspiInitialize: "
            "Fail to create instance for Qspi[%d]\n",Index));
    }
    if (FeaturePcdGet(PcdQspiTest) && (TRUE == ContainVariableStorage)) {
      Status = QspiTest(mQspiInstances[Index]);
      if (EFI_ERROR(Status)) {
        DEBUG((EFI_D_ERROR,"QspiInitialize: QspiTest Failed\n"));
        return EFI_DEVICE_ERROR;
      }
    }
  }

  //
  // Register for the virtual address change event
  //
  Status = gBS->CreateEventEx (
      EVT_NOTIFY_SIGNAL,
      TPL_NOTIFY,
      QspiVirtualNotifyEvent,
      NULL,
      &gEfiEventVirtualAddressChangeGuid,
      &mQspiVirtualAddrChangeEvent
      );
  ASSERT_EFI_ERROR (Status);

  return Status;
}

/**
  This is the unload handle for Qspi Controller module.

  Disconnect the driver specified by ImageHandle from all the devices in the
  handle database.
  Uninstall all the protocols installed in the driver entry point.

  @param[in] ImageHandle           The drivers' driver image.

  @retval    EFI_SUCCESS           The image is unloaded.
  @retval    Others                Failed to unload the image.

 **/
EFI_STATUS
  EFIAPI
QspiUnload (
    IN EFI_HANDLE             ImageHandle
    )
{
  EFI_STATUS                            Status;
  EFI_HANDLE                            *DeviceHandleBuffer;
  UINTN                         DeviceHandleCount;

  //
  // Get the list of all Qspi Controller handles in the handle database.
  // If there is an error getting the list, then the unload
  // operation fails.
  //
  Status = gBS->LocateHandleBuffer (
      ByProtocol,
      &gEfiBlockIoProtocolGuid,
      NULL,
      &DeviceHandleCount,
      &DeviceHandleBuffer
      );
  //
  // Free the buffer containing the list of handles from the handle database
  //
  if (DeviceHandleBuffer != NULL) {
    gBS->FreePool (DeviceHandleBuffer);
  }

  return Status;
}
