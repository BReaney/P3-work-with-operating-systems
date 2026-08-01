#include "shared.h"
#include <arpa/inet.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define _POSIX_C_SOURCE 200809L
#define FS_IDENTIFIER_SIZE 8U
#define FS_FAT_ENTRY_SIZE 4U

#define FS_OFFSET_IDENTIFIER 0U
#define FS_OFFSET_BLOCK_SIZE 8U
#define FS_OFFSET_BLOCK_COUNT 10U
#define FS_OFFSET_FAT_START 14U
#define FS_OFFSET_FAT_BLOCKS 18U
#define FS_OFFSET_ROOT_START 22U
#define FS_OFFSET_ROOT_BLOCKS 26U

#define DIR_OFFSET_STATUS 0U
#define DIR_OFFSET_STARTING_BLOCK 1U
#define DIR_OFFSET_NUMBER_OF_BLOCKS 5U
#define DIR_OFFSET_FILE_SIZE 9U
#define DIR_OFFSET_CREATION_TIME 13U
#define DIR_OFFSET_MODIFICATION_TIME 20U
#define DIR_OFFSET_FILE_NAME 27U

static uint16_t read_be16(const unsigned char *buffer)
{
    uint16_t value;

    memcpy(&value, buffer, sizeof(value));
    return ntohs(value);
}

static uint32_t read_be32(const unsigned char *buffer)
{
    uint32_t value;

    memcpy(&value, buffer, sizeof(value));
    return ntohl(value);
}

static int seek_to_offset(FILE *image, uint64_t offset, const char *description)
{
    off_t converted_offset = (off_t)offset;

    if (converted_offset < 0 || (uint64_t)converted_offset != offset) {
        fprintf(stderr,
                "Error: %s offset cannot be represented on this system.\n",
                description);
        return -1;
    }

    if (fseeko(image, converted_offset, SEEK_SET) != 0) {
        fprintf(stderr, "Unable to seek to %s: ", description);
        perror(NULL);
        return -1;
    }

    return 0;
}

static int read_exact(FILE *image,
                      unsigned char *buffer,
                      size_t byte_count,
                      const char *description)
{
    if (fread(buffer, 1U, byte_count, image) == byte_count) {
        return 0;
    }

    if (ferror(image)) {
        fprintf(stderr, "Unable to read %s: ", description);
        perror(NULL);
    } else {
        fprintf(stderr, "Error: unexpected end of file while reading %s.\n",
                description);
    }

    return -1;
}

int fs_read_superblock(FILE *image, struct fs_superblock *sb)
{
    unsigned char buffer[FS_SUPERBLOCK_SIZE];
    static const unsigned char expected_identifier[FS_IDENTIFIER_SIZE] = {
        'C', 'S', 'C', '3', '6', '0', 'F', 'S'
    };

    if (image == NULL || sb == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_read_superblock.\n");
        return -1;
    }

    if (seek_to_offset(image, 0U, "the superblock") != 0) {
        return -1;
    }

    if (read_exact(image, buffer, sizeof(buffer), "the superblock") != 0) {
        return -1;
    }

    if (memcmp(buffer + FS_OFFSET_IDENTIFIER,
               expected_identifier,
               FS_IDENTIFIER_SIZE) != 0) {
        fprintf(stderr, "Error: invalid file system identifier.\n");
        return -1;
    }

    sb->block_size = read_be16(buffer + FS_OFFSET_BLOCK_SIZE);
    sb->block_count = read_be32(buffer + FS_OFFSET_BLOCK_COUNT);
    sb->fat_start = read_be32(buffer + FS_OFFSET_FAT_START);
    sb->fat_blocks = read_be32(buffer + FS_OFFSET_FAT_BLOCKS);
    sb->root_start = read_be32(buffer + FS_OFFSET_ROOT_START);
    sb->root_blocks = read_be32(buffer + FS_OFFSET_ROOT_BLOCKS);

    if (sb->block_size == 0U ||
        sb->block_count == 0U ||
        sb->fat_blocks == 0U ||
        sb->root_blocks == 0U) {
        fprintf(stderr, "Error: invalid zero-valued superblock field.\n");
        return -1;
    }

    if ((sb->block_size % FS_DIRECTORY_ENTRY_SIZE) != 0U) {
        fprintf(stderr,
                "Error: block size is not a multiple of the directory entry size.\n");
        return -1;
    }

    if ((uint64_t)sb->root_start + sb->root_blocks > sb->block_count) {
        fprintf(stderr, "Error: root directory lies outside the file system.\n");
        return -1;
    }

    return 0;
}

