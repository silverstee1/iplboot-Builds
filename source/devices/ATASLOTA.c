/**
 * IDE-EXI Driver for GameCube.
 *
 * IDE EXI + M.2 SSD Bridge adapter for Slot A.
 *
 * Based on IDE-EXI Driver from Swiss
 * Based loosely on code written by Dampro
 * Re-written by emu_kidid, Extrems, webhdx, silversteel
 **/

#include <stdio.h>
#include <gccore.h> /*** Wrapper to include common libogc headers ***/
#include <ogcsys.h> /*** Needed for console support ***/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <debug.h>
#include <ogc/exi.h>
#include <ogc/machine/processor.h>
#include <malloc.h>
#include "ATASLOTA.h"

// #define _ataslota_DEBUG

extern void usleep(int s);

u16 bufferA[256] ATTRIBUTE_ALIGN(32);
static bool ATASLOTA_DriveInserted = false;

// Drive information struct
typeDriveInfoA ATASLOTADriveInfo;

// Returns 8 bits from the ATA Status register
inline u8 _ATASLOTA_ReadStatusReg()
{
    // read ATA_REG_CMDSTATUS1 | 0x00 (dummy)
    u16 dat = 0x1700;
    EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 2, EXI_WRITE);
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 1, EXI_READ);
    EXI_Deselect(EXI_CHANNEL_0);
    EXI_Unlock(EXI_CHANNEL_0);

    return *(u8 *)&dat;
}

// Returns 8 bits from the ATA Error register
inline u8 _ATASLOTA_ReadErrorReg()
{
    // read ATA_REG_ERROR | 0x00 (dummy)
    u16 dat = 0x1100;
    EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 2, EXI_WRITE);
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 1, EXI_READ);
    EXI_Deselect(EXI_CHANNEL_0);
    EXI_Unlock(EXI_CHANNEL_0);

    return *(u8 *)&dat;
}

// Writes 8 bits of data out to the specified ATA Register
inline void _ATASLOTA_WriteByte(u8 addr, u8 data)
{
    u32 dat = 0x80000000 | (addr << 24) | (data << 16);
    EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 3, EXI_WRITE);
    EXI_Deselect(EXI_CHANNEL_0);
    EXI_Unlock(EXI_CHANNEL_0);
}

// Writes 16 bits to the ATA Data register
inline void _ATASLOTA_WriteU16(u16 data)
{
    // write 16 bit to ATA_REG_DATA | data LSB | data MSB | 0x00 (dummy)
    u32 dat = 0xD0000000 | (((data >> 8) & 0xff) << 16) | ((data & 0xff) << 8);
    EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 4, EXI_WRITE);
    EXI_Deselect(EXI_CHANNEL_0);
    EXI_Unlock(EXI_CHANNEL_0);
}

// Returns 16 bits from the ATA Data register
inline u16 _ATASLOTA_ReadU16()
{
    // read 16 bit from ATA_REG_DATA | 0x00 (dummy)
    u16 dat = 0x5000;
    EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 2, EXI_WRITE);
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 2, EXI_READ); // read LSB & MSB
    EXI_Deselect(EXI_CHANNEL_0);
    EXI_Unlock(EXI_CHANNEL_0);

    return dat;
}

// Reads 512 bytes
inline void _ATASLOTA_ReadBuffer(u32 *dst)
{
    u16 dwords = 128; // 128 * 4 = 512 bytes
    // (31:29) 011b | (28:24) 10000b | (23:16) <num_words_LSB> | (15:8) <num_words_MSB> | (7:0) 00h (4 bytes)
    u32 dat = 0x70000000 | ((dwords & 0xff) << 16) | (((dwords >> 8) & 0xff) << 8);
    EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 4, EXI_WRITE);

    u32 *ptr = dst;
    if (((u32)dst) % 32)
    {
        ptr = (u32 *)memalign(32, 512);
    }

    DCInvalidateRange(ptr, 512);
    EXI_Dma(EXI_CHANNEL_0, ptr, 512, EXI_READ, NULL);
    EXI_Sync(EXI_CHANNEL_0);
    if (((u32)dst) % 32)
    {
        memcpy(dst, ptr, 512);
        free(ptr);
    }

    EXI_Deselect(EXI_CHANNEL_0);
    EXI_Unlock(EXI_CHANNEL_0);
}

