/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
*   Mupen64plus - dd_controller.c                                         *
*   Mupen64Plus homepage: https://mupen64plus.org/                        *
*   Copyright (C) 2015 LuigiBlood                                         *
*                                                                         *
*   This program is free software; you can redistribute it and/or modify  *
*   it under the terms of the GNU General Public License as published by  *
*   the Free Software Foundation; either version 2 of the License, or     *
*   (at your option) any later version.                                   *
*                                                                         *
*   This program is distributed in the hope that it will be useful,       *
*   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
*   GNU General Public License for more details.                          *
*                                                                         *
*   You should have received a copy of the GNU General Public License     *
*   along with this program; if not, write to the                         *
*   Free Software Foundation, Inc.,                                       *
*   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "dd_controller.h"

#include <assert.h>
#include <string.h>
#include <time.h>

#define M64P_CORE_PROTOTYPES 1
#include "api/m64p_types.h"
#include "api/callbacks.h"
#include "backends/api/clock_backend.h"
#include "backends/api/storage_backend.h"
#include "device/device.h"
#include "device/memory/memory.h"
#include "device/r4300/r4300_core.h"

/* dd commands definition */
#define DD_CMD_NOOP             UINT32_C(0x00000000)
#define DD_CMD_SEEK_READ        UINT32_C(0x00010001)
#define DD_CMD_SEEK_WRITE       UINT32_C(0x00020001)
#define DD_CMD_RECALIBRATE      UINT32_C(0x00030001) // ???
#define DD_CMD_SLEEP            UINT32_C(0x00040000)
#define DD_CMD_START            UINT32_C(0x00050001)
#define DD_CMD_SET_STANDBY      UINT32_C(0x00060000)
#define DD_CMD_SET_SLEEP        UINT32_C(0x00070000)
#define DD_CMD_CLR_DSK_CHNG     UINT32_C(0x00080000)
#define DD_CMD_CLR_RESET        UINT32_C(0x00090000)
#define DD_CMD_READ_VERSION     UINT32_C(0x000A0000)
#define DD_CMD_SET_DISK_TYPE    UINT32_C(0x000B0001)
#define DD_CMD_REQUEST_STATUS   UINT32_C(0x000C0000)
#define DD_CMD_STANDBY          UINT32_C(0x000D0000)
#define DD_CMD_IDX_LOCK_RETRY   UINT32_C(0x000E0000) // ???
#define DD_CMD_SET_YEAR_MONTH   UINT32_C(0x000F0000)
#define DD_CMD_SET_DAY_HOUR     UINT32_C(0x00100000)
#define DD_CMD_SET_MIN_SEC      UINT32_C(0x00110000)
#define DD_CMD_GET_YEAR_MONTH   UINT32_C(0x00120000)
#define DD_CMD_GET_DAY_HOUR     UINT32_C(0x00130000)
#define DD_CMD_GET_MIN_SEC      UINT32_C(0x00140000)
#define DD_CMD_FEATURE_INQ      UINT32_C(0x001B0000)

/* dd status register bitfields definition */
#define DD_STATUS_DATA_RQ       UINT32_C(0x40000000)
#define DD_STATUS_C2_XFER       UINT32_C(0x10000000)
#define DD_STATUS_BM_ERR        UINT32_C(0x08000000)
#define DD_STATUS_BM_INT        UINT32_C(0x04000000)
#define DD_STATUS_MECHA_INT     UINT32_C(0x02000000)
#define DD_STATUS_DISK_PRES     UINT32_C(0x01000000)
#define DD_STATUS_BUSY_STATE    UINT32_C(0x00800000)
#define DD_STATUS_RST_STATE     UINT32_C(0x00400000)
#define DD_STATUS_MTR_N_SPIN    UINT32_C(0x00100000)
#define DD_STATUS_HEAD_RTRCT    UINT32_C(0x00080000)
#define DD_STATUS_WR_PR_ERR     UINT32_C(0x00040000)
#define DD_STATUS_MECHA_ERR     UINT32_C(0x00020000)
#define DD_STATUS_DISK_CHNG     UINT32_C(0x00010000)

/* dd bm status register bitfields definition */
/* read flags */
#define DD_BM_STATUS_RUNNING    UINT32_C(0x80000000)
#define DD_BM_STATUS_ERROR      UINT32_C(0x04000000)
#define DD_BM_STATUS_MICRO      UINT32_C(0x02000000) /* ??? */
#define DD_BM_STATUS_BLOCK      UINT32_C(0x01000000)
#define DD_BM_STATUS_C1CRR      UINT32_C(0x00800000)
#define DD_BM_STATUS_C1DBL      UINT32_C(0x00400000)
#define DD_BM_STATUS_C1SNG      UINT32_C(0x00200000)
#define DD_BM_STATUS_C1ERR      UINT32_C(0x00010000) /* Typo ??? */
/* write flags */
#define DD_BM_CTL_START         UINT32_C(0x80000000)
#define DD_BM_CTL_MNGRMODE      UINT32_C(0x40000000)
#define DD_BM_CTL_INTMASK       UINT32_C(0x20000000)
#define DD_BM_CTL_RESET         UINT32_C(0x10000000)
#define DD_BM_CTL_DIS_OR_CHK    UINT32_C(0x08000000) /* ??? */
#define DD_BM_CTL_DIS_C1_CRR    UINT32_C(0x04000000)
#define DD_BM_CTL_BLK_TRANS     UINT32_C(0x02000000)
#define DD_BM_CTL_MECHA_RST     UINT32_C(0x01000000)

