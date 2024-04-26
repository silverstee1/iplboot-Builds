/**
 * IDE-EXI Driver for GameCube.
 *
 * IDE EXI + M.2 SSD Bridge adapter for Slot B.
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
#include "ATASLOTB.h"

// #define _ataslotb_DEBUG

extern void usleep(int s);

u16 bufferB[256] ATTRIBUTE_ALIGN(32);
static bool ATASLOTB_DriveInserted = false;

// Drive information struct
typeDriveInfoB ATASLOTBDriveInfo;

// Returns 8 bits from the ATA Status register
inline u8 _ATASLOTB_ReadStatusReg()
{
    // read ATA_REG_CMDSTATUS1 | 0x00 (dummy)
    u16 dat = 0x1700;
    EXI_Lock(EXI_CHANNEL_1, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_1, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 2, EXI_WRITE);
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 1, EXI_READ);
    EXI_Deselect(EXI_CHANNEL_1);
    EXI_Unlock(EXI_CHANNEL_1);

    return *(u8 *)&dat;
}

// Returns 8 bits from the ATA Error register
inline u8 _ATASLOTB_ReadErrorReg()
{
    // read ATA_REG_ERROR | 0x00 (dummy)
    u16 dat = 0x1100;
    EXI_Lock(EXI_CHANNEL_1, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_1, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 2, EXI_WRITE);
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 1, EXI_READ);
    EXI_Deselect(EXI_CHANNEL_1);
    EXI_Unlock(EXI_CHANNEL_1);

    return *(u8 *)&dat;
}

// Writes 8 bits of data out to the specified ATA Register
inline void _ATASLOTB_WriteByte(u8 addr, u8 data)
{
    u32 dat = 0x80000000 | (addr << 24) | (data << 16);
    EXI_Lock(EXI_CHANNEL_1, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_1, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 3, EXI_WRITE);
    EXI_Deselect(EXI_CHANNEL_1);
    EXI_Unlock(EXI_CHANNEL_1);
}

// Writes 16 bits to the ATA Data register
inline void _ATASLOTB_WriteU16(u16 data)
{
    // write 16 bit to ATA_REG_DATA | data LSB | data MSB | 0x00 (dummy)
    u32 dat = 0xD0000000 | (((data >> 8) & 0xff) << 16) | ((data & 0xff) << 8);
    EXI_Lock(EXI_CHANNEL_1, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_1, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 4, EXI_WRITE);
    EXI_Deselect(EXI_CHANNEL_1);
    EXI_Unlock(EXI_CHANNEL_1);
}

// Returns 16 bits from the ATA Data register
inline u16 _ATASLOTB_ReadU16()
{
    // read 16 bit from ATA_REG_DATA | 0x00 (dummy)
    u16 dat = 0x5000;
    EXI_Lock(EXI_CHANNEL_1, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_1, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 2, EXI_WRITE);
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 2, EXI_READ); // read LSB & MSB
    EXI_Deselect(EXI_CHANNEL_1);
    EXI_Unlock(EXI_CHANNEL_1);

    return dat;
}

// Reads 512 bytes
inline void _ATASLOTB_ReadBuffer(u32 *dst)
{
    u16 dwords = 128; // 128 * 4 = 512 bytes
    // (31:29) 011b | (28:24) 10000b | (23:16) <num_words_LSB> | (15:8) <num_words_MSB> | (7:0) 00h (4 bytes)
    u32 dat = 0x70000000 | ((dwords & 0xff) << 16) | (((dwords >> 8) & 0xff) << 8);
    EXI_Lock(EXI_CHANNEL_1, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_1, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 4, EXI_WRITE);

    u32 *ptr = dst;
    if (((u32)dst) % 32)
    {
        ptr = (u32 *)memalign(32, 512);
    }

    DCInvalidateRange(ptr, 512);
    EXI_Dma(EXI_CHANNEL_1, ptr, 512, EXI_READ, NULL);
    EXI_Sync(EXI_CHANNEL_1);
    if (((u32)dst) % 32)
    {
        memcpy(dst, ptr, 512);
        free(ptr);
    }

    EXI_Deselect(EXI_CHANNEL_1);
    EXI_Unlock(EXI_CHANNEL_1);
}

inline void _ATASLOTB_WriteBuffer(u32 *src)
{
    u16 dwords = 128; // 128 * 4 = 512 bytes
    // (23:21) 111b | (20:16) 10000b | (15:8) <num_words_LSB> | (7:0) <num_words_MSB> (3 bytes)
    u32 dat = 0xF0000000 | ((dwords & 0xff) << 16) | (((dwords >> 8) & 0xff) << 8);
    EXI_Lock(EXI_CHANNEL_1, EXI_DEVICE_0, NULL);
    EXI_Select(EXI_CHANNEL_1, EXI_DEVICE_0, EXI_SPEED32MHZ);
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 3, EXI_WRITE);
    EXI_ImmEx(EXI_CHANNEL_1, src, 512, EXI_WRITE);
    dat = 0;
    EXI_ImmEx(EXI_CHANNEL_1, &dat, 1, EXI_WRITE); // Burn an extra cycle for the M.2 Loader to know to stop serving data
    EXI_Deselect(EXI_CHANNEL_1);
    EXI_Unlock(EXI_CHANNEL_1);
}

void _ATASLOTB_PrintHddSector(u32 *dest)
{
    int i = 0;
    for (i = 0; i < 512 / 4; i += 4)
    {
        printf("%08X:%08X %08X %08X %08X\r\n", i * 4, dest[i], dest[i + 1], dest[i + 2], dest[i + 3]);
    }
}

bool ATASLOTB_IsInserted()
{
    u32 cid = 0;
    EXI_GetID(EXI_CHANNEL_1, EXI_DEVICE_0, &cid);

    //return cid == EXI_ATASLOTB_ID;
	return (cid&~0xff) == EXI_ATASLOTB_ID;
}

// Sends the IDENTIFY command to the SSD
// Returns 0 on success, -1 otherwise
u32 _ATASLOTB_DriveIdentify()
{
    u16 tmp, retries = 50;
    u32 i = 0;

    memset(&ATASLOTBDriveInfo, 0, sizeof(typeDriveInfoB));

    // Select the device
    _ATASLOTB_WriteByte(ATA_REG_DEVICE, 0 /*ATA_HEAD_USE_LBA*/);

    // Wait for drive to be ready (BSY to clear) - 5 sec timeout
    do
    {
        tmp = _ATASLOTB_ReadStatusReg();
        usleep(100000); // sleep for 0.1 seconds
        retries--;
#ifdef _ataslotb_DEBUG
        printf("(%08X) Waiting for BSY to clear..\r\n", tmp);
#endif
    } while ((tmp & ATA_SR_BSY) && retries);
    if (!retries)
    {
#ifdef _ataslotb_DEBUG
        printf("Exceeded retries..\r\n");
#endif

        return -1;
    }

    // Write the identify command
    _ATASLOTB_WriteByte(ATA_REG_COMMAND, ATA_CMD_IDENTIFY);

    // Wait for drive to request data transfer - 1 sec timeout
    retries = 10;
    do
    {
        tmp = _ATASLOTB_ReadStatusReg();
        usleep(100000); // sleep for 0.1 seconds
        retries--;
#ifdef _ataslotb_DEBUG
        printf("(%08X) Waiting for DRQ to toggle..\r\n", tmp);
#endif
    } while ((!(tmp & ATA_SR_DRQ)) && retries);
    if (!retries)
    {
#ifdef _ataslotb_DEBUG
        printf("(%08X) Drive did not respond in time, failing M.2 Loader init..\r\n", tmp);
#endif

        return -1;
    }
    usleep(2000);

    u16 *ptr = (u16 *)(&bufferB[0]);

    // Read Identify data from drive
    for (i = 0; i < 256; i++)
    {
        tmp = _ATASLOTB_ReadU16(); // get data
        *ptr++ = bswap16(tmp);     // swap
    }

    // Get the info out of the Identify data buffer
    // From the command set, check if LBA48 is supported
    u16 commandSet = *(u16 *)(&bufferB[ATA_IDENT_COMMANDSET]);
    ATASLOTBDriveInfo.lba48Support = (commandSet >> 8) & ATA_IDENT_LBA48MASK;

    if (ATASLOTBDriveInfo.lba48Support)
    {
        u16 lbaHi = *(u16 *)(&bufferB[ATA_IDENT_LBA48SECTORS + 2]);
        u16 lbaMid = *(u16 *)(&bufferB[ATA_IDENT_LBA48SECTORS + 1]);
        u16 lbaLo = *(u16 *)(&bufferB[ATA_IDENT_LBA48SECTORS]);
        ATASLOTBDriveInfo.sizeInSectors = (u64)(((u64)lbaHi << 32) | (lbaMid << 16) | lbaLo);
        ATASLOTBDriveInfo.sizeInGigaBytes = (u32)((ATASLOTBDriveInfo.sizeInSectors << 9) / 1024 / 1024 / 1024);
    }
    else
    {
        ATASLOTBDriveInfo.cylinders = *(u16 *)(&bufferB[ATA_IDENT_CYLINDERS]);
        ATASLOTBDriveInfo.heads = *(u16 *)(&bufferB[ATA_IDENT_HEADS]);
        ATASLOTBDriveInfo.sectors = *(u16 *)(&bufferB[ATA_IDENT_SECTORS]);
        ATASLOTBDriveInfo.sizeInSectors = ((*(u16 *)&bufferB[ATA_IDENT_LBASECTORS + 1]) << 16) |
                                          (*(u16 *)&bufferB[ATA_IDENT_LBASECTORS]);
        ATASLOTBDriveInfo.sizeInGigaBytes = (u32)((ATASLOTBDriveInfo.sizeInSectors << 9) / 1024 / 1024 / 1024);
    }

    i = 20;
    // copy serial string
    memcpy(&ATASLOTBDriveInfo.serial[0], &bufferB[ATA_IDENT_SERIAL], 20);
    // cut off the string (usually has trailing spaces)
    while ((ATASLOTBDriveInfo.serial[i] == ' ' || !ATASLOTBDriveInfo.serial[i]) && i >= 0)
    {
        ATASLOTBDriveInfo.serial[i] = 0;
        i--;
    }
    // copy model string
    memcpy(&ATASLOTBDriveInfo.model[0], &bufferB[ATA_IDENT_MODEL], 40);
    // cut off the string (usually has trailing spaces)
    i = 40;
    while ((ATASLOTBDriveInfo.model[i] == ' ' || !ATASLOTBDriveInfo.model[i]) && i >= 0)
    {
        ATASLOTBDriveInfo.model[i] = 0;
        i--;
    }

