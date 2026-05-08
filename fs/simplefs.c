/*
 * SimpleFS 实现
 * =============
 * 这是一个非常小的教学文件系统，核心思路是：
 * - 固定布局：超级块 / 位图 / 目录区 / 数据区
 * - 固定大小目录项数组
 * - 普通文件使用有限个直接块
 * - 当前目录由一个索引变量维护，不做完整路径解析
 */
#include "simplefs.h"

#include "../console/console.h"
#include "../drivers/ata.h"
#include "../include/string.h"

/* 磁盘布局常量：SimpleFS 区域从固定 LBA 起始。 */
#define SIMPLEFS_MAGIC 0x32534653U
#define SIMPLEFS_LBA_BASE 4096U
#define SIMPLEFS_BITMAP_LBA (SIMPLEFS_LBA_BASE + 1U)
#define SIMPLEFS_DIR_LBA (SIMPLEFS_LBA_BASE + 2U)
#define SIMPLEFS_DIR_SECTORS 4U
#define SIMPLEFS_DATA_LBA (SIMPLEFS_DIR_LBA + SIMPLEFS_DIR_SECTORS)
#define SIMPLEFS_DATA_BLOCKS 2048U

/* 目录项、直接块、打开文件表都采用固定上限。 */
#define SIMPLEFS_MAX_FILES 32
#define SIMPLEFS_DIRECT_BLOCKS 8
#define SIMPLEFS_MAX_OPEN_FILES 8

#define FILE_TYPE_REGULAR 1
#define FILE_TYPE_DIRECTORY 2
#define SIMPLEFS_ROOT_PARENT 0xFFFFU

typedef struct {
    /* 超级块描述整个文件系统的静态布局参数。 */
    uint32_t magic;
    uint32_t data_lba;
    uint32_t data_blocks;
    uint32_t max_files;
    uint32_t direct_blocks;
    uint8_t reserved[ATA_SECTOR_SIZE - 20];
} __attribute__((packed)) SimpleFsSuperblock;

typedef struct {
    /* 目录项既可表示普通文件，也可表示目录。 */
    uint8_t used;
    uint8_t type;
    uint16_t parent;
    uint32_t size;
    uint32_t blocks[SIMPLEFS_DIRECT_BLOCKS];
    char name[24];
} __attribute__((packed)) SimpleFsDirEntry;

typedef struct {
    /* open_files 是一个极简 fd 表，只记录“打开了哪个目录项、当前偏移是多少”。 */
    int used;
    int entry_index;
    uint32_t offset;
} OpenFile;

static SimpleFsSuperblock superblock;
static SimpleFsDirEntry directory[SIMPLEFS_MAX_FILES];
static uint8_t block_bitmap[ATA_SECTOR_SIZE];
static OpenFile open_files[SIMPLEFS_MAX_OPEN_FILES];
static uint8_t append_buffer[SIMPLEFS_MAX_FILE_SIZE];
/* 根目录没有真正目录项，用一个特殊 parent 值表示。 */
static uint16_t current_directory = SIMPLEFS_ROOT_PARENT;
static int fs_mounted = 0;

static void copy_name(char* dst, const char* src) {
    uint32_t i = 0;

    for (; i < sizeof(directory[0].name) - 1U && src[i]; i++) {
        dst[i] = src[i];
    }

    dst[i] = '\0';
}

static int valid_name(const char* name) {
    uint32_t length;

    /* 当前实现只接受简单名字，不接受 ".", "..", "/" 这类保留项。 */
    if (name == (const char*)0 || name[0] == '\0') {
        return 0;
    }

    length = (uint32_t)strlen(name);
    if (length >= sizeof(directory[0].name)) {
        return 0;
    }

    return strcmp(name, ".") != 0 && strcmp(name, "..") != 0 && strcmp(name, "/") != 0;
}

static int bitmap_test(uint32_t block) {
    return (block_bitmap[block / 8U] & (uint8_t)(1U << (block % 8U))) != 0;
}

static void bitmap_set(uint32_t block) {
    block_bitmap[block / 8U] |= (uint8_t)(1U << (block % 8U));
}

static void bitmap_clear(uint32_t block) {
    block_bitmap[block / 8U] &= (uint8_t)~(1U << (block % 8U));
}

static uint32_t block_lba(uint32_t block) {
    /* 数据块编号从 0 开始，换算到磁盘时需要加上数据区起始 LBA。 */
    return superblock.data_lba + block;
}