#define DD_TRACK_LOCK           UINT32_C(0x60000000)

/* disk geometry definitions */
enum { SECTORS_PER_BLOCK = 85 };
enum { BLOCKS_PER_TRACK  = 2  };

enum { DD_DISK_SYSTEM_DATA_SIZE = 0xe8 };

static const unsigned int zone_sec_size[16] = {
    232, 216, 208, 192, 176, 160, 144, 128,
    216, 208, 192, 176, 160, 144, 128, 112
};
static const unsigned int zone_sec_size_phys[9] = {
    232, 216, 208, 192, 176, 160, 144, 128, 112
};

static const uint32_t ZoneTracks[16] = {
    158, 158, 149, 149, 149, 149, 149, 114,
    158, 158, 149, 149, 149, 149, 149, 114
};
static const uint32_t DiskTypeZones[7][16] = {
    { 0, 1, 2, 9, 8, 3, 4, 5, 6, 7, 15, 14, 13, 12, 11, 10 },
    { 0, 1, 2, 3, 10, 9, 8, 4, 5, 6, 7, 15, 14, 13, 12, 11 },
    { 0, 1, 2, 3, 4, 11, 10, 9, 8, 5, 6, 7, 15, 14, 13, 12 },
    { 0, 1, 2, 3, 4, 5, 12, 11, 10, 9, 8, 6, 7, 15, 14, 13 },
    { 0, 1, 2, 3, 4, 5, 6, 13, 12, 11, 10, 9, 8, 7, 15, 14 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 14, 13, 12, 11, 10, 9, 8, 15 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 15, 14, 13, 12, 11, 10, 9, 8 }
};
static const uint32_t RevDiskTypeZones[7][16] = {
    { 0, 1, 2, 5, 6, 7, 8, 9, 4, 3, 15, 14, 13, 12, 11, 10 },
    { 0, 1, 2, 3, 7, 8, 9, 10, 6, 5, 4, 15, 14, 13, 12, 11 },
    { 0, 1, 2, 3, 4, 9, 10, 11, 8, 7, 6, 5, 15, 14, 13, 12 },
    { 0, 1, 2, 3, 4, 5, 11, 12, 10, 9, 8, 7, 6, 15, 14, 13 },
    { 0, 1, 2, 3, 4, 5, 6, 13, 12, 11, 10, 9, 8, 7, 15, 14 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 14, 13, 12, 11, 10, 9, 8, 15 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 15, 14, 13, 12, 11, 10, 9, 8 }
};
static const uint32_t StartBlock[7][16] = {
    { 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1 },
    { 0, 0, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 0 },
    { 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 1, 0, 1, 1 },
    { 0, 0, 0, 1, 0, 1, 1, 0, 1, 1, 0, 1, 0, 1, 0, 0 },
    { 0, 0, 0, 1, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 1, 1 },
    { 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 },
    { 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 1 }
};
const uint16_t VZoneLBATable[7][16] = {
    {0x0124, 0x0248, 0x035A, 0x047E, 0x05A2, 0x06B4, 0x07C6, 0x08D8, 0x09EA, 0x0AB6, 0x0B82, 0x0C94, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x06A2, 0x07C6, 0x08D8, 0x09EA, 0x0AFC, 0x0BC8, 0x0C94, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08C6, 0x09EA, 0x0AFC, 0x0C0E, 0x0CDA, 0x0DA6, 0x0EB8, 0x0FCA, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AEA, 0x0C0E, 0x0D20, 0x0DEC, 0x0EB8, 0x0FCA, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x08B4, 0x09C6, 0x0AD8, 0x0BEA, 0x0D0E, 0x0E32, 0x0EFE, 0x0FCA, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x086E, 0x0980, 0x0A92, 0x0BA4, 0x0CB6, 0x0DC8, 0x0EEC, 0x1010, 0x10DC},
    {0x0124, 0x0248, 0x035A, 0x046C, 0x057E, 0x0690, 0x07A2, 0x086E, 0x093A, 0x0A4C, 0x0B5E, 0x0C70, 0x0D82, 0x0E94, 0x0FB8, 0x10DC}
};
const uint16_t TrackZoneTable[2][8] = {
    {0x000, 0x09E, 0x13C, 0x1D1, 0x266, 0x2FB, 0x390, 0x425},
    {0x091, 0x12F, 0x1C4, 0x259, 0x2EE, 0x383, 0x418, 0x48A}
};
const uint8_t VZONE_PZONE_TBL[7][16] = {
    {0x0, 0x1, 0x2, 0x9, 0x8, 0x3, 0x4, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC, 0xB, 0xA},
    {0x0, 0x1, 0x2, 0x3, 0xA, 0x9, 0x8, 0x4, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC, 0xB},
    {0x0, 0x1, 0x2, 0x3, 0x4, 0xB, 0xA, 0x9, 0x8, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC},
    {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0xC, 0xB, 0xA, 0x9, 0x8, 0x6, 0x7, 0xF, 0xE, 0xD},
    {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8, 0x7, 0xF, 0xE},
    {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8, 0xF},
    {0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0xF, 0xE, 0xD, 0xC, 0xB, 0xA, 0x9, 0x8}
};