#ifdef _ataslotb_DEBUG
    printf("%d GB SDD Connected\r\n", ATASLOTBDriveInfo.sizeInGigaBytes);
    printf("LBA 48-Bit Mode %s\r\n", ATASLOTBDriveInfo.lba48Support ? "Supported" : "Not Supported");
    if (!ATASLOTBDriveInfo.lba48Support)
    {
        printf("Cylinders: %i\r\n", ATASLOTBDriveInfo.cylinders);
        printf("Heads Per Cylinder: %i\r\n", ATASLOTBDriveInfo.heads);
        printf("Sectors Per Track: %i\r\n", ATASLOTBDriveInfo.sectors);
    }
    printf("Model: %s\r\n", ATASLOTBDriveInfo.model);
    printf("Serial: %s\r\n", ATASLOTBDriveInfo.serial);
    _ATASLOTB_PrintHddSector((u32 *)&bufferB);
#endif

#ifdef _ataslotb_DEBUG
    int unlockStatus = ATASLOTB_Unlock(1, "password\0", ATA_CMD_UNLOCK);
    printf("Unlock Status was: %i\r\n", unlockStatus);
#else
    ATASLOTB_Unlock(1, "password\0", ATA_CMD_UNLOCK);
#endif

#ifdef _ataslotb_DEBUG
    unlockStatus = ATASLOTB_Unlock(1, "password\0", ATA_CMD_SECURITY_DISABLE);
    printf("Disable Status was: %i\r\n", unlockStatus);
