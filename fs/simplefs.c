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

#define SIMPLEFS_BOOT_MAGIC 0x544F4F42U
#define SIMPLEFS_MAGIC 0x34464E49U
#define SIMPLEFS_VERSION 2U

#define SIMPLEFS_LBA_BASE 4096U
#define SIMPLEFS_BOOT_LBA SIMPLEFS_LBA_BASE
#define SIMPLEFS_INFO_LBA (SIMPLEFS_LBA_BASE + 1U)
#define SIMPLEFS_INODE_TABLE_LBA (SIMPLEFS_LBA_BASE + 2U)

#define SIMPLEFS_MAX_INODES 64U
#define SIMPLEFS_DATA_BLOCKS 2048U
#define SIMPLEFS_DIRECT_BLOCKS 6U
#define SIMPLEFS_INDIRECT_ENTRIES (ATA_SECTOR_SIZE / sizeof(uint32_t))
#define SIMPLEFS_MAX_OPEN_FILES 8
#define SIMPLEFS_ROOT_INODE 0U
#define SIMPLEFS_INVALID_BLOCK 0xFFFFFFFFU
#define SIMPLEFS_INVALID_INODE 0xFFFFFFFFU

#define SIMPLEFS_INODE_TYPE_FREE      0U
#define SIMPLEFS_INODE_TYPE_REGULAR   1U
#define SIMPLEFS_INODE_TYPE_DIRECTORY 2U

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t root_inode;
    char name[16];
    uint8_t reserved[ATA_SECTOR_SIZE - 28U];
} __attribute__((packed)) SimpleFsBootBlock;

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

typedef struct {
    uint8_t type;
    uint8_t reserved0;
    uint16_t link_count;
    uint32_t size;
    uint32_t direct_blocks[SIMPLEFS_DIRECT_BLOCKS];
    uint32_t indirect_block;
    uint32_t reserved[7];
} __attribute__((packed)) SimpleFsInode;

typedef struct {
    uint32_t inode;
    char name[28];
} __attribute__((packed)) SimpleFsDirectoryEntry;

typedef struct {
    int used;
    uint32_t inode_index;
    uint32_t offset;
    char name[28];
} OpenFile;

#define SIMPLEFS_INODE_TABLE_SECTORS \
    ((uint32_t)((sizeof(SimpleFsInode) * SIMPLEFS_MAX_INODES + ATA_SECTOR_SIZE - 1U) / ATA_SECTOR_SIZE))
#define SIMPLEFS_DATA_LBA (SIMPLEFS_INODE_TABLE_LBA + SIMPLEFS_INODE_TABLE_SECTORS)
#define SIMPLEFS_MAX_DIRECTORY_ENTRIES (SIMPLEFS_MAX_FILE_SIZE / sizeof(SimpleFsDirectoryEntry))

typedef union {
    uint8_t bytes[SIMPLEFS_MAX_FILE_SIZE];
    SimpleFsDirectoryEntry directory_entries[SIMPLEFS_MAX_DIRECTORY_ENTRIES];
} SimpleFsDirectoryScratch;

static SimpleFsBootBlock boot_block;
static SimpleFsInfoBlock info_block;
static SimpleFsInode inode_table[SIMPLEFS_MAX_INODES];
static OpenFile open_files[SIMPLEFS_MAX_OPEN_FILES];
static SimpleFsDirectoryScratch directory_scratch;
static uint8_t file_scratch[SIMPLEFS_MAX_FILE_SIZE];
static uint32_t current_directory = SIMPLEFS_ROOT_INODE;
static int fs_mounted = 0;

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

static int name_is_special(const char* name) {
    return strcmp(name, ".") == 0 || strcmp(name, "..") == 0;
}

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

static void clear_block_pointers(SimpleFsInode* inode) {
    for (uint32_t i = 0; i < SIMPLEFS_DIRECT_BLOCKS; i++) {
        inode->direct_blocks[i] = SIMPLEFS_INVALID_BLOCK;
    }
    inode->indirect_block = SIMPLEFS_INVALID_BLOCK;
}

static void fill_u32_table(uint32_t* table, uint32_t count, uint32_t value) {
    for (uint32_t i = 0; i < count; i++) {
        table[i] = value;
    }
}

static int bitmap_test(const uint8_t* bitmap, uint32_t index) {
    return (bitmap[index / 8U] & (uint8_t)(1U << (index % 8U))) != 0;
}