#define BLOCKSIZE(_zone) zone_sec_size[_zone] * SECTORS_PER_BLOCK
#define TRACKSIZE(_zone) BLOCKSIZE(_zone) * BLOCKS_PER_TRACK
#define ZONESIZE(_zone) TRACKSIZE(_zone) * ZoneTracks[_zone]
#define VZONESIZE(_zone) TRACKSIZE(_zone) * (ZoneTracks[_zone] - 0xC)
#define VZoneToPZone(x, y) VZONE_PZONE_TBL[y][x]

#define MAX_LBA             0x10DB
#define SIZE_LBA            MAX_LBA+1
#define SYSTEM_LBAS         24
#define DISKID_LBA          14




static uint8_t byte2bcd(int n)
{
    n %= 100;
    return (uint8_t)(((n / 10) << 4) | (n % 10));
}

static uint32_t time2data(int hi, int lo)
{
    return ((uint32_t)byte2bcd(hi) << 24)
         | ((uint32_t)byte2bcd(lo) << 16);
}

static void update_rtc(struct dd_rtc* rtc)
{
    /* update rtc->now */
    time_t now = rtc->iclock->get_time(rtc->clock);
    rtc->now += now - rtc->last_update_rtc;
    rtc->last_update_rtc = now;
}

static void signal_dd_interrupt(struct dd_controller* dd, uint32_t bm_int)
{
    dd->regs[DD_ASIC_CMD_STATUS] |= bm_int;
    r4300_check_interrupt(dd->r4300, CP0_CAUSE_IP3, 1);
}

static void clear_dd_interrupt(struct dd_controller* dd, uint32_t bm_int)
{
    dd->regs[DD_ASIC_CMD_STATUS] &= ~bm_int;
    r4300_check_interrupt(dd->r4300, CP0_CAUSE_IP3, 0);
}

static void read_C2(struct dd_controller* dd)
{
    size_t i;

    size_t length = zone_sec_size[dd->bm_zone];
    size_t offset = 0x40 * (dd->regs[DD_ASIC_CUR_SECTOR] - SECTORS_PER_BLOCK);

    DebugMessage(M64MSG_VERBOSE, "read C2: length=%08x, offset=%08x",
            (uint32_t)length, (uint32_t)offset);

    for (i = 0; i < length; ++i) {
        dd->c2s_buf[(offset + i) ^ 3] = 0;
    }
}

static void read_sector(struct dd_controller* dd)
{
    struct extra_storage_disk* extra = (struct extra_storage_disk*)dd->idisk->extra(dd->disk);

    size_t i;
    const uint8_t* disk_mem = dd->idisk->data(dd->disk);
    size_t offset = dd->bm_track_offset;
    size_t length = dd->regs[DD_ASIC_HOST_SECBYTE] + 1;

    for (i = 0; i < length; ++i) {
        dd->ds_buf[i ^ 3] = disk_mem[offset + i];
    }
}

static void write_sector(struct dd_controller* dd)
{
    struct extra_storage_disk* extra = (struct extra_storage_disk*)dd->idisk->extra(dd->disk);

    size_t i;
    uint8_t* disk_mem = dd->idisk->data(dd->disk);
    size_t offset = dd->bm_track_offset;
    size_t length = zone_sec_size[dd->bm_zone];

    if (extra->format != DISK_FORMAT_MAME)
    {
        length = dd->regs[DD_ASIC_HOST_SECBYTE] + 1;
    }

	for (i = 0; i < length; ++i) {
		disk_mem[offset + i] = dd->ds_buf[i ^ 3];
    }

#if 0 /* disabled for now, because it causes too much slowdowns */
    dd->idisk->save(dd->disk);
#endif
}