#else
    ATASLOTB_Unlock(1, "password\0", ATA_CMD_SECURITY_DISABLE);
#endif

    return 0;
}

// Unlocks a ATA HDD with a password
// Returns 0 on success, -1 on failure.
int ATASLOTB_Unlock(int useMaster, char *password, int command)
{
    u32 i;
    u16 tmp, retries = 50;

    // Select the device
    _ATASLOTB_WriteByte(ATA_REG_DEVICE, ATA_HEAD_USE_LBA);

    // Wait for drive to be ready (BSY to clear) - 5 sec timeout
    do
    {
        tmp = _ATASLOTB_ReadStatusReg();
        usleep(100000); // sleep for 0.1 seconds
        retries--;
#ifdef _ataslotb_DEBUG
        printf("UNLOCK (%08X) Waiting for BSY to clear..\r\n", tmp);
#endif
    } while ((tmp & ATA_SR_BSY) && retries);
    if (!retries)
    {
#ifdef _ataslotb_DEBUG
        printf("UNLOCK Exceeded retries..\r\n");
#endif

        return -1;
    }

    // Write the appropriate unlock command
    _ATASLOTB_WriteByte(ATA_REG_COMMAND, command);

    // Wait for drive to request data transfer - 1 sec timeout
    retries = 10;
    do
    {
        tmp = _ATASLOTB_ReadStatusReg();
        usleep(100000); // sleep for 0.1 seconds
        retries--;
#ifdef _ataslotb_DEBUG
        printf("UNLOCK (%08X) Waiting for DRQ to toggle..\r\n", tmp);
#endif
    } while ((!(tmp & ATA_SR_DRQ)) && retries);
    if (!retries)
    {
#ifdef _ataslotb_DEBUG
        printf("UNLOCK (%08X) Drive did not respond in time, failing M.2 Loader init..\r\n", tmp);
#endif

        return -1;
    }
    usleep(2000);

    // Fill an unlock struct
    unlockStructB unlock;
    memset(&unlock, 0, sizeof(unlockStructB));
    unlock.type = (u16)useMaster;
    memcpy(unlock.password, password, strlen(password));

    // write data to the drive
    u16 *ptr = (u16 *)&unlock;
    for (i = 0; i < 256; i++)
    {
        ptr[i] = bswap16(ptr[i]);
        _ATASLOTB_WriteU16(ptr[i]);
    }

    // Wait for BSY to clear
    u32 temp = 0;
    while ((temp = _ATASLOTB_ReadStatusReg()) & ATA_SR_BSY)
        ;

    // If the error bit was set, fail.
    if (temp & ATA_SR_ERR)
    {
#ifdef _ataslotb_DEBUG
        printf("Error: %02X\r\n", _ATASLOTB_ReadErrorReg());
#endif

        return 1;
    }

    return !(_ATASLOTB_ReadErrorReg() & ATA_ER_ABRT);
}

