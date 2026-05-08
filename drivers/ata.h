#ifndef ATA_H
#define ATA_H

#include "../include/types.h"

/*
 * ATA 驱动接口
 * ============
 * 这里实现的是最简化的 PIO + LBA28 访问方式，供文件系统和分页换出使用。
 */

#define ATA_SECTOR_SIZE 512U

/* 初始化主盘控制器，成功返回 1。 */
int ata_init(void);
/* 查询磁盘是否已成功初始化。 */
int ata_is_ready(void);
/* 从指定 LBA 开始读取若干扇区到内存缓冲区。 */
int ata_read_sectors(uint32_t lba, uint8_t sector_count, void* buffer);
/* 把内存缓冲区内容写到指定 LBA 的若干扇区。 */
int ata_write_sectors(uint32_t lba, uint8_t sector_count, const void* buffer);

#endif