static void seek_track(struct dd_controller* dd)
{
    struct extra_storage_disk* extra = (struct extra_storage_disk*)dd->idisk->extra(dd->disk);

    if (extra->format == DISK_FORMAT_MAME)
    {
        //MAME Format Seek
        static const unsigned int start_offset[] = {
            0x0000000, 0x05f15e0, 0x0b79d00, 0x10801a0,
            0x1523720, 0x1963d80, 0x1d414c0, 0x20bbce0,
            0x23196e0, 0x28a1e00, 0x2df5dc0, 0x3299340,
            0x36d99a0, 0x3ab70e0, 0x3e31900, 0x4149200
        };

        static const unsigned int tracks[] = {
            0x000, 0x09e, 0x13c, 0x1d1, 0x266, 0x2fb, 0x390, 0x425
        };

        unsigned int tr_off;
        unsigned int head_x_8 = ((dd->regs[DD_ASIC_CUR_TK] & 0x1000) >> 9);
        unsigned int track = (dd->regs[DD_ASIC_CUR_TK] & 0x0fff);

        /* find track bm_zone */
        for (dd->bm_zone = 7; dd->bm_zone > 0; --dd->bm_zone) {
            if (track >= tracks[dd->bm_zone]) {
                break;
            }
        }

        tr_off = track - tracks[dd->bm_zone];

        /* set zone and track offset */
        dd->bm_zone += head_x_8;
        dd->bm_track_offset = start_offset[dd->bm_zone] + tr_off * TRACKSIZE(dd->bm_zone)
            + dd->bm_block * BLOCKSIZE(dd->bm_zone)
            + (dd->regs[DD_ASIC_CUR_SECTOR] - dd->bm_write) * zone_sec_size[dd->bm_zone];

        if (dd->regs[DD_ASIC_CUR_SECTOR] == 0)
        {
            uint16_t block = dd->bm_track_offset / 0x4D08;
            uint16_t block_sys = ((struct extra_storage_disk*)dd->idisk->extra(dd->disk))->offset_sys / 0x4D08;
            uint16_t block_id = ((struct extra_storage_disk*)dd->idisk->extra(dd->disk))->offset_id / 0x4D08;
            if (block < 12 && block != block_sys)
                dd->regs[DD_ASIC_BM_STATUS_CTL] |= DD_BM_STATUS_MICRO;
            else if (block > 12 && block < 16 && block != block_id)
                dd->regs[DD_ASIC_BM_STATUS_CTL] |= DD_BM_STATUS_MICRO;
        }
    }
    else if (extra->format == DISK_FORMAT_SDK)
    {
        //SDK Format Seek
        uint16_t head = ((dd->regs[DD_ASIC_CUR_TK] & 0x1000) / 0x1000);
        uint16_t track = (dd->regs[DD_ASIC_CUR_TK] & 0x0fff);
        uint16_t block = dd->bm_block;
        uint16_t sector = dd->regs[DD_ASIC_CUR_SECTOR] - dd->bm_write;
        uint16_t sectorsize = dd->regs[DD_ASIC_HOST_SECBYTE] + 1;
        uint16_t lba = PhysToLBA(dd, head, track, block);
        //dd->bm_zone = LBAToVZone(dd, PhysToLBA(dd, head, track, block));

        dd->bm_track_offset = LBAToByte(dd, 0, lba) + sector * sectorsize;

        //Handle Errors for wrong System Data
        if (sector == 0)
        {
            uint16_t block = dd->bm_track_offset / 0x4D08;
            uint16_t block_sys = ((struct extra_storage_disk*)dd->idisk->extra(dd->disk))->offset_sys / 0x4D08;
            uint16_t block_id = ((struct extra_storage_disk*)dd->idisk->extra(dd->disk))->offset_id / 0x4D08;
            if (block < 12 && block != block_sys)
                dd->regs[DD_ASIC_BM_STATUS_CTL] |= DD_BM_STATUS_MICRO;
            else if (block > 12 && block < 16 && block != block_id)
                dd->regs[DD_ASIC_BM_STATUS_CTL] |= DD_BM_STATUS_MICRO;
        }
    }
    else //if (extra->format == DISK_FORMAT_D64)
    {
        //D64 Format Seek
    }
}

void dd_update_bm(void* opaque)
{
    struct dd_controller* dd = (struct dd_controller*)opaque;

    /* not running */
	if ((dd->regs[DD_ASIC_BM_STATUS_CTL] & DD_BM_STATUS_RUNNING) == 0) {
		return;
    }

    /* handle writes (BM mode 0) */
    if (dd->bm_write) {
        /* first sector : just issue a BM interrupt to get things going */
        if (dd->regs[DD_ASIC_CUR_SECTOR] == 0) {
            ++dd->regs[DD_ASIC_CUR_SECTOR];
            dd->regs[DD_ASIC_CMD_STATUS] |= DD_STATUS_DATA_RQ;
        }
        /* subsequent sectors: write previous sector */
        else if (dd->regs[DD_ASIC_CUR_SECTOR] < SECTORS_PER_BLOCK) {
            seek_track(dd);
            write_sector(dd);
            ++dd->regs[DD_ASIC_CUR_SECTOR];
            dd->regs[DD_ASIC_CMD_STATUS] |= DD_STATUS_DATA_RQ;
        }
        /* otherwise write last sector */
        else if (dd->regs[DD_ASIC_CUR_SECTOR] < SECTORS_PER_BLOCK + 1) {
            /* continue to next block */
            if (dd->regs[DD_ASIC_BM_STATUS_CTL] & DD_BM_STATUS_BLOCK) {
                seek_track(dd);
                write_sector(dd);
                dd->bm_block = 1 - dd->bm_block;
                dd->regs[DD_ASIC_CUR_SECTOR] = 1;
                dd->regs[DD_ASIC_BM_STATUS_CTL] &= ~DD_BM_STATUS_BLOCK;
                dd->regs[DD_ASIC_CMD_STATUS] |= DD_STATUS_DATA_RQ;
            /* quit writing after second block */
            } else {
                seek_track(dd);
                write_sector(dd);
                ++dd->regs[DD_ASIC_CUR_SECTOR];
                dd->regs[DD_ASIC_BM_STATUS_CTL] &= ~DD_BM_STATUS_RUNNING;
            }
        }
        else {
            DebugMessage(M64MSG_ERROR, "DD Write, sector overrun");
        }
    }
    /* handle reads (BM mode 1) */
    else {
        uint8_t dev = ((struct extra_storage_disk*)dd->idisk->extra(dd->disk))->development;
        /* track 6 fails to read on retail units (XXX: retail test) */
        if (((dd->regs[DD_ASIC_CUR_TK] & 0x1fff) == 6) && dd->bm_block == 0 && !dev) {
            dd->regs[DD_ASIC_CMD_STATUS] &= ~DD_STATUS_DATA_RQ;
            dd->regs[DD_ASIC_BM_STATUS_CTL] |= DD_BM_STATUS_MICRO;
        }
        /* data sectors : read sector and signal BM interrupt */
        else if (dd->regs[DD_ASIC_CUR_SECTOR] < SECTORS_PER_BLOCK) {
            seek_track(dd);
            read_sector(dd);
            ++dd->regs[DD_ASIC_CUR_SECTOR];
            dd->regs[DD_ASIC_CMD_STATUS] |= DD_STATUS_DATA_RQ;
        }
        /* C2 sectors: do nothing since they're loaded with zeros */
        else if (dd->regs[DD_ASIC_CUR_SECTOR] < SECTORS_PER_BLOCK + 4) {
            read_C2(dd);
            ++dd->regs[DD_ASIC_CUR_SECTOR];
            if (dd->regs[DD_ASIC_CUR_SECTOR] == SECTORS_PER_BLOCK + 4) {
                dd->regs[DD_ASIC_CMD_STATUS] |= DD_STATUS_C2_XFER;
            }
        }
        /* Gap sector: continue to next block, quit after second block */
        else if (dd->regs[DD_ASIC_CUR_SECTOR] == SECTORS_PER_BLOCK + 4) {
            if (dd->regs[DD_ASIC_BM_STATUS_CTL] & DD_BM_STATUS_BLOCK) {
                dd->bm_block = 1 - dd->bm_block;
                dd->regs[DD_ASIC_CUR_SECTOR] = 0;
                dd->regs[DD_ASIC_BM_STATUS_CTL] &= ~DD_BM_STATUS_BLOCK;
            }
            else {
                dd->regs[DD_ASIC_BM_STATUS_CTL] &= ~DD_BM_STATUS_RUNNING;
            }
        }
        else {
            DebugMessage(M64MSG_ERROR, "DD Read, sector overrun");
        }
    }

    /* Signal a BM interrupt */
    signal_dd_interrupt(dd, DD_STATUS_BM_INT);
}



