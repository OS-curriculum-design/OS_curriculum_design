/*
 * SimpleFS 实现
 * =============
 * 这一版采用更接近 Unix/Linux 的磁盘布局：
 * - boot block：保留根信息与版本标记
 * - info block：文件系统元数据 + inode/data 位图
 * - inode table：所有 inode 的固定表
 * - data blocks：普通文件、目录文件、一级间接索引块的数据区
 *
 * 目录本身也是文件，目录内容由若干 {name, inode} 项组成，
 * 并默认维护 "." 与 ".." 两个特殊目录项。
 */
#include "simplefs.h"

#include "../console/console.h"
#include "../drivers/ata.h"
#include "../include/string.h"

/*
 * 磁盘布局常量
 * ============
 * SimpleFS 并不从磁盘 0 号扇区开始占用空间，而是故意把文件系统整体
 * 放在较靠后的位置，避免和启动区、GRUB 之类的内容发生冲突。
 *
 * 布局如下：
 *   LBA 4096                 : boot block
 *   LBA 4097                 : info block
 *   LBA 4098 ~ ...           : inode table
 *   inode table 之后的区域   : data blocks
 */
//一块就是一个扇区，一块512字节
#define SIMPLEFS_BOOT_MAGIC 0x544F4F42U
#define SIMPLEFS_MAGIC 0x34464E49U
#define SIMPLEFS_VERSION 2U

#define SIMPLEFS_LBA_BASE 4096U//文件系统起始地址
#define SIMPLEFS_BOOT_LBA SIMPLEFS_LBA_BASE//boot排布在起始地址第0块
#define SIMPLEFS_INFO_LBA (SIMPLEFS_LBA_BASE + 1U)//元数据存在第1块
#define SIMPLEFS_INODE_TABLE_LBA (SIMPLEFS_LBA_BASE + 2U)//inode区从第2块开始
//一个inode64字节，一块放inode，可以放8个
#define SIMPLEFS_MAX_INODES 64U//inode最多64个
#define SIMPLEFS_DATA_BLOCKS 2048U//数据块2048个
#define SIMPLEFS_DIRECT_BLOCKS 6U//一个inode中6个直接索引
#define SIMPLEFS_INDIRECT_ENTRIES (ATA_SECTOR_SIZE / sizeof(uint32_t))//一个块号4字节，这里是128
#define SIMPLEFS_MAX_OPEN_FILES 8//最多同时打开8个文件
#define SIMPLEFS_ROOT_INODE 0U//根inode号为0
#define SIMPLEFS_INVALID_BLOCK 0xFFFFFFFFU//表示“这里没有块”
#define SIMPLEFS_INVALID_INODE 0xFFFFFFFFU//表示“这里没有inode”

#define SIMPLEFS_INODE_TYPE_FREE      0U//空闲
#define SIMPLEFS_INODE_TYPE_REGULAR   1U//普通文件
#define SIMPLEFS_INODE_TYPE_DIRECTORY 2U//目录

/* boot block 只保存最基础的识别信息，便于启动后快速确认这里是不是 SimpleFS。 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t root_inode;
    char name[16];
    uint8_t reserved[ATA_SECTOR_SIZE - 28U];
} __attribute__((packed)) SimpleFsBootBlock;

/*
 * info block 是文件系统真正的“总控信息”：
 * - inode/data 位图记录哪些对象已经被占用
 * - inode table / data 区的位置和大小也都放在这里
 * 挂载时会优先读取并校验这一块。
 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t root_inode;
    uint32_t inode_table_lba;
    uint32_t inode_table_sectors;
    uint32_t data_lba;
    uint32_t inode_count;
    uint32_t data_block_count;
    uint32_t direct_blocks;
    uint32_t indirect_entries;
    uint8_t inode_bitmap[SIMPLEFS_MAX_INODES / 8U];
    uint8_t data_bitmap[SIMPLEFS_DATA_BLOCKS / 8U];
    uint8_t reserved[ATA_SECTOR_SIZE - 304U];
} __attribute__((packed)) SimpleFsInfoBlock;

/*
 * inode 描述一个文件或目录：
 * - type 区分普通文件 / 目录
 * - size 表示字节数
 * - direct_blocks 提供若干直接块
 * - indirect_block 指向一级间接索引块
 */
typedef struct {
    uint8_t type;
    uint8_t reserved0;
    uint16_t link_count;
    uint32_t size;
    uint32_t direct_blocks[SIMPLEFS_DIRECT_BLOCKS];
    uint32_t indirect_block;
    uint32_t reserved[7];
} __attribute__((packed)) SimpleFsInode;

/* 目录文件中的每一项都是一个固定长度的 {inode, name} 映射。 */
typedef struct {
    uint32_t inode;
    char name[28];
} __attribute__((packed)) SimpleFsDirectoryEntry;

/*
 * 这是教学版的“打开文件表”。
 * 它只保存在内存里，不会写盘；系统重启后所有 fd 都会消失。
 */
typedef struct {
    int used;
    uint32_t inode_index;
    uint32_t offset;
    char name[28];
} OpenFile;

#define SIMPLEFS_INODE_TABLE_SECTORS \
    ((uint32_t)((sizeof(SimpleFsInode) * SIMPLEFS_MAX_INODES + ATA_SECTOR_SIZE - 1U) / ATA_SECTOR_SIZE))//inode区有多少块
#define SIMPLEFS_DATA_LBA (SIMPLEFS_INODE_TABLE_LBA + SIMPLEFS_INODE_TABLE_SECTORS)//数据区起始地址
#define SIMPLEFS_MAX_DIRECTORY_ENTRIES (SIMPLEFS_MAX_FILE_SIZE / sizeof(SimpleFsDirectoryEntry))

typedef union {
    uint8_t bytes[SIMPLEFS_MAX_FILE_SIZE];
    SimpleFsDirectoryEntry directory_entries[SIMPLEFS_MAX_DIRECTORY_ENTRIES];
} SimpleFsDirectoryScratch;

/*
 * 下面这些静态全局变量就是文件系统挂载后的内存镜像：
 * - boot_block / info_block / inode_table 来自磁盘
 * - open_files / current_directory / fs_mounted 属于运行时状态
 * - directory_scratch / file_scratch 是临时缓冲区，避免频繁分配内存
 */