inline void _ATASLOTA_WriteBuffer(u32 *src)
{
    u16 dwords = 128; // 128 * 4 = 512 bytes
    // (23:21) 111b | (20:16) 10000b | (15:8) <num_words_LSB> | (7:0) <num_words_MSB> (3 bytes)
    u32 dat = 0xF0000000 | ((dwords & 0xff) << 16) | (((dwords >> 8) & 0xff) << 8);
    EXI_Lock(EXI_CHANNEL_0, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_0, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 3, EXI_WRITE);
    EXI_ImmEx(EXI_CHANNEL_0, src, 512, EXI_WRITE);
    dat = 0;
    EXI_ImmEx(EXI_CHANNEL_0, &dat, 1, EXI_WRITE); // Burn an extra cycle for the M.2 Loader to know to stop serving data
    EXI_Deselect(EXI_CHANNEL_0);
    EXI_Unlock(EXI_CHANNEL_0);
}

void _ATASLOTA_PrintHddSector(u32 *dest)
{
    int i = 0;
    for (i = 0; i < 512 / 4; i += 4)
    {
        printf("%08X:%08X %08X %08X %08X\r\n", i * 4, dest[i], dest[i + 1], dest[i + 2], dest[i + 3]);
    }
}

bool ATASLOTA_IsInserted()
{
    u32 cid = 0;
    EXI_GetID(EXI_CHANNEL_0, EXI_DEVICE_0, &cid);

    //return cid == EXI_ATASLOTA_ID;
	return (cid&~0xff) == EXI_ATASLOTA_ID;
}