static void bitmap_set(uint8_t* bitmap, uint32_t index) {
    bitmap[index / 8U] |= (uint8_t)(1U << (index % 8U));
}

static void bitmap_clear(uint8_t* bitmap, uint32_t index) {
    bitmap[index / 8U] &= (uint8_t)~(1U << (index % 8U));
}

static uint32_t block_lba(uint32_t block) {
    return info_block.data_lba + block;
}

static int write_boot_block(void) {
    return ata_write_sectors(SIMPLEFS_BOOT_LBA, 1, &boot_block);
}

static int write_info_block(void) {
    return ata_write_sectors(SIMPLEFS_INFO_LBA, 1, &info_block);
}

static int write_inode_table(void) {
    return ata_write_sectors(info_block.inode_table_lba,
                             (uint8_t)info_block.inode_table_sectors,
                             inode_table);
}

static void reset_open_files(void) {
    for (int i = 0; i < SIMPLEFS_MAX_OPEN_FILES; i++) {
        open_files[i].used = 0;
        open_files[i].inode_index = SIMPLEFS_INVALID_INODE;
        open_files[i].offset = 0;
        open_files[i].name[0] = '\0';
    }
}

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

static int read_indirect_block(const SimpleFsInode* inode, uint32_t* table_out) {
    fill_u32_table(table_out, SIMPLEFS_INDIRECT_ENTRIES, SIMPLEFS_INVALID_BLOCK);

    if (inode->indirect_block == SIMPLEFS_INVALID_BLOCK) {
        return 1;
    }

    return ata_read_sectors(block_lba(inode->indirect_block), 1, table_out);
}

static int write_indirect_block(const SimpleFsInode* inode, const uint32_t* table) {
    if (inode->indirect_block == SIMPLEFS_INVALID_BLOCK) {
        return 0;
    }

    return ata_write_sectors(block_lba(inode->indirect_block), 1, table);
}

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

static int inode_is_open(uint32_t inode_index) {
    for (int fd = 0; fd < SIMPLEFS_MAX_OPEN_FILES; fd++) {
        if (open_files[fd].used && open_files[fd].inode_index == inode_index) {
            return 1;
        }
    }

    return 0;
}

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

        if (!get_file_block(inode, logical_block, &block, indirect_table, &indirect_loaded)) {
            return 0;
        }
        if (block == SIMPLEFS_INVALID_BLOCK) {
            break;
        }

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
            inode->direct_blocks[logical] = block;
        } else {
            uint32_t indirect_index = logical - SIMPLEFS_DIRECT_BLOCKS;

            if (!needs_indirect) {
                uint32_t indirect_block = SIMPLEFS_INVALID_BLOCK;

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
    free_inode_blocks(inode);
    write_info_block();
    write_inode_table();
    return 0;
}

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

static int write_directory_entries(uint32_t dir_inode, uint32_t count) {
    return write_inode_data(dir_inode,
                            directory_scratch.bytes,
                            count * (uint32_t)sizeof(SimpleFsDirectoryEntry));
}

static int init_directory_file(uint32_t dir_inode, uint32_t parent_inode) {
    memset(directory_scratch.bytes, 0, sizeof(directory_scratch.bytes));
    directory_scratch.directory_entries[0].inode = dir_inode;
    copy_name(directory_scratch.directory_entries[0].name, ".", sizeof(directory_scratch.directory_entries[0].name));
    directory_scratch.directory_entries[1].inode = parent_inode;
    copy_name(directory_scratch.directory_entries[1].name, "..", sizeof(directory_scratch.directory_entries[1].name));
    return write_directory_entries(dir_inode, 2U);
}

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

static int get_parent_inode(uint32_t dir_inode, uint32_t* parent_out) {
    SimpleFsDirectoryEntry entry;

    if (!find_entry_in_directory(dir_inode, "..", &entry, (uint32_t*)0)) {
        return 0;
    }

    *parent_out = entry.inode;
    return 1;
}

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

static int lookup_entry_in_current_directory(const char* name, SimpleFsDirectoryEntry* entry_out) {
    return find_entry_in_directory(current_directory, name, entry_out, (uint32_t*)0);
}

static void print_padded_name(const char* name, uint32_t width) {
    uint32_t length = (uint32_t)strlen(name);

    console_write(name);
    while (length < width) {
        console_put_char(' ');
        length++;
    }
}

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

int simplefs_is_mounted(void) {
    return fs_mounted;
}

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

    inode_table[current_directory].link_count++;
    if (!write_inode_table()) {
        return 0;
    }

    return 1;
}

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