static int read_metadata(void) {
    /* 依次读回超级块、位图、目录表，视作“挂载”文件系统。 */
    if (!ata_read_sectors(SIMPLEFS_LBA_BASE, 1, &superblock)) {
        return 0;
    }

    if (superblock.magic != SIMPLEFS_MAGIC) {
        return 0;
    }

    if (!ata_read_sectors(SIMPLEFS_BITMAP_LBA, 1, block_bitmap)) {
        return 0;
    }

    if (!ata_read_sectors(SIMPLEFS_DIR_LBA, (uint8_t)SIMPLEFS_DIR_SECTORS, directory)) {
        return 0;
    }

    current_directory = SIMPLEFS_ROOT_PARENT;
    return 1;
}

static int write_bitmap(void) {
    return ata_write_sectors(SIMPLEFS_BITMAP_LBA, 1, block_bitmap);
}

static int write_directory(void) {
    return ata_write_sectors(SIMPLEFS_DIR_LBA, (uint8_t)SIMPLEFS_DIR_SECTORS, directory);
}

static int find_entry_in_parent(uint16_t parent, const char* name) {
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (directory[i].used && directory[i].parent == parent && strcmp(directory[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

static int find_entry(const char* name) {
    /* 当前实现只在“当前目录”里按名字查找，不支持路径遍历。 */
    return find_entry_in_parent(current_directory, name);
}

static int find_free_entry(void) {
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (!directory[i].used) {
            return i;
        }
    }

    return -1;
}

static int entry_is_open(int entry_index) {
    /* 删除文件前需要先确认没有 fd 仍在引用它。 */
    for (int fd = 0; fd < SIMPLEFS_MAX_OPEN_FILES; fd++) {
        if (open_files[fd].used && open_files[fd].entry_index == entry_index) {
            return 1;
        }
    }

    return 0;
}

static int allocate_block(uint32_t* block_out) {
    uint8_t zero[ATA_SECTOR_SIZE];

    /* 新分配的数据块先写成全 0，避免读到旧垃圾数据。 */
    for (uint32_t i = 0; i < ATA_SECTOR_SIZE; i++) {
        zero[i] = 0;
    }

    for (uint32_t block = 0; block < SIMPLEFS_DATA_BLOCKS; block++) {
        if (!bitmap_test(block)) {
            bitmap_set(block);
            if (!ata_write_sectors(block_lba(block), 1, zero)) {
                bitmap_clear(block);
                return 0;
            }
            *block_out = block;
            return 1;
        }
    }

    return 0;
}

static void free_entry_blocks(SimpleFsDirEntry* entry) {
    /* 释放一个文件占用的所有直接块，并把块号重置为无效值。 */
    for (uint32_t i = 0; i < SIMPLEFS_DIRECT_BLOCKS; i++) {
        if (entry->blocks[i] != 0xFFFFFFFFU) {
            bitmap_clear(entry->blocks[i]);
            entry->blocks[i] = 0xFFFFFFFFU;
        }
    }
    entry->size = 0;
}

static void init_entry(SimpleFsDirEntry* entry, const char* name, uint8_t type) {
    /* 新目录项默认属于当前目录，大小为 0，没有任何数据块。 */
    entry->used = 1;
    entry->type = type;
    entry->parent = current_directory;
    entry->size = 0;
    for (uint32_t i = 0; i < SIMPLEFS_DIRECT_BLOCKS; i++) {
        entry->blocks[i] = 0xFFFFFFFFU;
    }
    copy_name(entry->name, name);
}

static int read_entry_bytes(int index, uint32_t offset, uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_read_out) {
    uint32_t remaining;
    uint32_t copied = 0;
    uint32_t block_index;
    uint32_t sector_offset;
    uint8_t sector[ATA_SECTOR_SIZE];

    if (index < 0 || index >= SIMPLEFS_MAX_FILES ||
        !directory[index].used ||
        directory[index].type != FILE_TYPE_REGULAR ||
        buffer == (uint8_t*)0 ||
        bytes_read_out == (uint32_t*)0) {
        return 0;
    }

    if (offset >= directory[index].size) {
        *bytes_read_out = 0;
        return 1;
    }

    remaining = directory[index].size - offset;
    if (remaining > buffer_size) {
        remaining = buffer_size;
    }

    block_index = offset / ATA_SECTOR_SIZE;
    sector_offset = offset % ATA_SECTOR_SIZE;

    /*
     * 读取流程按“块内偏移 + 跨块推进”进行：
     * - 先读出一个整扇区到临时缓冲
     * - 再把真正需要的那一段拷给调用者
     */
    while (block_index < SIMPLEFS_DIRECT_BLOCKS && copied < remaining) {
        uint32_t to_copy;

        if (directory[index].blocks[block_index] == 0xFFFFFFFFU) {
            break;
        }

        if (!ata_read_sectors(block_lba(directory[index].blocks[block_index]), 1, sector)) {
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
        block_index++;
        sector_offset = 0;
    }

    *bytes_read_out = copied;
    return 1;
}

static int write_entry_data(int index, const uint8_t* data, uint32_t size) {
    uint32_t required_blocks;
    uint32_t written = 0;
    uint8_t sector[ATA_SECTOR_SIZE];

    if (index < 0 || index >= SIMPLEFS_MAX_FILES ||
        !directory[index].used ||
        directory[index].type != FILE_TYPE_REGULAR ||
        data == (const uint8_t*)0 ||
        size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    /* 当前写文件策略是“先清空旧块，再按新内容完整重写”。 */
    free_entry_blocks(&directory[index]);
    required_blocks = (size + ATA_SECTOR_SIZE - 1U) / ATA_SECTOR_SIZE;

    for (uint32_t i = 0; i < required_blocks; i++) {
        uint32_t block;
        uint32_t to_write = size - written;
        if (to_write > ATA_SECTOR_SIZE) {
            to_write = ATA_SECTOR_SIZE;
        }

        if (!allocate_block(&block)) {
            write_bitmap();
            write_directory();
            return 0;
        }

        for (uint32_t j = 0; j < ATA_SECTOR_SIZE; j++) {
            sector[j] = 0;
        }
        for (uint32_t j = 0; j < to_write; j++) {
            sector[j] = data[written + j];
        }

        if (!ata_write_sectors(block_lba(block), 1, sector)) {
            bitmap_clear(block);
            write_bitmap();
            write_directory();
            return 0;
        }

        directory[index].blocks[i] = block;
        written += to_write;
    }

    directory[index].size = size;
    if (!write_bitmap()) {
        return 0;
    }
    return write_directory();
}

static void print_path_to(int index) {
    /* 递归向上打印父目录路径，最后形成类似 /a/b/c 的输出。 */
    if (index < 0 || index >= SIMPLEFS_MAX_FILES || !directory[index].used) {
        console_write("/");
        return;
    }

    if (directory[index].parent != SIMPLEFS_ROOT_PARENT) {
        print_path_to((int)directory[index].parent);
        console_write("/");
        console_write(directory[index].name);
        return;
    }

    console_write("/");
    console_write(directory[index].name);
}

int simplefs_init(void) {
    fs_mounted = 0;
    current_directory = SIMPLEFS_ROOT_PARENT;

    /* 启动时先清空 fd 表，再尝试从磁盘读回已有文件系统。 */
    for (int i = 0; i < SIMPLEFS_MAX_OPEN_FILES; i++) {
        open_files[i].used = 0;
        open_files[i].entry_index = -1;
        open_files[i].offset = 0;
    }

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

    /* 格式化就是重新写一份全新的超级块、位图和目录区。 */
    memset(&superblock, 0, sizeof(superblock));
    memset(block_bitmap, 0, sizeof(block_bitmap));
    memset(directory, 0, sizeof(directory));

    superblock.magic = SIMPLEFS_MAGIC;
    superblock.data_lba = SIMPLEFS_DATA_LBA;
    superblock.data_blocks = SIMPLEFS_DATA_BLOCKS;
    superblock.max_files = SIMPLEFS_MAX_FILES;
    superblock.direct_blocks = SIMPLEFS_DIRECT_BLOCKS;

    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        directory[i].parent = SIMPLEFS_ROOT_PARENT;
        for (uint32_t block = 0; block < SIMPLEFS_DIRECT_BLOCKS; block++) {
            directory[i].blocks[block] = 0xFFFFFFFFU;
        }
    }

    if (!ata_write_sectors(SIMPLEFS_LBA_BASE, 1, &superblock)) {
        return 0;
    }
    if (!write_bitmap()) {
        return 0;
    }
    if (!write_directory()) {
        return 0;
    }

    fs_mounted = 1;
    current_directory = SIMPLEFS_ROOT_PARENT;
    for (int i = 0; i < SIMPLEFS_MAX_OPEN_FILES; i++) {
        open_files[i].used = 0;
        open_files[i].entry_index = -1;
        open_files[i].offset = 0;
    }
    return 1;
}

int simplefs_is_mounted(void) {
    return fs_mounted;
}

void simplefs_list(void) {
    if (!fs_mounted) {
        console_write_line("SimpleFS is not mounted. Run mkfs first.");
        return;
    }

    console_write_line("NAME                         SIZE");
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (directory[i].used && directory[i].parent == current_directory) {
            console_write(directory[i].name);
            if (directory[i].type == FILE_TYPE_DIRECTORY) {
                console_write_line(" <DIR>");
            } else {
                console_write(" ");
                console_write_dec((int)directory[i].size);
                console_write_line(" bytes");
            }
        }
    }
}

void simplefs_print_stats(void) {
    uint32_t used_blocks = 0;
    uint32_t files = 0;
    uint32_t dirs = 0;

    console_write("SimpleFS: ");
    console_write_line(fs_mounted ? "mounted" : "not mounted");
    if (!fs_mounted) {
        return;
    }

    /* 统计位图中已占用的数据块数量。 */
    for (uint32_t block = 0; block < SIMPLEFS_DATA_BLOCKS; block++) {
        if (bitmap_test(block)) {
            used_blocks++;
        }
    }

    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (directory[i].used) {
            if (directory[i].type == FILE_TYPE_DIRECTORY) {
                dirs++;
            } else {
                files++;
            }
        }
    }

    console_write("fs lba base: ");
    console_write_dec((int)SIMPLEFS_LBA_BASE);
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

    if (current_directory == SIMPLEFS_ROOT_PARENT) {
        console_write_line("/");
        return;
    }

    print_path_to((int)current_directory);
    console_put_char('\n');
}

int simplefs_create(const char* name) {
    int index;

    if (!fs_mounted || !valid_name(name) || find_entry(name) >= 0) {
        return 0;
    }

    index = find_free_entry();
    if (index < 0) {
        return 0;
    }

    init_entry(&directory[index], name, FILE_TYPE_REGULAR);
    return write_directory();
}

int simplefs_delete(const char* name) {
    int index;

    if (!fs_mounted) {
        return 0;
    }

    index = find_entry(name);
    if (index < 0 || directory[index].type != FILE_TYPE_REGULAR || entry_is_open(index)) {
        return 0;
    }

    /* 删除普通文件前必须先释放其数据块。 */
    free_entry_blocks(&directory[index]);
    memset(&directory[index], 0, sizeof(SimpleFsDirEntry));
    write_bitmap();
    return write_directory();
}

int simplefs_make_dir(const char* name) {
    int index;

    if (!fs_mounted || !valid_name(name) || find_entry(name) >= 0) {
        return 0;
    }

    index = find_free_entry();
    if (index < 0) {
        return 0;
    }

    init_entry(&directory[index], name, FILE_TYPE_DIRECTORY);
    return write_directory();
}

int simplefs_remove_dir(const char* name) {
    int index;

    if (!fs_mounted) {
        return 0;
    }

    index = find_entry(name);
    if (index < 0 || directory[index].type != FILE_TYPE_DIRECTORY) {
        return 0;
    }

    /* 目录非空时不允许删除。 */
    for (int i = 0; i < SIMPLEFS_MAX_FILES; i++) {
        if (directory[i].used && directory[i].parent == (uint16_t)index) {
            return 0;
        }
    }

    memset(&directory[index], 0, sizeof(SimpleFsDirEntry));
    return write_directory();
}

int simplefs_change_dir(const char* name) {
    int index;

    if (!fs_mounted || name == (const char*)0 || name[0] == '\0') {
        return 0;
    }

    if (strcmp(name, "/") == 0) {
        current_directory = SIMPLEFS_ROOT_PARENT;
        return 1;
    }

    if (strcmp(name, "..") == 0) {
        if (current_directory == SIMPLEFS_ROOT_PARENT) {
            return 1;
        }

        current_directory = directory[current_directory].parent;
        return 1;
    }

    index = find_entry(name);
    if (index < 0 || directory[index].type != FILE_TYPE_DIRECTORY) {
        return 0;
    }

    current_directory = (uint16_t)index;
    return 1;
}

int simplefs_read_file(const char* name, uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_read_out) {
    int index;

    if (!fs_mounted || buffer == (uint8_t*)0 || bytes_read_out == (uint32_t*)0) {
        return 0;
    }

    index = find_entry(name);
    if (index < 0 || directory[index].type != FILE_TYPE_REGULAR) {
        return 0;
    }

    return read_entry_bytes(index, 0, buffer, buffer_size, bytes_read_out);
}

int simplefs_write_file(const char* name, const uint8_t* data, uint32_t size) {
    int index;

    if (!fs_mounted || !valid_name(name) || data == (const uint8_t*)0 || size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    index = find_entry(name);
    /* 不存在时自动创建，这样 shell 里的 write/append 更顺手。 */
    if (index < 0) {
        if (!simplefs_create(name)) {
            return 0;
        }
        index = find_entry(name);
        if (index < 0) {
            return 0;
        }
    } else if (directory[index].type != FILE_TYPE_REGULAR) {
        return 0;
    }

    return write_entry_data(index, data, size);
}

int simplefs_append_file(const char* name, const uint8_t* data, uint32_t size) {
    uint32_t old_size = 0;

    /* append 通过“读旧内容 + 末尾拼接 + 整体重写”实现，逻辑简单但效率一般。 */
    if (data == (const uint8_t*)0 || size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    if (find_entry(name) >= 0) {
        if (!simplefs_read_file(name, append_buffer, sizeof(append_buffer), &old_size)) {
            return 0;
        }
    } else {
        old_size = 0;
    }

    if (old_size + size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    for (uint32_t i = 0; i < size; i++) {
        append_buffer[old_size + i] = data[i];
    }

    return simplefs_write_file(name, append_buffer, old_size + size);
}

int simplefs_open(const char* name) {
    int entry_index;

    if (!fs_mounted) {
        return -1;
    }

    entry_index = find_entry(name);
    if (entry_index < 0 || directory[entry_index].type != FILE_TYPE_REGULAR) {
        return -1;
    }

    /* 找到第一个空闲 fd 槽位，并把偏移初始化为 0。 */
    for (int fd = 0; fd < SIMPLEFS_MAX_OPEN_FILES; fd++) {
        if (!open_files[fd].used) {
            open_files[fd].used = 1;
            open_files[fd].entry_index = entry_index;
            open_files[fd].offset = 0;
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
    open_files[fd].entry_index = -1;
    open_files[fd].offset = 0;
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

    if (!read_entry_bytes(open_files[fd].entry_index, open_files[fd].offset, buffer, buffer_size, &bytes_read)) {
        return 0;
    }

    /* read 成功后推进文件偏移，行为更接近真实文件描述符。 */
    open_files[fd].offset += bytes_read;
    *bytes_read_out = bytes_read;
    return 1;
}

int simplefs_write_fd(int fd, const uint8_t* data, uint32_t size) {
    int entry_index;
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

    entry_index = open_files[fd].entry_index;
    offset = open_files[fd].offset;

    if (offset + size > SIMPLEFS_MAX_FILE_SIZE) {
        return 0;
    }

    if (!read_entry_bytes(entry_index, 0, append_buffer, sizeof(append_buffer), &old_size)) {
        return 0;
    }

    /*
     * 如果 seek 把偏移移动到了文件末尾之后，
     * 中间空洞区域会被自动补 0。
     */
    if (offset > old_size) {
        for (uint32_t i = old_size; i < offset; i++) {
            append_buffer[i] = 0;
        }
    }

    for (uint32_t i = 0; i < size; i++) {
        append_buffer[offset + i] = data[i];
    }

    new_size = old_size;
    if (offset + size > new_size) {
        new_size = offset + size;
    }

    if (!write_entry_data(entry_index, append_buffer, new_size)) {
        return 0;
    }

    /* write 成功后把当前 fd 偏移推进到新写入内容之后。 */
    open_files[fd].offset = offset + size;
    return 1;
}

int simplefs_seek(int fd, uint32_t offset) {
    int entry_index;

    if (!fs_mounted ||
        fd < 0 ||
        fd >= SIMPLEFS_MAX_OPEN_FILES ||
        !open_files[fd].used) {
        return 0;
    }

    entry_index = open_files[fd].entry_index;
    if (offset > directory[entry_index].size) {
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
            console_write_line(directory[open_files[fd].entry_index].name);
        }
    }
}
