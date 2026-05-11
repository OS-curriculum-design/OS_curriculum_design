#ifndef SIMPLEFS_H
#define SIMPLEFS_H

#include "../include/types.h"

/*
 * SimpleFS 接口
 * =============
 * 这是一个教学用极简 inode 文件系统，支持：
 * - boot block / info block / inode table / data blocks 固定布局
 * - 目录文件由 {name, inode} 项组成，并维护 "." / ".."
 * - inode 使用若干直接块 + 一个一级间接索引块
 * - 单级命令接口下的当前目录切换
 * - 普通文件创建、删除、读写、追加
 * - 目录创建/删除
 * - 简单的 open/read/write/seek 文件描述符接口
 */

#define SIMPLEFS_MAX_FILE_SIZE 68608U

/* 初始化并尝试从磁盘挂载文件系统。 */
int simplefs_init(void);
/* 重新格式化磁盘上的 SimpleFS 区域。 */
int simplefs_format(void);
/* 查询文件系统当前是否已挂载。 */
int simplefs_is_mounted(void);

/* 列出当前目录下的文件和子目录。 */
void simplefs_list(void);
/* 打印文件系统容量与占用统计。 */
void simplefs_print_stats(void);
/* 输出当前工作目录路径。 */
void simplefs_print_working_directory(void);
/* 在当前目录创建空文件。 */
int simplefs_create(const char* name);
/* 删除当前目录下的普通文件。 */
int simplefs_delete(const char* name);
/* 在当前目录创建子目录。 */
int simplefs_make_dir(const char* name);
/* 删除当前目录下的空目录。 */
int simplefs_remove_dir(const char* name);
/* 切换当前目录，支持普通名字、.. 和 /。 */
int simplefs_change_dir(const char* name);
/* 读取整个文件到缓冲区。 */
int simplefs_read_file(const char* name, uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_read_out);
/* 覆盖写入整个文件，不存在则自动创建。 */
int simplefs_write_file(const char* name, const uint8_t* data, uint32_t size);
/* 把数据追加到文件末尾。 */
int simplefs_append_file(const char* name, const uint8_t* data, uint32_t size);

/* 打开文件并返回 fd。 */
int simplefs_open(const char* name);
/* 关闭 fd。 */
int simplefs_close(int fd);
/* 从 fd 当前偏移读取数据。 */
int simplefs_read_fd(int fd, uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_read_out);
/* 从 fd 当前偏移写入数据。 */
int simplefs_write_fd(int fd, const uint8_t* data, uint32_t size);
/* 修改 fd 当前偏移。 */
int simplefs_seek(int fd, uint32_t offset);
/* 打印当前打开文件表。 */
void simplefs_print_open_files(void);

#endif