// Reads sectors from the specified lba, for the specified slot
// Returns 0 on success, -1 on failure.
int _ATASLOTB_ReadSector(u64 lba, u32 *Buffer)
{
    u32 temp = 0;

    // Wait for drive to be ready (BSY to clear)
    while (_ATASLOTB_ReadStatusReg() & ATA_SR_BSY)
        ;

    // Select the device differently based on 28 or 48bit mode
    if (ATASLOTBDriveInfo.lba48Support)
    {
        // Select the device (ATA_HEAD_USE_LBA is 0x40 for master, 0x50 for slave)
        _ATASLOTB_WriteByte(ATA_REG_DEVICE, ATA_HEAD_USE_LBA);
    }
    else
    {
        // Select the device (ATA_HEAD_USE_LBA is 0x40 for master, 0x50 for slave)
        _ATASLOTB_WriteByte(ATA_REG_DEVICE, 0xE0 | (u8)((lba >> 24) & 0x0F));
    }

    // check if drive supports LBA 48-bit
    if (ATASLOTBDriveInfo.lba48Support)
    {
        _ATASLOTB_WriteByte(ATA_REG_SECCOUNT, 0);                      // Sector count (Hi)
        _ATASLOTB_WriteByte(ATA_REG_LBALO, (u8)((lba >> 24) & 0xFF));  // LBA 4
        _ATASLOTB_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 32) & 0xFF)); // LBA 5
        _ATASLOTB_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 40) & 0xFF));  // LBA 6
        _ATASLOTB_WriteByte(ATA_REG_SECCOUNT, 1);                      // Sector count (Lo)
        _ATASLOTB_WriteByte(ATA_REG_LBALO, (u8)(lba & 0xFF));          // LBA 1
        _ATASLOTB_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 8) & 0xFF));  // LBA 2
        _ATASLOTB_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 16) & 0xFF));  // LBA 3
    }
    else
    {
        _ATASLOTB_WriteByte(ATA_REG_SECCOUNT, 1);                     // Sector count
        _ATASLOTB_WriteByte(ATA_REG_LBALO, (u8)(lba & 0xFF));         // LBA Lo
        _ATASLOTB_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 8) & 0xFF)); // LBA Mid
        _ATASLOTB_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 16) & 0xFF)); // LBA Hi
    }

    // Write the appropriate read command
    _ATASLOTB_WriteByte(ATA_REG_COMMAND, ATASLOTBDriveInfo.lba48Support ? ATA_CMD_READSECTEXT : ATA_CMD_READSECT);

    // Wait for BSY to clear
    while ((temp = _ATASLOTB_ReadStatusReg()) & ATA_SR_BSY)
        ;

    // If the error bit was set, fail.
    if (temp & ATA_SR_ERR)
    {
#ifdef _ataslotb_DEBUG
        printf("Error: %02X", _ATASLOTB_ReadErrorReg());
#endif

        return 1;
    }

    // Wait for drive to request data transfer
    while (!(_ATASLOTB_ReadStatusReg() & ATA_SR_DRQ))
        ;

    // Read data from drive
    _ATASLOTB_ReadBuffer(Buffer);

    temp = _ATASLOTB_ReadStatusReg();
    if (temp & ATA_SR_ERR)
    {
        return 1; // If the error bit was set, fail.
    }

    return temp & ATA_SR_ERR;
}