void init_dd(struct dd_controller* dd,
             void* clock, const struct clock_backend_interface* iclock,
             const uint32_t* rom, size_t rom_size,
             void* disk, const struct storage_backend_interface* idisk,
             struct r4300_core* r4300)
{
    dd->rtc.clock = clock;
    dd->rtc.iclock = iclock;

    dd->rom = rom;
    dd->rom_size = rom_size;

    dd->disk = disk;
    dd->idisk = idisk;

    GenerateLBAToPhysTable(dd);

    dd->r4300 = r4300;
}

void poweron_dd(struct dd_controller* dd)
{
    memset(dd->regs, 0, DD_ASIC_REGS_COUNT*sizeof(dd->regs[0]));
    memset(dd->c2s_buf, 0, 0x400);
    memset(dd->ds_buf, 0, 0x100);
    memset(dd->ms_ram, 0, 0x40);

    dd->bm_write = 0;
    dd->bm_reset_held = 0;
    dd->bm_block = 0;
    dd->bm_zone = 0;
    dd->bm_track_offset = 0;

    dd->rtc.now = 0;
    dd->rtc.last_update_rtc = 0;

    dd->regs[DD_ASIC_ID_REG] = 0x00030000;
    dd->regs[DD_ASIC_CMD_STATUS] |= DD_STATUS_RST_STATE;
    if (dd->idisk != NULL) {
        dd->regs[DD_ASIC_CMD_STATUS] |= DD_STATUS_DISK_PRES;
        if (((struct extra_storage_disk*)dd->idisk->extra(dd->disk))->development)
            dd->regs[DD_ASIC_ID_REG] = 0x00040000;
    }
}

void read_dd_regs(void* opaque, uint32_t address, uint32_t* value)
{
    struct dd_controller* dd = (struct dd_controller*)opaque;

    if (address < MM_DD_REGS || address >= MM_DD_MS_RAM) {
        DebugMessage(M64MSG_ERROR, "Unknown access in DD registers MMIO space %08x", address);
        *value = 0;
        return;
    }

    uint32_t reg = dd_reg(address);

    /* disk presence test */
    if (reg == DD_ASIC_CMD_STATUS) {
        if (dd->idisk != NULL) {
            dd->regs[reg] |= DD_STATUS_DISK_PRES;
        }
        else {
            dd->regs[reg] &= ~DD_STATUS_DISK_PRES;
        }
    }

    *value = dd->regs[reg];
    DebugMessage(M64MSG_VERBOSE, "DD REG: %08X -> %08x", address, *value);

    /* post read update. Not part of the returned value */
    switch(reg)
    {
    case DD_ASIC_CMD_STATUS: {
            /* clear BM interrupt when reading gap */
            if ((dd->regs[DD_ASIC_CMD_STATUS] & DD_STATUS_BM_INT) && (dd->regs[DD_ASIC_CUR_SECTOR] > SECTORS_PER_BLOCK)) {
                clear_dd_interrupt(dd, DD_STATUS_BM_INT);
                dd_update_bm(dd);
            }
        } break;
    }
}