// Sends the IDENTIFY command to the SSD
// Returns 0 on success, -1 otherwise
u32 _ATASLOTA_DriveIdentify()
{
    u16 tmp, retries = 50;
    u32 i = 0;

    memset(&ATASLOTADriveInfo, 0, sizeof(typeDriveInfoA));

    // Select the device
    _ATASLOTA_WriteByte(ATA_REG_DEVICE, 0 /*ATA_HEAD_USE_LBA*/);

    // Wait for drive to be ready (BSY to clear) - 5 sec timeout
    do
    {
        tmp = _ATASLOTA_ReadStatusReg();
        usleep(100000); // sleep for 0.1 seconds
        retries--;
#ifdef _ataslota_DEBUG
        printf("(%08X) Waiting for BSY to clear..\r\n", tmp);
#endif
    } while ((tmp & ATA_SR_BSY) && retries);
    if (!retries)
    {
#ifdef _ataslota_DEBUG
        printf("Exceeded retries..\r\n");
#endif

        return -1;
    }

    // Write the identify command
    _ATASLOTA_WriteByte(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    // Wait for drive to request data transfer - 1 sec timeout
    retries = 10;
    do
    {
        tmp = _ATASLOTA_ReadStatusReg();
        usleep(100000); // sleep for 0.1 seconds
        retries--;
#ifdef _ataslota_DEBUG
        printf("(%08X) Waiting for DRQ to toggle..\r\n", tmp);
#endif
    } while ((!(tmp & ATA_SR_DRQ)) && retries);
    if (!retries)
    {
#ifdef _ataslota_DEBUG
        printf("(%08X) Drive did not respond in time, failing M.2 Loader init..\r\n", tmp);
#endif

        return -1;
    }
    usleep(2000);

    u16 *ptr = (u16 *)(&bufferA[0]);

    // Read Identify data from drive
    for (i = 0; i < 256; i++)
    {
        tmp = _ATASLOTA_ReadU16(); // get data
        *ptr++ = bswap16(tmp);     // swap
    }

    // Get the info out of the Identify data buffer
    // From the command set, check if LBA48 is supported
    u16 commandSet = *(u16 *)(&bufferA[ATA_IDENT_COMMANDSET]);
    ATASLOTADriveInfo.lba48Support = (commandSet >> 8) & ATA_IDENT_LBA48MASK;

    if (ATASLOTADriveInfo.lba48Support)
    {
        u16 lbaHi = *(u16 *)(&bufferA[ATA_IDENT_LBA48SECTORS + 2]);
        u16 lbaMid = *(u16 *)(&bufferA[ATA_IDENT_LBA48SECTORS + 1]);
        u16 lbaLo = *(u16 *)(&bufferA[ATA_IDENT_LBA48SECTORS]);
        ATASLOTADriveInfo.sizeInSectors = (u64)(((u64)lbaHi << 32) | (lbaMid << 16) | lbaLo);
        ATASLOTADriveInfo.sizeInGigaBytes = (u32)((ATASLOTADriveInfo.sizeInSectors << 9) / 1024 / 1024 / 1024);
    }
    else
    {
        ATASLOTADriveInfo.cylinders = *(u16 *)(&bufferA[ATA_IDENT_CYLINDERS]);
        ATASLOTADriveInfo.heads = *(u16 *)(&bufferA[ATA_IDENT_HEADS]);
        ATASLOTADriveInfo.sectors = *(u16 *)(&bufferA[ATA_IDENT_SECTORS]);
        ATASLOTADriveInfo.sizeInSectors = ((*(u16 *)&bufferA[ATA_IDENT_LBASECTORS + 1]) << 16) |
                                          (*(u16 *)&bufferA[ATA_IDENT_LBASECTORS]);
        ATASLOTADriveInfo.sizeInGigaBytes = (u32)((ATASLOTADriveInfo.sizeInSectors << 9) / 1024 / 1024 / 1024);
    }

    i = 20;
    // copy serial string
    memcpy(&ATASLOTADriveInfo.serial[0], &bufferA[ATA_IDENT_SERIAL], 20);
    // cut off the string (usually has trailing spaces)
    while ((ATASLOTADriveInfo.serial[i] == ' ' || !ATASLOTADriveInfo.serial[i]) && i >= 0)
    {
        ATASLOTADriveInfo.serial[i] = 0;
        i--;
    }
    // copy model string
    memcpy(&ATASLOTADriveInfo.model[0], &bufferA[ATA_IDENT_MODEL], 40);
    // cut off the string (usually has trailing spaces)
    i = 40;
    while ((ATASLOTADriveInfo.model[i] == ' ' || !ATASLOTADriveInfo.model[i]) && i >= 0)
    {
        ATASLOTADriveInfo.model[i] = 0;
        i--;
    }

#ifdef _ataslota_DEBUG
    printf("%d GB SDD Connected\r\n", ATASLOTADriveInfo.sizeInGigaBytes);
    printf("LBA 48-Bit Mode %s\r\n", ATASLOTADriveInfo.lba48Support ? "Supported" : "Not Supported");
    if (!ATASLOTADriveInfo.lba48Support)
    {
        printf("Cylinders: %i\r\n", ATASLOTADriveInfo.cylinders);
        printf("Heads Per Cylinder: %i\r\n", ATASLOTADriveInfo.heads);
        printf("Sectors Per Track: %i\r\n", ATASLOTADriveInfo.sectors);
    }
    printf("Model: %s\r\n", ATASLOTADriveInfo.model);
    printf("Serial: %s\r\n", ATASLOTADriveInfo.serial);
    _ATASLOTA_PrintHddSector((u32 *)&bufferA);
#endif

#ifdef _ataslota_DEBUG
    int unlockStatus = ATASLOTA_Unlock(1, "password\0", ATA_CMD_UNLOCK);
    printf("Unlock Status was: %i\r\n", unlockStatus);
#else
    ATASLOTA_Unlock(1, "password\0", ATA_CMD_UNLOCK);
#endif

#ifdef _ataslota_DEBUG
    unlockStatus = ATASLOTA_Unlock(1, "password\0", ATA_CMD_SECURITY_DISABLE);
    printf("Disable Status was: %i\r\n", unlockStatus);
#else
    ATASLOTA_Unlock(1, "password\0", ATA_CMD_SECURITY_DISABLE);
#endif

    return 0;
}

// Unlocks a ATA HDD with a password
// Returns 0 on success, -1 on failure.
int ATASLOTA_Unlock(int useMaster, char *password, int command)
{
    u32 i;
    u16 tmp, retries = 50;

    // Select the device
    _ATASLOTA_WriteByte(ATA_REG_DEVICE, ATA_HEAD_USE_LBA);

    // Wait for drive to be ready (BSY to clear) - 5 sec timeout
    do
    {
        tmp = _ATASLOTA_ReadStatusReg();
        usleep(100000); // sleep for 0.1 seconds
        retries--;
#ifdef _ataslota_DEBUG
        printf("UNLOCK (%08X) Waiting for BSY to clear..\r\n", tmp);
#endif
    } while ((tmp & ATA_SR_BSY) && retries);
    if (!retries)
    {
#ifdef _ataslota_DEBUG
        printf("UNLOCK Exceeded retries..\r\n");
#endif

        return -1;
    }

    // Write the appropriate unlock command
    _ATASLOTA_WriteByte(ATA_REG_COMMAND, command);

    // Wait for drive to request data transfer - 1 sec timeout
    retries = 10;
    do
    {
        tmp = _ATASLOTA_ReadStatusReg();
        usleep(100000); // sleep for 0.1 seconds
        retries--;
#ifdef _ataslota_DEBUG
        printf("UNLOCK (%08X) Waiting for DRQ to toggle..\r\n", tmp);
#endif
    } while ((!(tmp & ATA_SR_DRQ)) && retries);
    if (!retries)
    {
#ifdef _ataslota_DEBUG
        printf("UNLOCK (%08X) Drive did not respond in time, failing M.2 Loader init..\r\n", tmp);
#endif

        return -1;
    }
    usleep(2000);

    // Fill an unlock struct
    unlockStructA unlock;
    memset(&unlock, 0, sizeof(unlockStructA));
    unlock.type = (u16)useMaster;
    memcpy(unlock.password, password, strlen(password));

    // write data to the drive
    u16 *ptr = (u16 *)&unlock;
    for (i = 0; i < 256; i++)
    {
        ptr[i] = bswap16(ptr[i]);
        _ATASLOTA_WriteU16(ptr[i]);
    }

    // Wait for BSY to clear
    u32 temp = 0;
    while ((temp = _ATASLOTA_ReadStatusReg()) & ATA_SR_BSY)
        ;

    // If the error bit was set, fail.
    if (temp & ATA_SR_ERR)
    {
#ifdef _ataslota_DEBUG
        printf("Error: %02X\r\n", _ATASLOTA_ReadErrorReg());
#endif

        return 1;
    }

    return !(_ATASLOTA_ReadErrorReg() & ATA_ER_ABRT);
}

// Reads sectors from the specified lba, for the specified slot
// Returns 0 on success, -1 on failure.
int _ATASLOTA_ReadSector(u64 lba, u32 *Buffer)
{
    u32 temp = 0;

    // Wait for drive to be ready (BSY to clear)
    while (_ATASLOTA_ReadStatusReg() & ATA_SR_BSY)
        ;

    // Select the device differently based on 28 or 48bit mode
    if (ATASLOTADriveInfo.lba48Support)
    {
        // Select the device (ATA_HEAD_USE_LBA is 0x40 for master, 0x50 for slave)
        _ATASLOTA_WriteByte(ATA_REG_DEVICE, ATA_HEAD_USE_LBA);
    }
    else
    {
        // Select the device (ATA_HEAD_USE_LBA is 0x40 for master, 0x50 for slave)
        _ATASLOTA_WriteByte(ATA_REG_DEVICE, 0xE0 | (u8)((lba >> 24) & 0x0F));
    }

    // check if drive supports LBA 48-bit
    if (ATASLOTADriveInfo.lba48Support)
    {
        _ATASLOTA_WriteByte(ATA_REG_SECCOUNT, 0);                      // Sector count (Hi)
        _ATASLOTA_WriteByte(ATA_REG_LBALO, (u8)((lba >> 24) & 0xFF));  // LBA 4
        _ATASLOTA_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 32) & 0xFF)); // LBA 5
        _ATASLOTA_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 40) & 0xFF));  // LBA 6
        _ATASLOTA_WriteByte(ATA_REG_SECCOUNT, 1);                      // Sector count (Lo)
        _ATASLOTA_WriteByte(ATA_REG_LBALO, (u8)(lba & 0xFF));          // LBA 1
        _ATASLOTA_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 8) & 0xFF));  // LBA 2
        _ATASLOTA_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 16) & 0xFF));  // LBA 3
    }
    else
    {
        _ATASLOTA_WriteByte(ATA_REG_SECCOUNT, 1);                     // Sector count
        _ATASLOTA_WriteByte(ATA_REG_LBALO, (u8)(lba & 0xFF));         // LBA Lo
        _ATASLOTA_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 8) & 0xFF)); // LBA Mid
        _ATASLOTA_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 16) & 0xFF)); // LBA Hi
    }

    // Write the appropriate read command
    _ATASLOTA_WriteByte(ATA_REG_COMMAND, ATASLOTADriveInfo.lba48Support ? ATA_CMD_READSECTEXT : ATA_CMD_READSECT);

    // Wait for BSY to clear
    while ((temp = _ATASLOTA_ReadStatusReg()) & ATA_SR_BSY)
        ;

    // If the error bit was set, fail.
    if (temp & ATA_SR_ERR)
    {
#ifdef _ataslota_DEBUG
        printf("Error: %02X", _ATASLOTA_ReadErrorReg());
#endif

        return 1;
    }

    // Wait for drive to request data transfer
    while (!(_ATASLOTA_ReadStatusReg() & ATA_SR_DRQ))
        ;

    // Read data from drive
    _ATASLOTA_ReadBuffer(Buffer);

    temp = _ATASLOTA_ReadStatusReg();
    if (temp & ATA_SR_ERR)
    {
        return 1; // If the error bit was set, fail.
    }

    return temp & ATA_SR_ERR;
}