// Writes sectors to the specified lba, for the specified slot
// Returns 0 on success, -1 on failure.
int _ATASLOTB_WriteSector(u64 lba, u32 *Buffer)
{
    u32 temp;

    // Wait for drive to be ready (BSY to clear)
    while (_ATASLOTB_ReadStatusReg() & ATA_SR_BSY)
        ;

    // Select the device differently based on 28 or 48bit mode
    if (ATASLOTBDriveInfo.lba48Support)
    {
        // Select the device (ATA_HEAD_USE_LBA is 0x40 for master, 0x50 for slave)
        _ATASLOTB_WriteByte(ATA_REG_DEVICE, ATA_HEAD_USE_LBA);
    }
    else
    {
        // Select the device (ATA_HEAD_USE_LBA is 0x40 for master, 0x50 for slave)
        _ATASLOTB_WriteByte(ATA_REG_DEVICE, 0xE0 | (u8)((lba >> 24) & 0x0F));
    }

    // check if drive supports LBA 48-bit
    if (ATASLOTBDriveInfo.lba48Support)
    {
        _ATASLOTB_WriteByte(ATA_REG_SECCOUNT, 0);                      // Sector count (Hi)
        _ATASLOTB_WriteByte(ATA_REG_LBALO, (u8)((lba >> 24) & 0xFF));  // LBA 4
        _ATASLOTB_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 32) & 0xFF)); // LBA 4
        _ATASLOTB_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 40) & 0xFF));  // LBA 5
        _ATASLOTB_WriteByte(ATA_REG_SECCOUNT, 1);                      // Sector count (Lo)
        _ATASLOTB_WriteByte(ATA_REG_LBALO, (u8)(lba & 0xFF));          // LBA 1
        _ATASLOTB_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 8) & 0xFF));  // LBA 2
        _ATASLOTB_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 16) & 0xFF));  // LBA 3
    }
    else
    {
        _ATASLOTB_WriteByte(ATA_REG_SECCOUNT, 1);                     // Sector count
        _ATASLOTB_WriteByte(ATA_REG_LBALO, (u8)(lba & 0xFF));         // LBA Lo
        _ATASLOTB_WriteByte(ATA_REG_LBAMID, (u8)((lba >> 8) & 0xFF)); // LBA Mid
        _ATASLOTB_WriteByte(ATA_REG_LBAHI, (u8)((lba >> 16) & 0xFF)); // LBA Hi
    }

    // Write the appropriate write command
    _ATASLOTB_WriteByte(ATA_REG_COMMAND, ATASLOTBDriveInfo.lba48Support ? ATA_CMD_WRITESECTEXT : ATA_CMD_WRITESECT);

    // Wait for BSY to clear
    while ((temp = _ATASLOTB_ReadStatusReg()) & ATA_SR_BSY)
        ;

    // If the error bit was set, fail.
    if (temp & ATA_SR_ERR)
    {
#ifdef _ataslotb_DEBUG
        printf("Error: %02X", _ATASLOTB_ReadErrorReg());
#endif

        return 1;
    }
    // Wait for drive to request data transfer
    while (!(_ATASLOTB_ReadStatusReg() & ATA_SR_DRQ))
        ;

    // Write data to the drive
    _ATASLOTB_WriteBuffer(Buffer);

    // Wait for the write to finish
    while (_ATASLOTB_ReadStatusReg() & ATA_SR_BSY)
        ;

    temp = _ATASLOTB_ReadStatusReg();
    if (temp & ATA_SR_ERR)
    {
        return 1; // If the error bit was set, fail.
    }

    return temp & ATA_SR_ERR;
}

