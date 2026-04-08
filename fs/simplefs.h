#ifndef SIMPLEFS_H
#define SIMPLEFS_H

#include "../include/types.h"

#define SIMPLEFS_MAX_FILE_SIZE 4096U

int simplefs_init(void);
int simplefs_format(void);
int simplefs_is_mounted(void);

void simplefs_list(void);
void simplefs_print_stats(void);
void simplefs_print_working_directory(void);
int simplefs_create(const char* name);
int simplefs_delete(const char* name);
int simplefs_make_dir(const char* name);
int simplefs_remove_dir(const char* name);
int simplefs_change_dir(const char* name);
int simplefs_read_file(const char* name, uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_read_out);
int simplefs_write_file(const char* name, const uint8_t* data, uint32_t size);
int simplefs_append_file(const char* name, const uint8_t* data, uint32_t size);

int simplefs_open(const char* name);
int simplefs_close(int fd);
int simplefs_read_fd(int fd, uint8_t* buffer, uint32_t buffer_size, uint32_t* bytes_read_out);
int simplefs_write_fd(int fd, const uint8_t* data, uint32_t size);
int simplefs_seek(int fd, uint32_t offset);
void simplefs_print_open_files(void);

#endif