// Writes sectors to the specified lba, for the specified slot
// Returns 0 on success, -1 on failure.
int _ATASLOTA_WriteSector(u64 lba, u32 *Buffer)
{
    u32 temp;

    // Wait for drive to be ready (BSY to clear)
    while (_ATASLOTA_ReadStatusReg() & ATA_SR_BSY)
        ;

    // Select the device differently based on 28 or 48bit mode
    if (ATASLOTADriveInfo.lba48Support)
    {
        // Select the device (ATA_HEAD_USE_LBA is 0x40 for master, 0x50 for slave)
        _ATASLOTA_WriteByte(ATA_REG_DEVICE, ATA_HEAD_USE_LBA);
    }
    else
    {
        // Select the device (ATA_HEAD_USE_LBA is 0x40 for master, 0x50 for slave)
        _ATASLOTA_WriteByte(ATA_REG_DEVICE, 0xE0 | (u8)((lba >> 24) & 0x0F));
    }

    // check if drive supports LBA 48-bit
    if (ATASLOTADriveInfo.lba48Support)
    {
        _ATASLOTA_WriteByte(ATA_REG_SECCOUNT, 0);                      // Sector count (Hi)
        _ATASLOTA_WriteByte(ATA_REG_LBALO, (u8)((lba >> 24) & 0xFF));  // LBA 4
        _ATASLOTA_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 32) & 0xFF)); // LBA 4
        _ATASLOTA_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 40) & 0xFF));  // LBA 5
        _ATASLOTA_WriteByte(ATA_REG_SECCOUNT, 1);                      // Sector count (Lo)
        _ATASLOTA_WriteByte(ATA_REG_LBALO, (u8)(lba & 0xFF));          // LBA 1
        _ATASLOTA_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 8) & 0xFF));  // LBA 2
        _ATASLOTA_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 16) & 0xFF));  // LBA 3
    }
    else
    {
        _ATASLOTA_WriteByte(ATA_REG_SECCOUNT, 1);                     // Sector count
        _ATASLOTA_WriteByte(ATA_REG_LBALO, (u8)(lba & 0xFF));         // LBA Lo
        _ATASLOTA_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 8) & 0xFF)); // LBA Mid
        _ATASLOTA_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 16) & 0xFF)); // LBA Hi
    }

    // Write the appropriate write command
    _ATASLOTA_WriteByte(ATA_REG_COMMAND, ATASLOTADriveInfo.lba48Support ? ATA_CMD_WRITESECTEXT : ATA_CMD_WRITESECT);

    // Wait for BSY to clear
    while ((temp = _ATASLOTA_ReadStatusReg()) & ATA_SR_BSY)
        ;

    // If the error bit was set, fail.
    if (temp & ATA_SR_ERR)
    {
#ifdef _ataslota_DEBUG
        printf("Error: %02X", _ATASLOTA_ReadErrorReg());
#endif

        return 1;
    }
    // Wait for drive to request data transfer
    while (!(_ATASLOTA_ReadStatusReg() & ATA_SR_DRQ))
        ;

    // Write data to the drive
    _ATASLOTA_WriteBuffer(Buffer);

    // Wait for the write to finish
    while (_ATASLOTA_ReadStatusReg() & ATA_SR_BSY)
        ;

    temp = _ATASLOTA_ReadStatusReg();
    if (temp & ATA_SR_ERR)
    {
        return 1; // If the error bit was set, fail.
    }

    return temp & ATA_SR_ERR;
}