static int read_fat_entry(FILE *image,
                          const struct fs_superblock *sb,
                          uint32_t block_number,
                          uint32_t *value)
{
    unsigned char buffer[FS_FAT_ENTRY_SIZE];
    uint64_t fat_capacity;
    uint64_t offset;

    if (block_number >= sb->block_count) {
        fprintf(stderr, "Error: FAT index is outside the file system.\n");
        return -1;
    }

    fat_capacity = ((uint64_t)sb->fat_blocks * sb->block_size) /
                   FS_FAT_ENTRY_SIZE;
    if (block_number >= fat_capacity) {
        fprintf(stderr, "Error: FAT is too small for the requested entry.\n");
        return -1;
    }

    offset = ((uint64_t)sb->fat_start * sb->block_size) +
             ((uint64_t)block_number * FS_FAT_ENTRY_SIZE);

    if (seek_to_offset(image, offset, "a FAT entry") != 0) {
        return -1;
    }

    if (read_exact(image, buffer, sizeof(buffer), "a FAT entry") != 0) {
        return -1;
    }

    *value = read_be32(buffer);
    return 0;
}

int fs_count_fat_entries(FILE *image,
                         const struct fs_superblock *sb,
                         uint32_t *free_blocks,
                         uint32_t *reserved_blocks,
                         uint32_t *allocated_blocks)
{
    uint64_t fat_capacity;
    uint32_t block_number;

    if (image == NULL || sb == NULL || free_blocks == NULL ||
        reserved_blocks == NULL || allocated_blocks == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_count_fat_entries.\n");
        return -1;
    }

    fat_capacity = ((uint64_t)sb->fat_blocks * sb->block_size) /
                   FS_FAT_ENTRY_SIZE;
    if (fat_capacity < sb->block_count) {
        fprintf(stderr,
                "Error: FAT is too small for the declared block count.\n");
        return -1;
    }

    *free_blocks = 0U;
    *reserved_blocks = 0U;
    *allocated_blocks = 0U;

    for (block_number = 0U;
         block_number < sb->block_count;
         ++block_number) {
        uint32_t value;

        if (read_fat_entry(image, sb, block_number, &value) != 0) {
            return -1;
        }

        if (value == FS_FAT_FREE) {
            ++(*free_blocks);
        } else if (value == FS_FAT_RESERVED) {
            ++(*reserved_blocks);
        } else {
            ++(*allocated_blocks);
        }
    }

    return 0;
}

static void parse_timestamp(const unsigned char *buffer,
                            struct fs_timestamp *timestamp)
{
    timestamp->year = read_be16(buffer);
    timestamp->month = buffer[2];
    timestamp->day = buffer[3];
    timestamp->hour = buffer[4];
    timestamp->minute = buffer[5];
    timestamp->second = buffer[6];
}

static void parse_directory_entry(const unsigned char *buffer,
                                  struct fs_directory_entry *entry)
{
    const unsigned char *name_field = buffer + DIR_OFFSET_FILE_NAME;
    const unsigned char *terminator;
    size_t name_length;

    entry->status = buffer[DIR_OFFSET_STATUS];
    entry->starting_block = read_be32(buffer + DIR_OFFSET_STARTING_BLOCK);
    entry->number_of_blocks = read_be32(buffer + DIR_OFFSET_NUMBER_OF_BLOCKS);
    entry->file_size = read_be32(buffer + DIR_OFFSET_FILE_SIZE);

    parse_timestamp(buffer + DIR_OFFSET_CREATION_TIME,
                    &entry->creation_time);
    parse_timestamp(buffer + DIR_OFFSET_MODIFICATION_TIME,
                    &entry->modification_time);

    terminator = memchr(name_field, '\0', FS_FILENAME_FIELD_SIZE);
    if (terminator == NULL) {
        name_length = FS_MAX_FILENAME_LENGTH;
    } else {
        name_length = (size_t)(terminator - name_field);
        if (name_length > FS_MAX_FILENAME_LENGTH) {
            name_length = FS_MAX_FILENAME_LENGTH;
        }
    }