static SimpleFsBootBlock boot_block;
static SimpleFsInfoBlock info_block;
static SimpleFsInode inode_table[SIMPLEFS_MAX_INODES];
static OpenFile open_files[SIMPLEFS_MAX_OPEN_FILES];
static SimpleFsDirectoryScratch directory_scratch;
static uint8_t file_scratch[SIMPLEFS_MAX_FILE_SIZE];
static uint32_t current_directory = SIMPLEFS_ROOT_INODE;
static int fs_mounted = 0;

/* 安全拷贝目录项名字，始终保证结果以 '\0' 结尾。 */
static void copy_name(char* dst, const char* src, uint32_t capacity) {
    uint32_t i = 0;

    if (capacity == 0U) {
        return;
    }

    for (; i + 1U < capacity && src[i]; i++) {
        dst[i] = src[i];
    }

    dst[i] = '\0';
}

/* "." 与 ".." 在目录里有特殊含义，创建普通名字时要排除它们。 */
static int name_is_special(const char* name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

/*
 * 文件名合法性检查：
 * - 不能为空
 * - 不能超过目录项固定长度
 * - 不能伪装成 "." / ".." / "/"
 */
static int valid_name(const char* name) {
    uint32_t length;

    if (name == (const char*)0 || name[0] == '\0') {
        return 0;
    }

    length = (uint32_t)strlen(name);
    if (length >= sizeof(((SimpleFsDirectoryEntry*)0)->name)) {
        return 0;
    }

    return !name_is_special(name) && strcmp(name, "/") != 0;
}

/* 把 inode 的所有块指针初始化为“无效块”，表示当前还没有分配数据块。 */
static void clear_block_pointers(SimpleFsInode* inode) {
    for (uint32_t i = 0; i < SIMPLEFS_DIRECT_BLOCKS; i++) {
        inode->direct_blocks[i] = SIMPLEFS_INVALID_BLOCK;
    }
    inode->indirect_block = SIMPLEFS_INVALID_BLOCK;
}

/* 把一张 uint32_t 表批量填成同一个值，常用于初始化间接块索引表。 */
static void fill_u32_table(uint32_t* table, uint32_t count, uint32_t value) {
    for (uint32_t i = 0; i < count; i++) {
        table[i] = value;
    }
}

/* 位图工具：测试某个 inode / data block 当前是否已被占用。 */
static int bitmap_test(const uint8_t* bitmap, uint32_t index) {
    return (bitmap[index / 8U] & (uint8_t)(1U << (index % 8U))) != 0;
}

/* 位图工具：标记某个 inode / data block 为已占用。 */
static void bitmap_set(uint8_t* bitmap, uint32_t index) {
    bitmap[index / 8U] |= (uint8_t)(1U << (index % 8U));
}

/* 位图工具：释放某个 inode / data block。 */
static void bitmap_clear(uint8_t* bitmap, uint32_t index) {
    bitmap[index / 8U] &= (uint8_t)~(1U << (index % 8U));
}

/* data block 编号是相对于数据区的逻辑编号，这里把它换算成真实 LBA。 */
static uint32_t block_lba(uint32_t block) {
    return info_block.data_lba + block;
}

/* 把内存中的 boot block 同步回磁盘固定位置。 */
static int write_boot_block(void) {
    return ata_write_sectors(SIMPLEFS_BOOT_LBA, 1, &boot_block);
}

/* 把 info block 同步回磁盘；位图变化后通常都要调用它。 */
static int write_info_block(void) {
    return ata_write_sectors(SIMPLEFS_INFO_LBA, 1, &info_block);
}

/* inode table 占若干连续扇区，这里统一整表写回。 */
static int write_inode_table(void) {
    return ata_write_sectors(info_block.inode_table_lba,
                             (uint8_t)info_block.inode_table_sectors,
                             inode_table);
}

/* 系统启动或重新格式化后，把打开文件表重置为空。 */
static void reset_open_files(void) {
    for (int i = 0; i < SIMPLEFS_MAX_OPEN_FILES; i++) {
        open_files[i].used = 0;
        open_files[i].inode_index = SIMPLEFS_INVALID_INODE;
        open_files[i].offset = 0;
        open_files[i].name[0] = '\0';
    }
}

/*
 * 从磁盘读取并校验文件系统元数据。
 * 这一步就是本项目里“挂载”的核心：
 * - 读 boot block / info block / inode table
 * - 校验 magic、版本号、布局参数
 * - 检查根 inode 是否存在且确实是目录
 */
static int read_metadata(void) {
    if (!ata_read_sectors(SIMPLEFS_BOOT_LBA, 1, &boot_block)) {
        return 0;
    }

    if (!ata_read_sectors(SIMPLEFS_INFO_LBA, 1, &info_block)) {
        return 0;
    }

    if (boot_block.magic != SIMPLEFS_BOOT_MAGIC ||
        boot_block.version != SIMPLEFS_VERSION ||
        info_block.magic != SIMPLEFS_MAGIC ||
        info_block.version != SIMPLEFS_VERSION ||
        info_block.root_inode != SIMPLEFS_ROOT_INODE ||
        info_block.inode_table_lba != SIMPLEFS_INODE_TABLE_LBA ||
        info_block.inode_table_sectors != SIMPLEFS_INODE_TABLE_SECTORS ||
        info_block.data_lba != SIMPLEFS_DATA_LBA ||
        info_block.inode_count != SIMPLEFS_MAX_INODES ||
        info_block.data_block_count != SIMPLEFS_DATA_BLOCKS ||
        info_block.direct_blocks != SIMPLEFS_DIRECT_BLOCKS ||
        info_block.indirect_entries != SIMPLEFS_INDIRECT_ENTRIES) {
        return 0;
    }

    if (!ata_read_sectors(info_block.inode_table_lba,
                          (uint8_t)info_block.inode_table_sectors,
                          inode_table)) {
        return 0;
    }

    if (!bitmap_test(info_block.inode_bitmap, SIMPLEFS_ROOT_INODE) ||
        inode_table[SIMPLEFS_ROOT_INODE].type != SIMPLEFS_INODE_TYPE_DIRECTORY) {
        return 0;
    }

    current_directory = SIMPLEFS_ROOT_INODE;
    return 1;
}

/*
 * 找到一个空闲数据块并分配出去。
 * 分配时会顺便把整个扇区清零，这样新文件读到的内容是确定的，
 * 也避免泄露之前留在该块里的旧数据。
 */
static int allocate_data_block(uint32_t* block_out) {
    uint8_t zero[ATA_SECTOR_SIZE];

    for (uint32_t i = 0; i < ATA_SECTOR_SIZE; i++) {
        zero[i] = 0;
    }

    for (uint32_t block = 0; block < SIMPLEFS_DATA_BLOCKS; block++) {
        if (!bitmap_test(info_block.data_bitmap, block)) {
            bitmap_set(info_block.data_bitmap, block);
            if (!ata_write_sectors(block_lba(block), 1, zero)) {
                bitmap_clear(info_block.data_bitmap, block);
                return 0;
            }
            *block_out = block;
            return 1;
        }
    }

    return 0;
}

/*
 * 读一级间接索引块。
 * 如果 inode 还没有间接块，就把输出表视为“全是无效块”，并返回成功，
 * 这样调用者不需要额外区分“没有间接块”和“读取失败”。
 */
static int read_indirect_block(const SimpleFsInode* inode, uint32_t* table_out) {
    fill_u32_table(table_out, SIMPLEFS_INDIRECT_ENTRIES, SIMPLEFS_INVALID_BLOCK);

    if (inode->indirect_block == SIMPLEFS_INVALID_BLOCK) {
        return 1;
    }

    return ata_read_sectors(block_lba(inode->indirect_block), 1, table_out);
}

/* 把内存中的一级间接索引表写回磁盘。 */
static int write_indirect_block(const SimpleFsInode* inode, const uint32_t* table) {
    if (inode->indirect_block == SIMPLEFS_INVALID_BLOCK) {
        return 0;
    }

    return ata_write_sectors(block_lba(inode->indirect_block), 1, table);
}

/*
 * 释放一个 inode 持有的全部数据块。
 * 这会同时处理：
 * - 直接块
 * - 一级间接块中记录的所有数据块
 * - 一级间接块本身
 *
 * 注意：这里只更新位图和 inode 内存状态，不负责把元数据写盘；
 * 调用者需要在合适时机再写回 info block / inode table。
 */
static void free_inode_blocks(SimpleFsInode* inode) {
    uint32_t indirect_table[SIMPLEFS_INDIRECT_ENTRIES];

    for (uint32_t i = 0; i < SIMPLEFS_DIRECT_BLOCKS; i++) {
        if (inode->direct_blocks[i] != SIMPLEFS_INVALID_BLOCK) {
            bitmap_clear(info_block.data_bitmap, inode->direct_blocks[i]);
            inode->direct_blocks[i] = SIMPLEFS_INVALID_BLOCK;
        }
    }

    if (inode->indirect_block != SIMPLEFS_INVALID_BLOCK) {
        if (read_indirect_block(inode, indirect_table)) {
            for (uint32_t i = 0; i < SIMPLEFS_INDIRECT_ENTRIES; i++) {
                if (indirect_table[i] != SIMPLEFS_INVALID_BLOCK) {
                    bitmap_clear(info_block.data_bitmap, indirect_table[i]);
                }
            }
        }

        bitmap_clear(info_block.data_bitmap, inode->indirect_block);
        inode->indirect_block = SIMPLEFS_INVALID_BLOCK;
    }

    inode->size = 0;
}

/* 删除文件前要检查该 inode 是否还被某个 fd 打开着。 */
static int inode_is_open(uint32_t inode_index) {
    for (int fd = 0; fd < SIMPLEFS_MAX_OPEN_FILES; fd++) {
        if (open_files[fd].used && open_files[fd].inode_index == inode_index) {
            return 1;
        }
    }

    return 0;
}

/*
 * 分配一个新的 inode。
 * 这里会立刻把 inode 位图和 inode table 写回磁盘，保证分配结果持久化。
 */
static int allocate_inode(uint8_t type, uint32_t* inode_out) {
    for (uint32_t inode_index = 0; inode_index < SIMPLEFS_MAX_INODES; inode_index++) {
        if (!bitmap_test(info_block.inode_bitmap, inode_index)) {
            bitmap_set(info_block.inode_bitmap, inode_index);
            memset(&inode_table[inode_index], 0, sizeof(SimpleFsInode));
            inode_table[inode_index].type = type;
            inode_table[inode_index].link_count = (type == SIMPLEFS_INODE_TYPE_DIRECTORY) ? 2U : 1U;
            clear_block_pointers(&inode_table[inode_index]);

            if (!write_info_block() || !write_inode_table()) {
                bitmap_clear(info_block.inode_bitmap, inode_index);
                memset(&inode_table[inode_index], 0, sizeof(SimpleFsInode));
                return 0;
            }

            *inode_out = inode_index;
            return 1;
        }
    }

    return 0;
}

/*
 * 释放一个 inode：
 * - 根 inode 不能释放
 * - 先回收其所有数据块
 * - 再清 inode 自身并更新位图
 */
static int release_inode(uint32_t inode_index) {
    if (inode_index >= SIMPLEFS_MAX_INODES ||
        !bitmap_test(info_block.inode_bitmap, inode_index) ||
        inode_index == SIMPLEFS_ROOT_INODE) {
        return 0;
    }

    free_inode_blocks(&inode_table[inode_index]);
    memset(&inode_table[inode_index], 0, sizeof(SimpleFsInode));
    bitmap_clear(info_block.inode_bitmap, inode_index);

    if (!write_info_block()) {
        return 0;
    }
    return write_inode_table();
}

/*
 * 根据“逻辑块号”找到文件真正对应的数据块号。
 * 逻辑块号 0、1、2... 是从文件视角看的第几个块；
 * 真正写盘时还需要把它映射成 direct block 或 indirect block 中的编号。
 */
static int get_file_block(const SimpleFsInode* inode,
                          uint32_t logical_block,
                          uint32_t* block_out,
                          uint32_t* indirect_table,
                          int* indirect_loaded) {
    if (logical_block < SIMPLEFS_DIRECT_BLOCKS) {
        *block_out = inode->direct_blocks[logical_block];
        return 1;
    }

    logical_block -= SIMPLEFS_DIRECT_BLOCKS;
    if (logical_block >= SIMPLEFS_INDIRECT_ENTRIES) {
        *block_out = SIMPLEFS_INVALID_BLOCK;
        return 1;
    }

    if (!*indirect_loaded) {
        if (!read_indirect_block(inode, indirect_table)) {
            return 0;
        }
        *indirect_loaded = 1;
    }

    *block_out = indirect_table[logical_block];
    return 1;
}

/*
 * 从指定 inode 中按字节读取数据。
 * 这个函数是真正的文件内容读取核心，支持：
 * - 从任意 offset 开始
 * - 跨多个扇区读取
 * - 同时处理直接块和一级间接块
 */
static int read_inode_bytes(uint32_t inode_index,
                            uint32_t offset,
                            uint8_t* buffer,
                            uint32_t buffer_size,
                            uint32_t* bytes_read_out) {
    uint32_t remaining;
    uint32_t copied = 0;
    uint32_t logical_block;
    uint32_t sector_offset;
    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t indirect_table[SIMPLEFS_INDIRECT_ENTRIES];
    uint32_t block;
    int indirect_loaded = 0;
    SimpleFsInode* inode;

    if (inode_index >= SIMPLEFS_MAX_INODES ||
        !bitmap_test(info_block.inode_bitmap, inode_index) ||
        buffer == (uint8_t*)0 ||
        bytes_read_out == (uint32_t*)0) {
        return 0;
    }

    inode = &inode_table[inode_index];
    if (offset >= inode->size) {
        *bytes_read_out = 0;
        return 1;
    }

    remaining = inode->size - offset;
    if (remaining > buffer_size) {
        remaining = buffer_size;
    }

    logical_block = offset / ATA_SECTOR_SIZE;
    sector_offset = offset % ATA_SECTOR_SIZE;

    while (copied < remaining) {
        uint32_t to_copy;

        /* 先根据逻辑块号找到当前应该读取哪一个磁盘块。 */
        if (!get_file_block(inode, logical_block, &block, indirect_table, &indirect_loaded)) {
            return 0;
        }
        if (block == SIMPLEFS_INVALID_BLOCK) {
            /* 理论上正常文件不该在中间出现“洞”，这里做保守退出。 */
            break;
        }

        /* 每次先整扇区读入，再从里面截取自己真正需要的字节范围。 */
        if (!ata_read_sectors(block_lba(block), 1, sector)) {
            return 0;
        }

        to_copy = remaining - copied;
        if (to_copy > ATA_SECTOR_SIZE - sector_offset) {
            to_copy = ATA_SECTOR_SIZE - sector_offset;
        }

        for (uint32_t j = 0; j < to_copy; j++) {
            buffer[copied + j] = sector[sector_offset + j];
        }

        copied += to_copy;
        logical_block++;
        sector_offset = 0;
    }

    *bytes_read_out = copied;
    return 1;
}

/*
 * 把整个 inode 的内容“重写”为 data[0..size)。
 * 当前实现采用最简单直观的策略：
 * - 先释放旧块
 * - 再按新内容重新分配并写入
 *
 * 这样实现简单，但代价是覆盖写不是原地更新。
 */
static int write_inode_data(uint32_t inode_index, const uint8_t* data, uint32_t size) {
    uint32_t required_blocks;
    uint32_t written = 0;
    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t indirect_table[SIMPLEFS_INDIRECT_ENTRIES];
    int needs_indirect = 0;
    SimpleFsInode* inode;

    if (inode_index >= SIMPLEFS_MAX_INODES ||
        !bitmap_test(info_block.inode_bitmap, inode_index) ||
        data == (const uint8_t*)0 ||
        size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    inode = &inode_table[inode_index];
    fill_u32_table(indirect_table, SIMPLEFS_INDIRECT_ENTRIES, SIMPLEFS_INVALID_BLOCK);

    /*
     * 这是“覆盖写”语义，不是“增量写”语义。
     * 所以旧内容先全部回收，再重新布置新文件占用的块。
     */
    free_inode_blocks(inode);
    required_blocks = (size + ATA_SECTOR_SIZE - 1U) / ATA_SECTOR_SIZE;

    for (uint32_t logical = 0; logical < required_blocks; logical++) {
        uint32_t block;
        uint32_t to_write = size - written;

        if (to_write > ATA_SECTOR_SIZE) {
            to_write = ATA_SECTOR_SIZE;
        }

        if (!allocate_data_block(&block)) {
            goto fail;
        }

        for (uint32_t j = 0; j < ATA_SECTOR_SIZE; j++) {
            sector[j] = 0;
        }
        for (uint32_t j = 0; j < to_write; j++) {
            sector[j] = data[written + j];
        }

        if (!ata_write_sectors(block_lba(block), 1, sector)) {
            bitmap_clear(info_block.data_bitmap, block);
            goto fail;
        }

        if (logical < SIMPLEFS_DIRECT_BLOCKS) {
            /* 前几个块直接放进 inode 的 direct_blocks。 */
            inode->direct_blocks[logical] = block;
        } else {
            uint32_t indirect_index = logical - SIMPLEFS_DIRECT_BLOCKS;

            if (!needs_indirect) {
                uint32_t indirect_block = SIMPLEFS_INVALID_BLOCK;

                /* 第一次越过直接块上限时，才真正为一级间接块分配空间。 */
                if (!allocate_data_block(&indirect_block)) {
                    goto fail;
                }

                inode->indirect_block = indirect_block;
                if (!write_indirect_block(inode, indirect_table)) {
                    goto fail;
                }
                needs_indirect = 1;
            }

            indirect_table[indirect_index] = block;
        }

        written += to_write;
    }

    if (needs_indirect && !write_indirect_block(inode, indirect_table)) {
        goto fail;
    }

    inode->size = size;
    if (!write_info_block()) {
        return 0;
    }
    return write_inode_table();

fail:
    /*
     * 失败回滚策略：
     * - 尽量把刚分到的块全部释放掉
     * - 再把位图和 inode table 刷回磁盘
     * 这样至少不会把磁盘留在明显不一致的状态。
     */
    free_inode_blocks(inode);
    write_info_block();
    write_inode_table();
    return 0;
}

/* 把目录文件读入临时缓冲区，并解析出当前共有多少个目录项。 */
static int read_directory_entries(uint32_t dir_inode, uint32_t* count_out) {
    uint32_t bytes_read = 0;
    SimpleFsInode* inode;

    if (dir_inode >= SIMPLEFS_MAX_INODES ||
        !bitmap_test(info_block.inode_bitmap, dir_inode) ||
        count_out == (uint32_t*)0) {
        return 0;
    }

    inode = &inode_table[dir_inode];
    if (inode->type != SIMPLEFS_INODE_TYPE_DIRECTORY || inode->size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    if (!read_inode_bytes(dir_inode, 0, directory_scratch.bytes, sizeof(directory_scratch.bytes), &bytes_read)) {
        return 0;
    }

    *count_out = bytes_read / (uint32_t)sizeof(SimpleFsDirectoryEntry);
    return 1;
}

/* 把临时缓冲区中的目录项整体写回某个目录 inode。 */
static int write_directory_entries(uint32_t dir_inode, uint32_t count) {
    return write_inode_data(dir_inode,
                            directory_scratch.bytes,
                            count * (uint32_t)sizeof(SimpleFsDirectoryEntry));
}

/*
 * 初始化一个新目录：
 * - 第 0 项是 "."
 * - 第 1 项是 ".."
 * 这和 Unix 风格目录保持一致。
 */
static int init_directory_file(uint32_t dir_inode, uint32_t parent_inode) {
    memset(directory_scratch.bytes, 0, sizeof(directory_scratch.bytes));
    directory_scratch.directory_entries[0].inode = dir_inode;
    copy_name(directory_scratch.directory_entries[0].name, ".", sizeof(directory_scratch.directory_entries[0].name));
    directory_scratch.directory_entries[1].inode = parent_inode;
    copy_name(directory_scratch.directory_entries[1].name, "..", sizeof(directory_scratch.directory_entries[1].name));
    return write_directory_entries(dir_inode, 2U);
}

/* 在指定目录里按名字查找目录项，可选返回目录项内容和所在槽位。 */
static int find_entry_in_directory(uint32_t dir_inode,
                                   const char* name,
                                   SimpleFsDirectoryEntry* entry_out,
                                   uint32_t* slot_out) {
    uint32_t count = 0;

    if (!read_directory_entries(dir_inode, &count)) {
        return 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (directory_scratch.directory_entries[i].inode != SIMPLEFS_INVALID_INODE &&
            strcmp(directory_scratch.directory_entries[i].name, name) == 0) {
            if (entry_out != (SimpleFsDirectoryEntry*)0) {
                *entry_out = directory_scratch.directory_entries[i];
            }
            if (slot_out != (uint32_t*)0) {
                *slot_out = i;
            }
            return 1;
        }
    }

    return 0;
}

/* 在目录末尾追加一个新的 {name, inode} 项。 */
static int append_entry_to_directory(uint32_t dir_inode,
                                     const char* name,
                                     uint32_t child_inode) {
    uint32_t count = 0;

    if (!read_directory_entries(dir_inode, &count)) {
        return 0;
    }

    if (count >= SIMPLEFS_MAX_DIRECTORY_ENTRIES) {
        return 0;
    }

    directory_scratch.directory_entries[count].inode = child_inode;
    copy_name(directory_scratch.directory_entries[count].name, name, sizeof(directory_scratch.directory_entries[count].name));
    return write_directory_entries(dir_inode, count + 1U);
}

/*
 * 删除目录中的一个名字。
 * 当前实现直接把后面的项整体前移，因此目录项在磁盘上是紧凑排列的。
 */
static int remove_entry_from_directory(uint32_t dir_inode,
                                       const char* name,
                                       SimpleFsDirectoryEntry* removed_out) {
    uint32_t count = 0;
    uint32_t slot = 0;

    if (!find_entry_in_directory(dir_inode, name, removed_out, &slot)) {
        return 0;
    }
    if (!read_directory_entries(dir_inode, &count)) {
        return 0;
    }

    for (uint32_t i = slot; i + 1U < count; i++) {
        directory_scratch.directory_entries[i] = directory_scratch.directory_entries[i + 1U];
    }

    if (count > 0U) {
        directory_scratch.directory_entries[count - 1U].inode = SIMPLEFS_INVALID_INODE;
        directory_scratch.directory_entries[count - 1U].name[0] = '\0';
    }

    return write_directory_entries(dir_inode, count - 1U);
}

/* 目录的父目录 inode 来自 ".." 目录项。 */
static int get_parent_inode(uint32_t dir_inode, uint32_t* parent_out) {
    SimpleFsDirectoryEntry entry;

    if (!find_entry_in_directory(dir_inode, "..", &entry, (uint32_t*)0)) {
        return 0;
    }

    *parent_out = entry.inode;
    return 1;
}

/* 反向查找：已知父目录和子 inode，找出这个子项在父目录里的名字。 */
static int find_name_for_inode(uint32_t dir_inode, uint32_t child_inode, char* name_out) {
    uint32_t count = 0;

    if (!read_directory_entries(dir_inode, &count)) {
        return 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (directory_scratch.directory_entries[i].inode == child_inode &&
            !name_is_special(directory_scratch.directory_entries[i].name)) {
            copy_name(name_out, directory_scratch.directory_entries[i].name, sizeof(directory_scratch.directory_entries[i].name));
            return 1;
        }
    }

    return 0;
}

/* 目录是否为空，等价于“除了 . 和 .. 之外没有别的条目”。 */
static int directory_is_empty(uint32_t dir_inode) {
    uint32_t count = 0;

    if (!read_directory_entries(dir_inode, &count)) {
        return 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        if (!name_is_special(directory_scratch.directory_entries[i].name)) {
            return 0;
        }
    }

    return 1;
}

/* 对外很多操作都只在当前工作目录里查名字，这里做一个小包装。 */
static int lookup_entry_in_current_directory(const char* name, SimpleFsDirectoryEntry* entry_out) {
    return find_entry_in_directory(current_directory, name, entry_out, (uint32_t*)0);
}

/* 为了让 ls 输出更整齐，把文件名补齐到固定宽度。 */
static void print_padded_name(const char* name, uint32_t width) {
    uint32_t length = (uint32_t)strlen(name);

    console_write(name);
    while (length < width) {
        console_put_char(' ');
        length++;
    }
}

/*
 * 递归打印某个 inode 对应的绝对路径。
 * 做法是不断向上找父目录，回溯时再把各级名字依次输出。
 */
static void print_path_to_inode(uint32_t inode_index) {
    char name[28];
    uint32_t parent = SIMPLEFS_ROOT_INODE;

    if (inode_index == SIMPLEFS_ROOT_INODE) {
        console_write("/");
        return;
    }

    if (!get_parent_inode(inode_index, &parent) || parent >= SIMPLEFS_MAX_INODES) {
        console_write("/");
        return;
    }

    print_path_to_inode(parent);
    if (!find_name_for_inode(parent, inode_index, name)) {
        return;
    }

    if (parent != SIMPLEFS_ROOT_INODE) {
        console_write("/");
    }
    console_write(name);
}

/* 启动时调用：尝试从磁盘读取并挂载已经存在的 SimpleFS。 */
int simplefs_init(void) {
    fs_mounted = 0;
    current_directory = SIMPLEFS_ROOT_INODE;
    reset_open_files();

    if (!ata_is_ready()) {
        return 0;
    }

    fs_mounted = read_metadata();
    return fs_mounted;
}

/*
 * 格式化文件系统：
 * - 清空内存中的元数据镜像
 * - 构造 boot/info/inode table
 * - 建立根目录 inode，并写入 "." 与 ".."
 */
int simplefs_format(void) {
    if (!ata_is_ready()) {
        return 0;
    }

    memset(&boot_block, 0, sizeof(boot_block));
    memset(&info_block, 0, sizeof(info_block));
    memset(inode_table, 0, sizeof(inode_table));

    boot_block.magic = SIMPLEFS_BOOT_MAGIC;
    boot_block.version = SIMPLEFS_VERSION;
    boot_block.root_inode = SIMPLEFS_ROOT_INODE;
    copy_name(boot_block.name, "SimpleFS", sizeof(boot_block.name));

    info_block.magic = SIMPLEFS_MAGIC;
    info_block.version = SIMPLEFS_VERSION;
    info_block.root_inode = SIMPLEFS_ROOT_INODE;
    info_block.inode_table_lba = SIMPLEFS_INODE_TABLE_LBA;
    info_block.inode_table_sectors = SIMPLEFS_INODE_TABLE_SECTORS;
    info_block.data_lba = SIMPLEFS_DATA_LBA;
    info_block.inode_count = SIMPLEFS_MAX_INODES;
    info_block.data_block_count = SIMPLEFS_DATA_BLOCKS;
    info_block.direct_blocks = SIMPLEFS_DIRECT_BLOCKS;
    info_block.indirect_entries = SIMPLEFS_INDIRECT_ENTRIES;

    for (uint32_t i = 0; i < SIMPLEFS_MAX_INODES; i++) {
        clear_block_pointers(&inode_table[i]);
    }

    bitmap_set(info_block.inode_bitmap, SIMPLEFS_ROOT_INODE);
    inode_table[SIMPLEFS_ROOT_INODE].type = SIMPLEFS_INODE_TYPE_DIRECTORY;
    inode_table[SIMPLEFS_ROOT_INODE].link_count = 2U;

    if (!write_boot_block()) {
        return 0;
    }
    if (!write_info_block()) {
        return 0;
    }
    if (!write_inode_table()) {
        return 0;
    }
    if (!init_directory_file(SIMPLEFS_ROOT_INODE, SIMPLEFS_ROOT_INODE)) {
        return 0;
    }

    fs_mounted = 1;
    current_directory = SIMPLEFS_ROOT_INODE;
    reset_open_files();
    return 1;
}

/* 仅返回当前挂载状态，不做任何 I/O。 */
int simplefs_is_mounted(void) {
    return fs_mounted;
}

/* 列出当前目录下的普通目录项，跳过 "." 和 ".."。 */
void simplefs_list(void) {
    uint32_t count = 0;

    if (!fs_mounted) {
        console_write_line("SimpleFS is not mounted. Run mkfs first.");
        return;
    }

    if (!read_directory_entries(current_directory, &count)) {
        console_write_line("ls failed.");
        return;
    }

    console_write_line("NAME                         SIZE");
    for (uint32_t i = 0; i < count; i++) {
        uint32_t inode_index = directory_scratch.directory_entries[i].inode;
        const char* name = directory_scratch.directory_entries[i].name;

        if (inode_index == SIMPLEFS_INVALID_INODE || name_is_special(name)) {
            continue;
        }

        print_padded_name(name, 29U);
        if (inode_index >= SIMPLEFS_MAX_INODES ||
            !bitmap_test(info_block.inode_bitmap, inode_index)) {
            console_write_line("<STALE>");
            continue;
        }

        if (inode_table[inode_index].type == SIMPLEFS_INODE_TYPE_DIRECTORY) {
            console_write_line("<DIR>");
        } else {
            console_write_dec((int)inode_table[inode_index].size);
            console_write_line(" bytes");
        }
    }
}

/* 打印文件系统整体占用情况，便于观察 inode/块使用率。 */
void simplefs_print_stats(void) {
    uint32_t used_blocks = 0;
    uint32_t used_inodes = 0;
    uint32_t files = 0;
    uint32_t dirs = 0;

    console_write("SimpleFS: ");
    console_write_line(fs_mounted ? "mounted" : "not mounted");
    if (!fs_mounted) {
        return;
    }

    for (uint32_t block = 0; block < SIMPLEFS_DATA_BLOCKS; block++) {
        if (bitmap_test(info_block.data_bitmap, block)) {
            used_blocks++;
        }
    }

    for (uint32_t inode_index = 0; inode_index < SIMPLEFS_MAX_INODES; inode_index++) {
        if (!bitmap_test(info_block.inode_bitmap, inode_index)) {
            continue;
        }

        used_inodes++;
        if (inode_table[inode_index].type == SIMPLEFS_INODE_TYPE_DIRECTORY) {
            dirs++;
        } else if (inode_table[inode_index].type == SIMPLEFS_INODE_TYPE_REGULAR) {
            files++;
        }
    }

    console_write("layout: boot/info/inodes/data");
    console_put_char('\n');
    console_write("root inode: ");
    console_write_dec((int)info_block.root_inode);
    console_put_char('\n');
    console_write("inodes used/free: ");
    console_write_dec((int)used_inodes);
    console_write("/");
    console_write_dec((int)(SIMPLEFS_MAX_INODES - used_inodes));
    console_put_char('\n');
    console_write("files/dirs: ");
    console_write_dec((int)files);
    console_write("/");
    console_write_dec((int)dirs);
    console_put_char('\n');
    console_write("blocks used/free: ");
    console_write_dec((int)used_blocks);
    console_write("/");
    console_write_dec((int)(SIMPLEFS_DATA_BLOCKS - used_blocks));
    console_put_char('\n');
    console_write("max file size: ");
    console_write_dec((int)SIMPLEFS_MAX_FILE_SIZE);
    console_write_line(" bytes");
}

/* 打印当前工作目录。 */
void simplefs_print_working_directory(void) {
    if (!fs_mounted) {
        console_write_line("SimpleFS is not mounted. Run mkfs first.");
        return;
    }

    if (current_directory == SIMPLEFS_ROOT_INODE) {
        console_write_line("/");
        return;
    }

    print_path_to_inode(current_directory);
    console_put_char('\n');
}

/* 在当前目录下创建一个空普通文件。 */
int simplefs_create(const char* name) {
    uint32_t inode_index = 0;

    if (!fs_mounted || !valid_name(name) || lookup_entry_in_current_directory(name, (SimpleFsDirectoryEntry*)0)) {
        return 0;
    }

    if (!allocate_inode(SIMPLEFS_INODE_TYPE_REGULAR, &inode_index)) {
        return 0;
    }

    if (!append_entry_to_directory(current_directory, name, inode_index)) {
        release_inode(inode_index);
        return 0;
    }

    /*
     * 这里把当前目录的 link_count 也加一。
     * 它更像一个教学用统计字段，而不是严格遵循 Unix 语义的链接计数。
     */
    inode_table[current_directory].link_count++;
    if (!write_inode_table()) {
        return 0;
    }

    return 1;
}

/* 删除当前目录下的普通文件；如果文件仍然打开中则拒绝删除。 */
int simplefs_delete(const char* name) {
    SimpleFsDirectoryEntry entry;

    if (!fs_mounted) {
        return 0;
    }

    if (!find_entry_in_directory(current_directory, name, &entry, (uint32_t*)0) ||
        entry.inode >= SIMPLEFS_MAX_INODES ||
        !bitmap_test(info_block.inode_bitmap, entry.inode) ||
        inode_table[entry.inode].type != SIMPLEFS_INODE_TYPE_REGULAR ||
        inode_is_open(entry.inode)) {
        return 0;
    }

    if (!remove_entry_from_directory(current_directory, name, &entry)) {
        return 0;
    }

    if (!release_inode(entry.inode)) {
        return 0;
    }

    if (inode_table[current_directory].link_count > 0U) {
        inode_table[current_directory].link_count--;
    }
    return write_inode_table();
}

/* 在当前目录下创建子目录，并自动初始化 "." / ".."。 */
int simplefs_make_dir(const char* name) {
    uint32_t inode_index = 0;

    if (!fs_mounted || !valid_name(name) || lookup_entry_in_current_directory(name, (SimpleFsDirectoryEntry*)0)) {
        return 0;
    }

    if (!allocate_inode(SIMPLEFS_INODE_TYPE_DIRECTORY, &inode_index)) {
        return 0;
    }

    if (!init_directory_file(inode_index, current_directory)) {
        release_inode(inode_index);
        return 0;
    }

    if (!append_entry_to_directory(current_directory, name, inode_index)) {
        release_inode(inode_index);
        return 0;
    }

    return 1;
}

/* 删除当前目录下的空目录，根目录本身不能删。 */
int simplefs_remove_dir(const char* name) {
    SimpleFsDirectoryEntry entry;

    if (!fs_mounted) {
        return 0;
    }

    if (!find_entry_in_directory(current_directory, name, &entry, (uint32_t*)0) ||
        entry.inode == SIMPLEFS_ROOT_INODE ||
        entry.inode >= SIMPLEFS_MAX_INODES ||
        !bitmap_test(info_block.inode_bitmap, entry.inode) ||
        inode_table[entry.inode].type != SIMPLEFS_INODE_TYPE_DIRECTORY) {
        return 0;
    }

    if (!directory_is_empty(entry.inode)) {
        return 0;
    }

    if (!remove_entry_from_directory(current_directory, name, &entry)) {
        return 0;
    }

    return release_inode(entry.inode);
}

/* 切换当前工作目录，支持普通名字和根目录 "/"。 */
int simplefs_change_dir(const char* name) {
    SimpleFsDirectoryEntry entry;

    if (!fs_mounted || name == (const char*)0 || name[0] == '\0') {
        return 0;
    }

    if (strcmp(name, "/") == 0) {
        current_directory = SIMPLEFS_ROOT_INODE;
        return 1;
    }

    if (!find_entry_in_directory(current_directory, name, &entry, (uint32_t*)0) ||
        entry.inode >= SIMPLEFS_MAX_INODES ||
        !bitmap_test(info_block.inode_bitmap, entry.inode) ||
        inode_table[entry.inode].type != SIMPLEFS_INODE_TYPE_DIRECTORY) {
        return 0;
    }

    current_directory = entry.inode;
    return 1;
}

/* 按文件名把整个文件内容读出来。 */
int simplefs_read_file(const char* name, uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_read_out) {
    SimpleFsDirectoryEntry entry;

    if (!fs_mounted || buffer == (uint8_t*)0 || bytes_read_out == (uint32_t*)0) {
        return 0;
    }

    if (!find_entry_in_directory(current_directory, name, &entry, (uint32_t*)0) ||
        entry.inode >= SIMPLEFS_MAX_INODES ||
        !bitmap_test(info_block.inode_bitmap, entry.inode) ||
        inode_table[entry.inode].type != SIMPLEFS_INODE_TYPE_REGULAR) {
        return 0;
    }

    return read_inode_bytes(entry.inode, 0, buffer, buffer_size, bytes_read_out);
}

/*
 * 覆盖写文件：
 * - 文件不存在则自动创建
 * - 文件存在则整个内容被替换
 */
int simplefs_write_file(const char* name, const uint8_t* data, uint32_t size) {
    SimpleFsDirectoryEntry entry;

    if (!fs_mounted || !valid_name(name) || data == (const uint8_t*)0 || size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    if (!find_entry_in_directory(current_directory, name, &entry, (uint32_t*)0)) {
        if (!simplefs_create(name)) {
            return 0;
        }
        if (!find_entry_in_directory(current_directory, name, &entry, (uint32_t*)0)) {
            return 0;
        }
    } else if (entry.inode >= SIMPLEFS_MAX_INODES ||
               !bitmap_test(info_block.inode_bitmap, entry.inode) ||
               inode_table[entry.inode].type != SIMPLEFS_INODE_TYPE_REGULAR) {
        return 0;
    }

    return write_inode_data(entry.inode, data, size);
}

/* 追加写的实现比较直接：先整文件读出，再在末尾拼接，再整体重写。 */
int simplefs_append_file(const char* name, const uint8_t* data, uint32_t size) {
    uint32_t old_size = 0;

    if (data == (const uint8_t*)0 || size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    if (lookup_entry_in_current_directory(name, (SimpleFsDirectoryEntry*)0)) {
        if (!simplefs_read_file(name, file_scratch, sizeof(file_scratch), &old_size)) {
            return 0;
        }
    } else {
        old_size = 0;
    }

    if (old_size + size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    for (uint32_t i = 0; i < size; i++) {
        file_scratch[old_size + i] = data[i];
    }

    return simplefs_write_file(name, file_scratch, old_size + size);
}

/* 打开当前目录下的普通文件，并在 open_files 表里分配一个 fd。 */
int simplefs_open(const char* name) {
    SimpleFsDirectoryEntry entry;

    if (!fs_mounted) {
        return -1;
    }

    if (!find_entry_in_directory(current_directory, name, &entry, (uint32_t*)0) ||
        entry.inode >= SIMPLEFS_MAX_INODES ||
        !bitmap_test(info_block.inode_bitmap, entry.inode) ||
        inode_table[entry.inode].type != SIMPLEFS_INODE_TYPE_REGULAR) {
        return -1;
    }

    for (int fd = 0; fd < SIMPLEFS_MAX_OPEN_FILES; fd++) {
        if (!open_files[fd].used) {
            open_files[fd].used = 1;
            open_files[fd].inode_index = entry.inode;
            open_files[fd].offset = 0;
            copy_name(open_files[fd].name, name, sizeof(open_files[fd].name));
            return fd;
        }
    }

    return -1;
}

/* 关闭 fd，本质上只是清掉内存里的打开文件表项。 */
int simplefs_close(int fd) {
    if (fd < 0 || fd >= SIMPLEFS_MAX_OPEN_FILES || !open_files[fd].used) {
        return 0;
    }

    open_files[fd].used = 0;
    open_files[fd].inode_index = SIMPLEFS_INVALID_INODE;
    open_files[fd].offset = 0;
    open_files[fd].name[0] = '\0';
    return 1;
}

/* 从 fd 当前偏移开始读取，并自动推进 offset。 */
int simplefs_read_fd(int fd, uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_read_out) {
    uint32_t bytes_read = 0;

    if (!fs_mounted ||
        fd < 0 ||
        fd >= SIMPLEFS_MAX_OPEN_FILES ||
        !open_files[fd].used ||
        buffer == (uint8_t*)0 ||
        bytes_read_out == (uint32_t*)0) {
        return 0;
    }

    if (!read_inode_bytes(open_files[fd].inode_index, open_files[fd].offset, buffer, buffer_size, &bytes_read)) {
        return 0;
    }

    open_files[fd].offset += bytes_read;
    *bytes_read_out = bytes_read;
    return 1;
}

/*
 * 从 fd 当前偏移写入。
 * 当前实现同样走“整文件读出 -> 改内存缓冲区 -> 整体重写”的简单路线。
 */
int simplefs_write_fd(int fd, const uint8_t* data, uint32_t size) {
    uint32_t old_size = 0;
    uint32_t new_size;
    uint32_t offset;

    if (!fs_mounted ||
        fd < 0 ||
        fd >= SIMPLEFS_MAX_OPEN_FILES ||
        !open_files[fd].used ||
        data == (const uint8_t*)0) {
        return 0;
    }

    offset = open_files[fd].offset;
    if (offset + size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    if (!read_inode_bytes(open_files[fd].inode_index, 0, file_scratch, sizeof(file_scratch), &old_size)) {
        return 0;
    }

    if (offset > old_size) {
        /* 支持把偏移移动到文件尾之后再写，中间空洞补 0。 */
        for (uint32_t i = old_size; i < offset; i++) {
            file_scratch[i] = 0;
        }
    }

    for (uint32_t i = 0; i < size; i++) {
        file_scratch[offset + i] = data[i];
    }

    new_size = old_size;
    if (offset + size > new_size) {
        new_size = offset + size;
    }

    if (!write_inode_data(open_files[fd].inode_index, file_scratch, new_size)) {
        return 0;
    }

    open_files[fd].offset = offset + size;
    return 1;
}

/* 调整 fd 的当前读写位置，但不允许越过文件当前大小。 */
int simplefs_seek(int fd, uint32_t offset) {
    uint32_t inode_index;

    if (!fs_mounted ||
        fd < 0 ||
        fd >= SIMPLEFS_MAX_OPEN_FILES ||
        !open_files[fd].used) {
        return 0;
    }

    inode_index = open_files[fd].inode_index;
    if (inode_index >= SIMPLEFS_MAX_INODES ||
        !bitmap_test(info_block.inode_bitmap, inode_index) ||
        offset > inode_table[inode_index].size) {
        return 0;
    }

    open_files[fd].offset = offset;
    return 1;
}

/* 打印当前已经打开的文件描述符表，便于 shell 中观察状态。 */
void simplefs_print_open_files(void) {
    console_write_line("FD  OFFSET  NAME");
    for (int fd = 0; fd < SIMPLEFS_MAX_OPEN_FILES; fd++) {
        if (open_files[fd].used) {
            console_write_dec(fd);
            console_write("   ");
            console_write_dec((int)open_files[fd].offset);
            console_write("       ");
            console_write_line(open_files[fd].name);
        }
    }
}
