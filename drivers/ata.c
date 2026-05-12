/*
 * ATA PIO 驱动实现
 * =================
 * 这个模块通过 IDE 主通道的标准端口与磁盘交互，采用最基础的 PIO 模式。
 * 对本项目来说，它主要服务于 SimpleFS 和分页换出交换区。
 */
#include "ata.h"

#include "io.h"

/* IDE 主通道寄存器端口定义。 */
#define ATA_DATA       0x1F0//读写数据
#define ATA_ERROR      0x1F1//错误寄存器
#define ATA_SECCOUNT   0x1F2//扇区数量寄存器，表示要连续读写多少个扇区
#define ATA_LBA_LOW    0x1F3
#define ATA_LBA_MID    0x1F4
#define ATA_LBA_HIGH   0x1F5//设三个放LBA地址低24位
//LBA的地址就是指磁盘扇区编号。28位每个块512字节。共128G
#define ATA_DRIVE      0x1F6//选择主盘/从盘和LBA的高四位。低四位是地址，高四位固定0xE0主盘
#define ATA_STATUS     0x1F7//状态寄存器
#define ATA_COMMAND    0x1F7//命令寄存器。端口号同上。写时是发命令，读时是看状态
#define ATA_ALT_STATUS 0x3F6//备用状态寄存器，读它作为延时

#define ATA_CMD_READ_SECTORS  0x20//从磁盘拿到LBA的扇区数据
#define ATA_CMD_WRITE_SECTORS 0x30//把数据写到磁盘的LBA扇区
#define ATA_CMD_CACHE_FLUSH   0xE7//将控制器缓存刷新到磁盘
//用来解析从状态寄存器读出的数据
#define ATA_STATUS_ERR  0x01//出错
#define ATA_STATUS_DRQ  0x08//数据缓冲区就绪，可以开始读写
#define ATA_STATUS_DF   0x20//设备故障
#define ATA_STATUS_RDY  0x40//设备准备好接收命令
#define ATA_STATUS_BSY  0x80//设备忙

/* 只有初始化成功后，读写接口才会真正工作。 */
static int ata_ready = 0;

static void ata_io_wait(void) {
    /* 读取备用状态寄存器四次是 ATA 驱动里常见的短延时写法。 */
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
    inb(ATA_ALT_STATUS);
}

static int ata_wait_not_busy(void) {
    /* 在发送新命令前，必须先确认设备不再处于忙状态。 */
    for (uint32_t i = 0; i < 100000U; i++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_STATUS_BSY)) {
            return 1;
        }
    }

    return 0;
}

static int ata_wait_drq(void) {
    /* DRQ=1 表示当前扇区的数据缓冲区已经准备好。 */
    for (uint32_t i = 0; i < 100000U; i++) {
        uint8_t status = inb(ATA_STATUS);

        if (status & (ATA_STATUS_ERR | ATA_STATUS_DF)) {//ATA出错/设备故障
            (void)inb(ATA_ERROR);
            return 0;
        }

        if (!(status & ATA_STATUS_BSY) && (status & ATA_STATUS_DRQ)) {//不忙且可以接受数据
            return 1;
        }
    }

    return 0;
}

static void ata_select_lba28(uint32_t lba, uint8_t sector_count) {
    /* 0xE0 选择主盘并启用 LBA；LBA 高 4 位拼进 DRIVE 寄存器。 */
    outb(ATA_DRIVE, (uint8_t)(0xE0 | ((lba >> 24) & 0x0F)));
    ata_io_wait();
    outb(ATA_SECCOUNT, sector_count);
    outb(ATA_LBA_LOW, (uint8_t)(lba & 0xFF));
    outb(ATA_LBA_MID, (uint8_t)((lba >> 8) & 0xFF));
    outb(ATA_LBA_HIGH, (uint8_t)((lba >> 16) & 0xFF));
}

int ata_init(void) {
    /* 简单探测：设备不忙且 RDY 置位，即视为可用。 */
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

        /* 一个扇区 512 字节，PIO 模式下按 16 位字读出。 */
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

        /* 写入时同样按 16 位字推送到数据端口。 */
        for (uint32_t word = 0; word < ATA_SECTOR_SIZE / 2U; word++) {
            outw(ATA_DATA, *in++);
        }
        ata_io_wait();
    }

    /* CACHE FLUSH 尽量确保控制器把缓存中的写入真正刷回磁盘。 */
    if (!ata_wait_not_busy()) {
        return 0;
    }

    outb(ATA_COMMAND, ATA_CMD_CACHE_FLUSH);
    return ata_wait_not_busy();
}