    memcpy(entry->name, name_field, name_length);
    entry->name[name_length] = '\0';
}

int fs_read_directory(FILE *image,
                      const struct fs_superblock *sb,
                      uint32_t starting_block,
                      uint32_t number_of_blocks,
                      enum fs_directory_layout layout,
                      struct fs_directory_entry **entries,
                      size_t *entry_count)
{
    unsigned char *block_buffer = NULL;
    unsigned char *visited = NULL;
    struct fs_directory_entry *result = NULL;
    uint64_t entries_per_block;
    uint64_t total_entries;
    uint32_t current_block;
    uint32_t block_index;
    size_t output_index = 0U;
    int status = -1;

    if (image == NULL || sb == NULL || entries == NULL || entry_count == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_read_directory.\n");
        return -1;
    }

    *entries = NULL;
    *entry_count = 0U;

    if (number_of_blocks == 0U) {
        return 0;
    }

    if (starting_block >= sb->block_count ||
        number_of_blocks > sb->block_count) {
        fprintf(stderr, "Error: directory lies outside the file system.\n");
        return -1;
    }

    if (layout == FS_DIRECTORY_CONTIGUOUS &&
        (uint64_t)starting_block + number_of_blocks > sb->block_count) {
        fprintf(stderr, "Error: contiguous directory exceeds the file system.\n");
        return -1;
    }

    entries_per_block = sb->block_size / FS_DIRECTORY_ENTRY_SIZE;
    total_entries = entries_per_block * number_of_blocks;

    if (total_entries > (uint64_t)(SIZE_MAX / sizeof(*result))) {
        fprintf(stderr, "Error: directory is too large to load into memory.\n");
        return -1;
    }

    result = calloc((size_t)total_entries, sizeof(*result));
    block_buffer = malloc(sb->block_size);
    if (result == NULL || block_buffer == NULL) {
        perror("Unable to allocate memory for directory data");
        goto cleanup;
    }

    if (layout == FS_DIRECTORY_FAT_CHAIN) {
        visited = calloc(sb->block_count, sizeof(*visited));
        if (visited == NULL) {
            perror("Unable to allocate memory for FAT-chain validation");
            goto cleanup;
        }
    }

    current_block = starting_block;

    for (block_index = 0U;
         block_index < number_of_blocks;
         ++block_index) {
        uint64_t block_offset;
        size_t entry_index;

        if (current_block >= sb->block_count) {
            fprintf(stderr, "Error: directory block is outside the file system.\n");
            goto cleanup;
        }

        if (visited != NULL) {
            if (visited[current_block] != 0U) {
                fprintf(stderr, "Error: cycle detected in directory FAT chain.\n");
                goto cleanup;
            }
            visited[current_block] = 1U;
        }

        block_offset = (uint64_t)current_block * sb->block_size;
        if (seek_to_offset(image, block_offset, "a directory block") != 0) {
            goto cleanup;
        }

        if (read_exact(image,
                       block_buffer,
                       sb->block_size,
                       "a directory block") != 0) {
            goto cleanup;
        }

        for (entry_index = 0U;
             entry_index < (size_t)entries_per_block;
             ++entry_index) {
            parse_directory_entry(block_buffer +
                                  (entry_index * FS_DIRECTORY_ENTRY_SIZE),
                                  &result[output_index]);
            ++output_index;
        }

        if (block_index + 1U < number_of_blocks) {
            if (layout == FS_DIRECTORY_CONTIGUOUS) {
                ++current_block;
            } else {
                uint32_t next_block;

                if (read_fat_entry(image, sb, current_block, &next_block) != 0) {
                    goto cleanup;
                }

                if (next_block == FS_FAT_FREE ||
                    next_block == FS_FAT_RESERVED ||
                    next_block == FS_FAT_EOF ||
                    next_block >= sb->block_count) {
                    fprintf(stderr,
                            "Error: directory FAT chain ended before its declared block count.\n");
                    goto cleanup;
                }

                current_block = next_block;
            }
        }
    }

    *entries = result;
    *entry_count = (size_t)total_entries;
    result = NULL;
    status = 0;

cleanup:
    free(visited);
    free(block_buffer);
    free(result);
    return status;
}