// Wrapper to read a number of sectors
// 0 on Success, -1 on Error
int _ATASLOTA_ReadSectors(u64 sector, unsigned int numSectors, unsigned char *dest)
{
    int ret = 0;
    while (numSectors)
    {
#ifdef _ataslota_DEBUG
        printf("Reading, sec %08X, numSectors %i, dest %08X ..\r\n", (u32)(sector & 0xFFFFFFFF), numSectors, (u32)dest);
#endif

        if ((ret = _ATASLOTA_ReadSector(sector, (u32 *)dest)))
        {
#ifdef _ataslota_DEBUG
            printf("(%08X) Failed to read!..\r\n", ret);
#endif

            return -1;
        }

#ifdef _ataslota_DEBUG
        _ATASLOTA_PrintHddSector((u32 *)dest);
#endif

        dest += 512;
        sector++;
        numSectors--;
    }

    return 0;
}

// Wrapper to write a number of sectors
// 0 on Success, -1 on Error
int _ATASLOTA_WriteSectors(u64 sector, unsigned int numSectors, unsigned char *src)
{
    int ret = 0;
    while (numSectors)
    {
        if ((ret = _ATASLOTA_WriteSector(sector, (u32 *)src)))
        {
#ifdef _ataslota_DEBUG
            printf("(%08X) Failed to write!..\r\n", ret);
#endif

            return -1;
        }

        src += 512;
        sector++;
        numSectors--;
    }

    return 0;
}