void write_dd_regs(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    uint8_t start_sector;
    struct dd_controller* dd = (struct dd_controller*)opaque;

    if (address < MM_DD_REGS || address >= MM_DD_MS_RAM) {
        DebugMessage(M64MSG_ERROR, "Unknown access in DD registers MMIO space %08x", address);
        return;
    }

    uint32_t reg = dd_reg(address);

    assert(mask == ~UINT32_C(0));

    DebugMessage(M64MSG_VERBOSE, "DD REG: %08X <- %08x", address, value);

    switch (reg)
    {
    case DD_ASIC_DATA:
        dd->regs[DD_ASIC_DATA] = value;
        break;

    case DD_ASIC_CMD_STATUS:
        update_rtc(&dd->rtc);
        const struct tm* tm = localtime(&dd->rtc.now);

        switch ((value >> 16) & 0xff)
        {
        /* No-op */
        case 0x00:
            break;

        /* Seek track */
        case 0x01:
        case 0x02:
            dd->regs[DD_ASIC_CUR_TK] = dd->regs[DD_ASIC_DATA] >> 16;
            /* lock track */
            dd->regs[DD_ASIC_CUR_TK] |= DD_TRACK_LOCK;
            dd->bm_write = (value >> 17) & 0x1;
            break;

        /* Clear Disk change flag */
        case 0x08:
            dd->regs[DD_ASIC_CMD_STATUS] &= ~DD_STATUS_DISK_CHNG;
            break;

        /* Clear reset flag */
        case 0x09:
            dd->regs[DD_ASIC_CMD_STATUS] &= ~DD_STATUS_RST_STATE;
            dd->regs[DD_ASIC_CMD_STATUS] &= ~DD_STATUS_DISK_CHNG;
            break;

        /* Set Disk type */
        case 0x0b:
            DebugMessage(M64MSG_VERBOSE, "Setting disk type %u", (dd->regs[DD_ASIC_DATA] >> 16) & 0xf);
            break;

        /* Read RTC in ASIC_DATA (BCD format) */
        case 0x12:
            dd->regs[DD_ASIC_DATA] = time2data(tm->tm_year, tm->tm_mon + 1);
            break;
        case 0x13:
            dd->regs[DD_ASIC_DATA] = time2data(tm->tm_mday, tm->tm_hour);
            break;
        case 0x14:
            dd->regs[DD_ASIC_DATA] = time2data(tm->tm_min, tm->tm_sec);
            break;

        /* Feature inquiry */
        case 0x1b:
            dd->regs[DD_ASIC_DATA] = 0x00000000;
            break;

        default:
            DebugMessage(M64MSG_WARNING, "DD ASIC CMD not yet implemented (%08x)", value);
        }

        /* Signal a MECHA interrupt */
        signal_dd_interrupt(dd, DD_STATUS_MECHA_INT);
        break;

    case DD_ASIC_BM_STATUS_CTL:
        /* set sector */
        start_sector = (value >> 16) & 0xff;
        if (start_sector == 0x00) {
            dd->bm_block = 0;
            dd->regs[DD_ASIC_CUR_SECTOR] = 0;
        } else if (start_sector == 0x5a) {
            dd->bm_block = 1;
            dd->regs[DD_ASIC_CUR_SECTOR] = 0;
        }
        else {
            DebugMessage(M64MSG_ERROR, "Start sector not aligned");
        }

        /* clear MECHA interrupt */
        if (value & DD_BM_CTL_MECHA_RST) {
            dd->regs[DD_ASIC_CMD_STATUS] &= ~DD_STATUS_MECHA_INT;
        }
        /* start block transfer */
        if (value & DD_BM_CTL_BLK_TRANS) {
            dd->regs[DD_ASIC_BM_STATUS_CTL] |= DD_BM_STATUS_BLOCK;
        }
        /* handle reset */
        if (value & DD_BM_CTL_RESET) {
            dd->bm_reset_held = 1;
        }
        if (!(value & DD_BM_CTL_RESET) && dd->bm_reset_held) {
            dd->bm_reset_held = 0;
            dd->regs[DD_ASIC_CMD_STATUS] &= ~(DD_STATUS_DATA_RQ
                                            | DD_STATUS_C2_XFER
                                            | DD_STATUS_BM_ERR
                                            | DD_STATUS_BM_INT);
            dd->regs[DD_ASIC_BM_STATUS_CTL] = 0;
            dd->regs[DD_ASIC_CUR_SECTOR] = 0;
            dd->bm_block = 0;
        }

        /* clear DD interrupt if both MECHA and BM are cleared */
        if ((dd->regs[DD_ASIC_CMD_STATUS] & (DD_STATUS_BM_INT | DD_STATUS_MECHA_INT)) == 0) {
            clear_dd_interrupt(dd, DD_STATUS_BM_INT);
        }

        /* start transfer */
        if (value & DD_BM_CTL_START) {
            if (dd->bm_write && (value & DD_BM_CTL_MNGRMODE)) {
                DebugMessage(M64MSG_WARNING, "Attempt to write disk with BM mode 1");
            }
            if (!dd->bm_write && !(value & DD_BM_CTL_MNGRMODE)) {
                DebugMessage(M64MSG_WARNING, "Attempt to read disk with BM mode 0");
            }
            dd->regs[DD_ASIC_BM_STATUS_CTL] |= DD_BM_STATUS_RUNNING;
            dd_update_bm(dd);
        }
        break;

    case DD_ASIC_HARD_RESET:
        if (value != 0xaaaa0000) {
            DebugMessage(M64MSG_WARNING, "Unexpected hard reset value %08x", value);
        }
        dd->regs[DD_ASIC_CMD_STATUS] |= DD_STATUS_RST_STATE;
        break;

    case DD_ASIC_HOST_SECBYTE:
        dd->regs[DD_ASIC_HOST_SECBYTE] = (value >> 16) & 0xff;
        if ((dd->regs[DD_ASIC_HOST_SECBYTE] + 1) != zone_sec_size[dd->bm_zone]) {
            DebugMessage(M64MSG_WARNING, "Sector size %u set different than expected %u",
                    dd->regs[DD_ASIC_HOST_SECBYTE] + 1, zone_sec_size[dd->bm_zone]);
        }
        break;

    case DD_ASIC_SEC_BYTE:
        dd->regs[DD_ASIC_SEC_BYTE] = (value >> 24) & 0xff;
        if (dd->regs[DD_ASIC_SEC_BYTE] != SECTORS_PER_BLOCK + 4) {
            DebugMessage(M64MSG_WARNING, "Sectors per block %u set different than expected %u",
                    dd->regs[DD_ASIC_SEC_BYTE] + 1, SECTORS_PER_BLOCK + 4);
        }
        break;

    default:
        dd->regs[reg] = value;
    }
}


