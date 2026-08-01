#ifndef FS_H
#define FS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define FS_SUPERBLOCK_SIZE 512U
#define FS_DIRECTORY_ENTRY_SIZE 64U
#define FS_FILENAME_FIELD_SIZE 31U
#define FS_MAX_FILENAME_LENGTH 30U

#define FS_STATUS_IN_USE 0x01U
#define FS_STATUS_FILE 0x02U
#define FS_STATUS_DIRECTORY 0x04U

#define FS_FAT_FREE 0x00000000U
#define FS_FAT_RESERVED 0x00000001U
#define FS_FAT_EOF 0xFFFFFFFFU

struct fs_superblock {
    uint16_t block_size;
    uint32_t block_count;
    uint32_t fat_start;
    uint32_t fat_blocks;
    uint32_t root_start;
    uint32_t root_blocks;
};

struct fs_timestamp {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

struct fs_directory_entry {
    uint8_t status;
    uint32_t starting_block;
    uint32_t number_of_blocks;
    uint32_t file_size;
    struct fs_timestamp creation_time;
    struct fs_timestamp modification_time;
    char name[FS_MAX_FILENAME_LENGTH + 1U];
};

enum fs_directory_layout {
    FS_DIRECTORY_CONTIGUOUS,
    FS_DIRECTORY_FAT_CHAIN
};

int fs_read_superblock(FILE *image, struct fs_superblock *sb);

int fs_count_fat_entries(FILE *image,
                         const struct fs_superblock *sb,
                         uint32_t *free_blocks,
                         uint32_t *reserved_blocks,
                         uint32_t *allocated_blocks);

int fs_read_directory(FILE *image,
                      const struct fs_superblock *sb,
                      uint32_t starting_block,
                      uint32_t number_of_blocks,
                      enum fs_directory_layout layout,
                      struct fs_directory_entry **entries,
                      size_t *entry_count);

#endif