bool ATASLOTA_IsDriveInserted()
{
    if (ATASLOTA_DriveInserted)
    {
        return true;
    }

    if (_ATASLOTA_DriveIdentify())
    {
        return false;
    }

    ATASLOTA_DriveInserted = true;

    return true;
}

int ATASLOTA_Shutdown()
{
    ATASLOTA_DriveInserted = 0;

    return 1;
}

static bool __ataslota_startup(void)
{
    return ATASLOTA_IsDriveInserted();
}

static bool __ataslota_isInserted(void)
{
    return ATASLOTA_IsDriveInserted();
}

static bool __ataslota_readSectors(sec_t sector, sec_t numSectors, void *buffer)
{
    return !_ATASLOTA_ReadSectors((u64)sector, numSectors, buffer);
}

static bool __ataslota_writeSectors(sec_t sector, sec_t numSectors, void *buffer)
{
    return !_ATASLOTA_WriteSectors((u64)sector, numSectors, buffer);
}

static bool __ataslota_clearStatus(void)
{
    return true;
}

static bool __ataslota_shutdown(void)
{
    return true;
}

const DISC_INTERFACE __io_ataslota = {
    DEVICE_TYPE_GC_ATASLOTA,
    FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_GAMECUBE_SLOTA,
    (FN_MEDIUM_STARTUP)&__ataslota_startup,
    (FN_MEDIUM_ISINSERTED)&__ataslota_isInserted,
    (FN_MEDIUM_READSECTORS)&__ataslota_readSectors,
    (FN_MEDIUM_WRITESECTORS)&__ataslota_writeSectors,
    (FN_MEDIUM_CLEARSTATUS)&__ataslota_clearStatus,
    (FN_MEDIUM_SHUTDOWN)&__ataslota_shutdown};
    