void read_dd_rom(void* opaque, uint32_t address, uint32_t* value)
{
    struct dd_controller* dd = (struct dd_controller*)opaque;
	uint32_t addr = dd_rom_address(address);

    *value = dd->rom[addr];

    DebugMessage(M64MSG_VERBOSE, "DD ROM: %08X -> %08x", address, *value);
}

void write_dd_rom(void* opaque, uint32_t address, uint32_t value, uint32_t mask)
{
    DebugMessage(M64MSG_VERBOSE, "DD ROM: %08X <- %08x & %08x", address, value, mask);
}

unsigned int dd_dom_dma_read(void* opaque, const uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    struct dd_controller* dd = (struct dd_controller*)opaque;
    uint8_t* mem;
    size_t i;

    DebugMessage(M64MSG_VERBOSE, "DD DMA read dram=%08x  cart=%08x length=%08x",
            dram_addr, cart_addr, length);

    if (cart_addr == MM_DD_DS_BUFFER) {
        cart_addr = (cart_addr - MM_DD_DS_BUFFER) & 0x3fffff;
        mem = dd->ds_buf;
    }
    else {
        DebugMessage(M64MSG_ERROR, "Unknown DD dma read dram=%08x  cart=%08x length=%08x",
            dram_addr, cart_addr, length);
        return (length * 63) / 50;
    }

    for (i = 0; i < length; ++i) {
        mem[(cart_addr + i) ^ S8] = dram[(dram_addr + i) ^ S8];
    }

    return (length * 63) / 50;
}

unsigned int dd_dom_dma_write(void* opaque, uint8_t* dram, uint32_t dram_addr, uint32_t cart_addr, uint32_t length)
{
    struct dd_controller* dd = (struct dd_controller*)opaque;
    unsigned int cycles;
    const uint8_t* mem;
    size_t i;

    DebugMessage(M64MSG_VERBOSE, "DD DMA write dram=%08x  cart=%08x length=%08x",
            dram_addr, cart_addr, length);

    if (cart_addr < MM_DD_ROM) {
        if (cart_addr == MM_DD_C2S_BUFFER) {
            /* C2 sector buffer */
            cart_addr = (cart_addr - MM_DD_C2S_BUFFER);
            mem = (const uint8_t*)&dd->c2s_buf;
        }
        else if (cart_addr == MM_DD_DS_BUFFER) {
            /* Data sector buffer */
            cart_addr = (cart_addr - MM_DD_DS_BUFFER);
            mem = (const uint8_t*)&dd->ds_buf;
        }
        else {
            DebugMessage(M64MSG_ERROR, "Unknown DD dma write dram=%08x  cart=%08x length=%08x",
                dram_addr, cart_addr, length);

            return (length * 63) / 50;
        }

        cycles = (length * 63) / 50;
    }
    else {
        /* DD ROM */
        cart_addr = (cart_addr - MM_DD_ROM);
        mem = (const uint8_t*)dd->rom;
        cycles = (length * 63) / 50;
    }

    for (i = 0; i < length; ++i) {
        dram[(dram_addr + i) ^ S8] = mem[(cart_addr + i) ^ S8];
    }

    invalidate_r4300_cached_code(dd->r4300, R4300_KSEG0 + dram_addr, length);
    invalidate_r4300_cached_code(dd->r4300, R4300_KSEG1 + dram_addr, length);

    return cycles;
}

void dd_on_pi_cart_addr_write(struct dd_controller* dd, uint32_t address)
{
    /* clear C2 xfer */
    if (address == MM_DD_C2S_BUFFER) {
        dd->regs[DD_ASIC_CMD_STATUS] &= ~(DD_STATUS_C2_XFER | DD_STATUS_BM_ERR);
        clear_dd_interrupt(dd, DD_STATUS_BM_INT);
    }
    /* clear data RQ */
    else if (address == MM_DD_DS_BUFFER) {
        dd->regs[DD_ASIC_CMD_STATUS] &= ~(DD_STATUS_DATA_RQ | DD_STATUS_BM_ERR);
        clear_dd_interrupt(dd, DD_STATUS_BM_INT);
    }
}

