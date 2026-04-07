#include "ata.h"

#include "io.h"

#define ATA_DATA       0x1F0
#define ATA_ERROR      0x1F1
#define ATA_SECCOUNT   0x1F2
#define ATA_LBA_LOW    0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HIGH   0x1F5
#define ATA_DRIVE      0x1F6
#define ATA_STATUS     0x1F7
#define ATA_COMMAND    0x1F7
#define ATA_ALT_STATUS 0x3F6

#define ATA_CMD_READ_SECTORS  0x20
#define ATA_CMD_WRITE_SECTORS 0x30
#define ATA_CMD_CACHE_FLUSH   0xE7

#define ATA_STATUS_ERR  0x01
#define ATA_STATUS_DRQ  0x08
#define ATA_STATUS_DF   0x20
#define ATA_STATUS_RDY  0x40
#define ATA_STATUS_BSY  0x80

static int ata_ready = 0;

static void ata_io_wait(void) {
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
}

static int ata_wait_not_busy(void) {
    for (uint32_t i = 0; i < 100000U; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_STATUS_BSY)) {
            return 1;
        }
    }

    return 0;
}

static int ata_wait_drq(void) {
    for (uint32_t i = 0; i < 100000U; i++) {
        uint8_t status = inb(ATA_STATUS);

        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {
            (void)inb(ATA_ERROR);
            return 0;
        }

        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ)) {
            return 1;
        }
    }

    return 0;
}

static void ata_select_lba28(uint32_t lba, uint8_t sector_count) {
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_io_wait();
    outb(ATA_SECCOUNT, sector_count);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}

int ata_init(void) {
    outb(ATA_DRIVE, 0xE0);
    ata_io_wait();

    ata_ready = ata_wait_not_busy() && ((inb(ATA_STATUS) & ATA_STATUS_RDY) != 0);
    return ata_ready;
}

int ata_is_ready(void) {
    return ata_ready;
}

int ata_read_sectors(uint32_t lba, uint8_t sector_count, void* buffer) {
    uint16_t* out = (uint16_t*)buffer;

    if (!ata_ready || sector_count == 0 || buffer == (void*)0) {
        return 0;
    }

    if (!ata_wait_not_busy()) {
        return 0;
    }

    ata_select_lba28(lba, sector_count);
    outb(ATA_COMMAND, ATA_CMD_READ_SECTORS);

    for (uint32_t sector = 0; sector < sector_count; sector++) {
        if (!ata_wait_drq()) {
            return 0;
        }

        for (uint32_t word = 0; word < ATA_SECTOR_SIZE / 2U; word++) {
            *out++ = inw(ATA_DATA);
        }
    }

    return 1;
}

int ata_write_sectors(uint32_t lba, uint8_t sector_count, const void* buffer) {
    const uint16_t* in = (const uint16_t*)buffer;

    if (!ata_ready || sector_count == 0 || buffer == (const void*)0) {
        return 0;
    }

    if (!ata_wait_not_busy()) {
        return 0;
    }

    ata_select_lba28(lba, sector_count);
    outb(ATA_COMMAND, ATA_CMD_WRITE_SECTORS);

    for (uint32_t sector = 0; sector < sector_count; sector++) {
        if (!ata_wait_drq()) {
            return 0;
        }

        for (uint32_t word = 0; word < ATA_SECTOR_SIZE / 2U; word++) {
            outw(ATA_DATA, *in++);
        }
        ata_io_wait();
    }

    if (!ata_wait_not_busy()) {
        return 0;
    }

    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy();
}
