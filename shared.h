#ifndef SHARED_H
#define SHARED_H

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

#define FS_NO_ENTRY_OFFSET UINT64_MAX

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

struct fs_directory_location {
    uint32_t starting_block;
    uint32_t number_of_blocks;
    enum fs_directory_layout layout;
    uint64_t entry_offset;
};

int fs_read_superblock(FILE *image, struct fs_superblock *sb);

int fs_count_fat_entries(FILE *image,
                         const struct fs_superblock *sb,
                         uint32_t *free_blocks,
                         uint32_t *reserved_blocks,
                         uint32_t *allocated_blocks);

int fs_read_fat_entry(FILE *image,
                      const struct fs_superblock *sb,
                      uint32_t block_number,
                      uint32_t *value);

int fs_write_fat_entry(FILE *image,
                       const struct fs_superblock *sb,
                       uint32_t block_number,
                       uint32_t value);

int fs_read_block(FILE *image,
                  const struct fs_superblock *sb,
                  uint32_t block_number,
                  unsigned char *buffer);

int fs_write_block(FILE *image,
                   const struct fs_superblock *sb,
                   uint32_t block_number,
                   const unsigned char *buffer);

void fs_get_root_location(const struct fs_superblock *sb,
                          struct fs_directory_location *location);

int fs_read_directory(FILE *image,
                      const struct fs_superblock *sb,
                      const struct fs_directory_location *location,
                      struct fs_directory_entry **entries,
                      uint64_t **entry_offsets,
                      size_t *entry_count);

int fs_read_directory_entry_at(FILE *image,
                               uint64_t offset,
                               struct fs_directory_entry *entry);

int fs_write_directory_entry_at(FILE *image,
                                uint64_t offset,
                                const struct fs_directory_entry *entry);

int fs_find_entry(FILE *image,
                  const struct fs_superblock *sb,
                  const struct fs_directory_location *directory,
                  const char *name,
                  struct fs_directory_entry *entry,
                  uint64_t *entry_offset);

int fs_resolve_directory(FILE *image,
                         const struct fs_superblock *sb,
                         const char *path,
                         struct fs_directory_location *location);

int fs_get_free_directory_slot(FILE *image,
                               const struct fs_superblock *sb,
                               struct fs_directory_location *directory,
                               uint64_t *entry_offset);

int fs_create_directory(FILE *image,
                        const struct fs_superblock *sb,
                        struct fs_directory_location *parent,
                        const char *name,
                        struct fs_directory_location *created);

int fs_collect_chain(FILE *image,
                     const struct fs_superblock *sb,
                     uint32_t starting_block,
                     uint32_t number_of_blocks,
                     uint32_t **blocks);

int fs_find_free_blocks(FILE *image,
                        const struct fs_superblock *sb,
                        uint32_t number_needed,
                        uint32_t **blocks);

int fs_set_chain(FILE *image,
                 const struct fs_superblock *sb,
                 const uint32_t *blocks,
                 uint32_t block_count);

int fs_free_blocks(FILE *image,
                   const struct fs_superblock *sb,
                   const uint32_t *blocks,
                   uint32_t block_count);

void fs_current_timestamp(struct fs_timestamp *timestamp);

void fs_initialize_entry(struct fs_directory_entry *entry,
                         uint8_t type_status,
                         const char *name,
                         uint32_t starting_block,
                         uint32_t number_of_blocks,
                         uint32_t file_size,
                         const struct fs_timestamp *creation_time,
                         const struct fs_timestamp *modification_time);

int fs_entry_is_in_use(const struct fs_directory_entry *entry);
int fs_entry_is_file(const struct fs_directory_entry *entry);
int fs_entry_is_directory(const struct fs_directory_entry *entry);
int fs_valid_component(const char *name);

#endif