/* Disk Helper routines */
void GenerateLBAToPhysTable(struct dd_controller* dd)
{
    //For SDK and D64 formats
    struct extra_storage_disk* extra = (struct extra_storage_disk*)dd->idisk->extra(dd->disk);

    if (extra->format == DISK_FORMAT_MAME)
        return;

    for (uint32_t lba = 0; lba < SIZE_LBA; lba++)
    {
        dd->lba_phys_table[lba] = LBAToPhys(dd, lba);
    }
}

uint32_t LBAToVZone(struct dd_controller* dd, uint32_t lba)
{
    struct extra_storage_disk* extra = (struct extra_storage_disk*)dd->idisk->extra(dd->disk);
    const uint8_t* sys_data = dd->idisk->data(dd->disk);
    return LBAToVZoneA(sys_data[5 + extra->offset_sys], lba);
}

uint32_t LBAToVZoneA(uint8_t type, uint32_t lba)
{
    for (uint32_t vzone = 0; vzone < 16; vzone++) {
        if (lba < VZoneLBATable[type & 0x0F][vzone]) {
            return vzone;
        }
    }
    return -1;
}

uint32_t LBAToByte(struct dd_controller* dd, uint32_t lba, uint32_t nlbas)
{
    struct extra_storage_disk* extra = (struct extra_storage_disk*)dd->idisk->extra(dd->disk);
    const uint8_t* sys_data = dd->idisk->data(dd->disk);
    return LBAToByteA(sys_data[5 + extra->offset_sys], lba, nlbas);
}

uint32_t LBAToByteA(uint8_t type, uint32_t lba, uint32_t nlbas)
{
    uint8_t init_flag = 1;
    uint32_t totalbytes = 0;
    uint32_t blocksize = 0;
    uint32_t vzone, pzone = 0;

    uint8_t disktype = type & 0x0F;

    if (nlbas != 0)
    {
        for (; nlbas != 0; nlbas--)
        {
            if ((init_flag == 1) || (VZoneLBATable[disktype][vzone] == lba))
            {
                vzone = LBAToVZone(type, lba);
                pzone = VZoneToPZone(vzone, disktype);
                if (7 < pzone)
                {
                    pzone -= 7;
                }
                blocksize = zone_sec_size_phys[pzone] * SECTORS_PER_BLOCK;
            }

            totalbytes += blocksize;
            lba++;
            init_flag = 0;
            if ((nlbas != 0) && (lba > MAX_LBA))
            {
                return 0xFFFFFFFF;
            }
        }
    }

    return totalbytes;
}

uint16_t LBAToPhys(struct dd_controller* dd, uint32_t lba)
{
    struct extra_storage_disk* extra = (struct extra_storage_disk*)dd->idisk->extra(dd->disk);
    const uint8_t* sys_data = dd->idisk->data(dd->disk);
    uint8_t disktype = sys_data[extra->offset_sys + 5] & 0x0F;

    const uint16_t OUTERCYL_TBL[8] = { 0x000, 0x09E, 0x13C, 0x1D1, 0x266, 0x2FB, 0x390, 0x425 };

    //Get Block 0/1 on Disk Track
    uint8_t block = 1;
    if (((lba & 3) == 0) || ((lba & 3) == 3))
        block = 0;

    //Get Virtual & Physical Disk Zones
    uint16_t vzone = LBAToVZone(dd, lba);
    uint16_t pzone = VZoneToPZone(vzone, disktype);

    //Get Disk Head
    uint16_t head = (7 < pzone);

    //Get Disk Zone
    uint16_t disk_zone = pzone;
    if (disk_zone != 0)
        disk_zone = pzone - 7;

    //Get Virtual Zone LBA start, if Zone 0, it's LBA 0
    uint16_t vzone_lba = 0;
    if (vzone != 0)
        vzone_lba = VZoneLBATable[disktype][vzone - 1];

    //Calculate Physical Track
    uint16_t track = (lba - vzone_lba) >> 1;

    //Get the start track from current zone
    uint16_t track_zone_start = TrackZoneTable[0][pzone];
    if (head != 0)
    {
        //If Head 1, count from the other way around
        track = -track;
        track_zone_start = OUTERCYL_TBL[disk_zone - 1];
    }
    track += TrackZoneTable[0][pzone];

    //Get the relative offset to defect tracks for the current zone (if Zone 0, then it's 0)
    uint16_t defect_offset = 0;
    if (pzone != 0)
        defect_offset = sys_data[extra->offset_sys + (8 + pzone - 1)];

    //Get amount of defect tracks for the current zone
    uint16_t defect_amount = sys_data[extra->offset_sys + (8 + pzone)] - defect_offset;

    //Skip defect tracks
    while ((defect_amount != 0) && ((sys_data[extra->offset_sys + (0x20 + defect_offset)] + track_zone_start) <= track))
    {
        track++;
        defect_offset++;
        defect_amount--;
    }

    return track | (head * 0x1000) | (block * 0x2000);
}

uint32_t PhysToLBA(struct dd_controller* dd, uint16_t head, uint16_t track, uint16_t block)
{
    uint16_t expectedvalue = track | (head * 0x1000) | (block * 0x2000);

    for (uint16_t lba = 0; lba < SIZE_LBA; lba++)
    {
        if (dd->lba_phys_table[lba] == expectedvalue)
        {
            return lba;
        }
    }
    return 0xFFFF;
}