// Wrapper to read a number of sectors
// 0 on Success, -1 on Error
int _ATASLOTB_ReadSectors(u64 sector, unsigned int numSectors, unsigned char *dest)
{
    int ret = 0;
    while (numSectors)
    {
#ifdef _ataslotb_DEBUG
        printf("Reading, sec %08X, numSectors %i, dest %08X ..\r\n", (u32)(sector & 0xFFFFFFFF), numSectors, (u32)dest);
#endif

        if ((ret = _ATASLOTB_ReadSector(sector, (u32 *)dest)))
        {
#ifdef _ataslotb_DEBUG
            printf("(%08X) Failed to read!..\r\n", ret);
#endif

            return -1;
        }

#ifdef _ataslotb_DEBUG
        _ATASLOTB_PrintHddSector((u32 *)dest);
#endif

        dest += 512;
        sector++;
        numSectors--;
    }

    return 0;
}

// Wrapper to write a number of sectors
// 0 on Success, -1 on Error
int _ATASLOTB_WriteSectors(u64 sector, unsigned int numSectors, unsigned char *src)
{
    int ret = 0;
    while (numSectors)
    {
        if ((ret = _ATASLOTB_WriteSector(sector, (u32 *)src)))
        {
#ifdef _ataslotb_DEBUG
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

bool ATASLOTB_IsDriveInserted()
{
    if (ATASLOTB_DriveInserted)
    {
        return true;
    }

    if (_ATASLOTB_DriveIdentify())
    {
        return false;
    }

    ATASLOTB_DriveInserted = true;

    return true;
}

int ATASLOTB_Shutdown()
{
    ATASLOTB_DriveInserted = 0;

    return 1;
}

static bool __ataslotb_startup(void)
{
    return ATASLOTB_IsDriveInserted();
}

static bool __ataslotb_isInserted(void)
{
    return ATASLOTB_IsDriveInserted();
}

static bool __ataslotb_readSectors(sec_t sector, sec_t numSectors, void *buffer)
{
    return !_ATASLOTB_ReadSectors((u64)sector, numSectors, buffer);
}

static bool __ataslotb_writeSectors(sec_t sector, sec_t numSectors, void *buffer)
{
    return !_ATASLOTB_WriteSectors((u64)sector, numSectors, buffer);
}

static bool __ataslotb_clearStatus(void)
{
    return true;
}

static bool __ataslotb_shutdown(void)
{
    return true;
}

const DISC_INTERFACE __io_ataslotb = {
    DEVICE_TYPE_GC_ATASLOTB,
    FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE | FEATURE_GAMECUBE_SLOTB,
    (FN_MEDIUM_STARTUP)&__ataslotb_startup,
    (FN_MEDIUM_ISINSERTED)&__ataslotb_isInserted,
    (FN_MEDIUM_READSECTORS)&__ataslotb_readSectors,
    (FN_MEDIUM_WRITESECTORS)&__ataslotb_writeSectors,
    (FN_MEDIUM_CLEARSTATUS)&__ataslotb_clearStatus,
    (FN_MEDIUM_SHUTDOWN)&__ataslotb_shutdown};
    