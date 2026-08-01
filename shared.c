#include "shared.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

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

static void write_be16(unsigned char *buffer, uint16_t value)
{
    uint16_t converted = htons(value);

    memcpy(buffer, &converted, sizeof(converted));
}

static void write_be32(unsigned char *buffer, uint32_t value)
{
    uint32_t converted = htonl(value);

    memcpy(buffer, &converted, sizeof(converted));
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
        fprintf(stderr,
                "Error: unexpected end of file while reading %s.\n",
                description);
    }

    return -1;
}

static int write_exact(FILE *image,
                       const unsigned char *buffer,
                       size_t byte_count,
                       const char *description)
{
    if (fwrite(buffer, 1U, byte_count, image) == byte_count) {
        return 0;
    }

    fprintf(stderr, "Unable to write %s: ", description);
    perror(NULL);
    return -1;
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

static void encode_timestamp(unsigned char *buffer,
                             const struct fs_timestamp *timestamp)
{
    write_be16(buffer, timestamp->year);
    buffer[2] = timestamp->month;
    buffer[3] = timestamp->day;
    buffer[4] = timestamp->hour;
    buffer[5] = timestamp->minute;
    buffer[6] = timestamp->second;
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

static void encode_directory_entry(unsigned char *buffer,
                                   const struct fs_directory_entry *entry)
{
    size_t name_length = strlen(entry->name);

    memset(buffer, 0xFF, FS_DIRECTORY_ENTRY_SIZE);
    buffer[DIR_OFFSET_STATUS] = entry->status;
    write_be32(buffer + DIR_OFFSET_STARTING_BLOCK, entry->starting_block);
    write_be32(buffer + DIR_OFFSET_NUMBER_OF_BLOCKS,
               entry->number_of_blocks);
    write_be32(buffer + DIR_OFFSET_FILE_SIZE, entry->file_size);
    encode_timestamp(buffer + DIR_OFFSET_CREATION_TIME,
                     &entry->creation_time);
    encode_timestamp(buffer + DIR_OFFSET_MODIFICATION_TIME,
                     &entry->modification_time);

    if (name_length > FS_MAX_FILENAME_LENGTH) {
        name_length = FS_MAX_FILENAME_LENGTH;
    }
    memcpy(buffer + DIR_OFFSET_FILE_NAME, entry->name, name_length);
    buffer[DIR_OFFSET_FILE_NAME + name_length] = '\0';
}

static int block_is_structural(const struct fs_superblock *sb,
                               uint32_t block_number)
{
    uint64_t fat_end = (uint64_t)sb->fat_start + sb->fat_blocks;
    uint64_t root_end = (uint64_t)sb->root_start + sb->root_blocks;

    if (block_number == 0U) {
        return 1;
    }

    if ((uint64_t)block_number >= sb->fat_start &&
        (uint64_t)block_number < fat_end) {
        return 1;
    }

    if ((uint64_t)block_number >= sb->root_start &&
        (uint64_t)block_number < root_end) {
        return 1;
    }

    return 0;
}

int fs_read_superblock(FILE *image, struct fs_superblock *sb)
{
    unsigned char buffer[FS_SUPERBLOCK_SIZE];
    static const unsigned char expected_identifier[FS_IDENTIFIER_SIZE] = {
        'C', 'S', 'C', '3', '6', '0', 'F', 'S'
    };
    uint64_t fat_end;
    uint64_t root_end;

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

    fat_end = (uint64_t)sb->fat_start + sb->fat_blocks;
    root_end = (uint64_t)sb->root_start + sb->root_blocks;
    if (fat_end > sb->block_count || root_end > sb->block_count) {
        fprintf(stderr,
                "Error: a structural region lies outside the file system.\n");
        return -1;
    }

    return 0;
}

int fs_read_fat_entry(FILE *image,
                      const struct fs_superblock *sb,
                      uint32_t block_number,
                      uint32_t *value)
{
    unsigned char buffer[FS_FAT_ENTRY_SIZE];
    uint64_t fat_capacity;
    uint64_t offset;

    if (image == NULL || sb == NULL || value == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_read_fat_entry.\n");
        return -1;
    }

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

int fs_write_fat_entry(FILE *image,
                       const struct fs_superblock *sb,
                       uint32_t block_number,
                       uint32_t value)
{
    unsigned char buffer[FS_FAT_ENTRY_SIZE];
    uint64_t offset;

    if (image == NULL || sb == NULL || block_number >= sb->block_count) {
        fprintf(stderr, "Error: invalid argument to fs_write_fat_entry.\n");
        return -1;
    }

    offset = ((uint64_t)sb->fat_start * sb->block_size) +
             ((uint64_t)block_number * FS_FAT_ENTRY_SIZE);
    write_be32(buffer, value);

    if (seek_to_offset(image, offset, "a FAT entry") != 0) {
        return -1;
    }

    return write_exact(image, buffer, sizeof(buffer), "a FAT entry");
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

        if (fs_read_fat_entry(image, sb, block_number, &value) != 0) {
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

int fs_read_block(FILE *image,
                  const struct fs_superblock *sb,
                  uint32_t block_number,
                  unsigned char *buffer)
{
    uint64_t offset;

    if (image == NULL || sb == NULL || buffer == NULL ||
        block_number >= sb->block_count) {
        fprintf(stderr, "Error: invalid argument to fs_read_block.\n");
        return -1;
    }

    offset = (uint64_t)block_number * sb->block_size;
    if (seek_to_offset(image, offset, "a file-system block") != 0) {
        return -1;
    }

    return read_exact(image, buffer, sb->block_size, "a file-system block");
}

int fs_write_block(FILE *image,
                   const struct fs_superblock *sb,
                   uint32_t block_number,
                   const unsigned char *buffer)
{
    uint64_t offset;

    if (image == NULL || sb == NULL || buffer == NULL ||
        block_number >= sb->block_count) {
        fprintf(stderr, "Error: invalid argument to fs_write_block.\n");
        return -1;
    }

    offset = (uint64_t)block_number * sb->block_size;
    if (seek_to_offset(image, offset, "a file-system block") != 0) {
        return -1;
    }

    return write_exact(image,
                       buffer,
                       sb->block_size,
                       "a file-system block");
}

void fs_get_root_location(const struct fs_superblock *sb,
                          struct fs_directory_location *location)
{
    location->starting_block = sb->root_start;
    location->number_of_blocks = sb->root_blocks;
    location->layout = FS_DIRECTORY_CONTIGUOUS;
    location->entry_offset = FS_NO_ENTRY_OFFSET;
}

int fs_collect_chain(FILE *image,
                     const struct fs_superblock *sb,
                     uint32_t starting_block,
                     uint32_t number_of_blocks,
                     uint32_t **blocks)
{
    uint32_t *result = NULL;
    unsigned char *visited = NULL;
    uint32_t current_block;
    uint32_t index;
    int status = -1;

    if (image == NULL || sb == NULL || blocks == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_collect_chain.\n");
        return -1;
    }

    *blocks = NULL;
    if (number_of_blocks == 0U) {
        return 0;
    }

    if (starting_block >= sb->block_count ||
        number_of_blocks > sb->block_count) {
        fprintf(stderr, "Error: FAT chain lies outside the file system.\n");
        return -1;
    }

    result = malloc((size_t)number_of_blocks * sizeof(*result));
    visited = calloc(sb->block_count, sizeof(*visited));
    if (result == NULL || visited == NULL) {
        perror("Unable to allocate memory for a FAT chain");
        goto cleanup;
    }

    current_block = starting_block;
    for (index = 0U; index < number_of_blocks; ++index) {
        uint32_t next_block;

        if (current_block >= sb->block_count ||
            visited[current_block] != 0U) {
            fprintf(stderr, "Error: invalid or cyclic FAT chain.\n");
            goto cleanup;
        }

        visited[current_block] = 1U;
        result[index] = current_block;

        if (fs_read_fat_entry(image, sb, current_block, &next_block) != 0) {
            goto cleanup;
        }

        if (index + 1U < number_of_blocks) {
            if (next_block == FS_FAT_FREE ||
                next_block == FS_FAT_RESERVED ||
                next_block == FS_FAT_EOF ||
                next_block >= sb->block_count) {
                fprintf(stderr,
                        "Error: FAT chain ended before its declared block count.\n");
                goto cleanup;
            }
            current_block = next_block;
        } else if (next_block != FS_FAT_EOF) {
            fprintf(stderr,
                    "Error: FAT chain is longer than its declared block count.\n");
            goto cleanup;
        }
    }

    *blocks = result;
    result = NULL;
    status = 0;

cleanup:
    free(visited);
    free(result);
    return status;
}

int fs_read_directory(FILE *image,
                      const struct fs_superblock *sb,
                      const struct fs_directory_location *location,
                      struct fs_directory_entry **entries,
                      uint64_t **entry_offsets,
                      size_t *entry_count)
{
    struct fs_directory_entry *result = NULL;
    uint64_t *offset_result = NULL;
    uint32_t *chain_blocks = NULL;
    unsigned char *block_buffer = NULL;
    uint64_t entries_per_block;
    uint64_t total_entries;
    uint32_t block_index;
    size_t output_index = 0U;
    int status = -1;

    if (image == NULL || sb == NULL || location == NULL ||
        entries == NULL || entry_count == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_read_directory.\n");
        return -1;
    }

    *entries = NULL;
    if (entry_offsets != NULL) {
        *entry_offsets = NULL;
    }
    *entry_count = 0U;

    if (location->number_of_blocks == 0U) {
        return 0;
    }

    entries_per_block = sb->block_size / FS_DIRECTORY_ENTRY_SIZE;
    total_entries = entries_per_block * location->number_of_blocks;
    if (total_entries > (uint64_t)(SIZE_MAX / sizeof(*result))) {
        fprintf(stderr, "Error: directory is too large to load into memory.\n");
        return -1;
    }

    result = calloc((size_t)total_entries, sizeof(*result));
    block_buffer = malloc(sb->block_size);
    if (entry_offsets != NULL) {
        offset_result = malloc((size_t)total_entries *
                               sizeof(*offset_result));
    }
    if (result == NULL || block_buffer == NULL ||
        (entry_offsets != NULL && offset_result == NULL)) {
        perror("Unable to allocate memory for directory data");
        goto cleanup;
    }

    if (location->layout == FS_DIRECTORY_FAT_CHAIN) {
        if (fs_collect_chain(image,
                             sb,
                             location->starting_block,
                             location->number_of_blocks,
                             &chain_blocks) != 0) {
            goto cleanup;
        }
    }

    for (block_index = 0U;
         block_index < location->number_of_blocks;
         ++block_index) {
        uint32_t block_number;
        size_t entry_index;

        if (location->layout == FS_DIRECTORY_CONTIGUOUS) {
            uint64_t candidate = (uint64_t)location->starting_block +
                                 block_index;
            if (candidate >= sb->block_count) {
                fprintf(stderr,
                        "Error: contiguous directory exceeds the file system.\n");
                goto cleanup;
            }
            block_number = (uint32_t)candidate;
        } else {
            block_number = chain_blocks[block_index];
        }

        if (fs_read_block(image, sb, block_number, block_buffer) != 0) {
            goto cleanup;
        }

        for (entry_index = 0U;
             entry_index < (size_t)entries_per_block;
             ++entry_index) {
            uint64_t entry_offset =
                ((uint64_t)block_number * sb->block_size) +
                (entry_index * FS_DIRECTORY_ENTRY_SIZE);

            parse_directory_entry(block_buffer +
                                  (entry_index * FS_DIRECTORY_ENTRY_SIZE),
                                  &result[output_index]);
            if (offset_result != NULL) {
                offset_result[output_index] = entry_offset;
            }
            ++output_index;
        }
    }

    *entries = result;
    if (entry_offsets != NULL) {
        *entry_offsets = offset_result;
    }
    *entry_count = (size_t)total_entries;
    result = NULL;
    offset_result = NULL;
    status = 0;

cleanup:
    free(chain_blocks);
    free(block_buffer);
    free(offset_result);
    free(result);
    return status;
}

int fs_read_directory_entry_at(FILE *image,
                               uint64_t offset,
                               struct fs_directory_entry *entry)
{
    unsigned char buffer[FS_DIRECTORY_ENTRY_SIZE];

    if (image == NULL || entry == NULL) {
        fprintf(stderr,
                "Error: invalid argument to fs_read_directory_entry_at.\n");
        return -1;
    }

    if (seek_to_offset(image, offset, "a directory entry") != 0) {
        return -1;
    }

    if (read_exact(image,
                   buffer,
                   sizeof(buffer),
                   "a directory entry") != 0) {
        return -1;
    }

    parse_directory_entry(buffer, entry);
    return 0;
}

int fs_write_directory_entry_at(FILE *image,
                                uint64_t offset,
                                const struct fs_directory_entry *entry)
{
    unsigned char buffer[FS_DIRECTORY_ENTRY_SIZE];

    if (image == NULL || entry == NULL) {
        fprintf(stderr,
                "Error: invalid argument to fs_write_directory_entry_at.\n");
        return -1;
    }

    encode_directory_entry(buffer, entry);
    if (seek_to_offset(image, offset, "a directory entry") != 0) {
        return -1;
    }

    return write_exact(image,
                       buffer,
                       sizeof(buffer),
                       "a directory entry");
}

int fs_entry_is_in_use(const struct fs_directory_entry *entry)
{
    return entry != NULL &&
           (entry->status & FS_STATUS_IN_USE) != 0U;
}

int fs_entry_is_file(const struct fs_directory_entry *entry)
{
    return fs_entry_is_in_use(entry) &&
           (entry->status & FS_STATUS_FILE) != 0U &&
           (entry->status & FS_STATUS_DIRECTORY) == 0U;
}

int fs_entry_is_directory(const struct fs_directory_entry *entry)
{
    return fs_entry_is_in_use(entry) &&
           (entry->status & FS_STATUS_DIRECTORY) != 0U &&
           (entry->status & FS_STATUS_FILE) == 0U;
}

int fs_valid_component(const char *name)
{
    size_t length;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }

    length = strlen(name);
    if (length > FS_MAX_FILENAME_LENGTH || strchr(name, '/') != NULL) {
        return 0;
    }

    return 1;
}

int fs_find_entry(FILE *image,
                  const struct fs_superblock *sb,
                  const struct fs_directory_location *directory,
                  const char *name,
                  struct fs_directory_entry *entry,
                  uint64_t *entry_offset)
{
    struct fs_directory_entry *entries = NULL;
    uint64_t *offsets = NULL;
    size_t entry_count = 0U;
    size_t index;
    int found = 0;

    if (name == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_find_entry.\n");
        return -1;
    }

    if (fs_read_directory(image,
                          sb,
                          directory,
                          &entries,
                          &offsets,
                          &entry_count) != 0) {
        return -1;
    }

    for (index = 0U; index < entry_count; ++index) {
        if (fs_entry_is_in_use(&entries[index]) &&
            strcmp(entries[index].name, name) == 0) {
            if (entry != NULL) {
                *entry = entries[index];
            }
            if (entry_offset != NULL) {
                *entry_offset = offsets[index];
            }
            found = 1;
            break;
        }
    }

    free(offsets);
    free(entries);
    return found;
}

int fs_resolve_directory(FILE *image,
                         const struct fs_superblock *sb,
                         const char *path,
                         struct fs_directory_location *location)
{
    const char *cursor;

    if (image == NULL || sb == NULL || path == NULL || location == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_resolve_directory.\n");
        return -1;
    }

    fs_get_root_location(sb, location);
    cursor = path;

    while (*cursor != '\0') {
        const char *start;
        size_t length;
        char component[FS_MAX_FILENAME_LENGTH + 1U];
        struct fs_directory_entry entry;
        uint64_t entry_offset;
        int find_result;

        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        length = (size_t)(cursor - start);

        if (length == 1U && start[0] == '.') {
            continue;
        }
        if (length == 2U && start[0] == '.' && start[1] == '.') {
            return 0;
        }
        if (length == 0U || length > FS_MAX_FILENAME_LENGTH) {
            return 0;
        }

        memcpy(component, start, length);
        component[length] = '\0';

        find_result = fs_find_entry(image,
                                    sb,
                                    location,
                                    component,
                                    &entry,
                                    &entry_offset);
        if (find_result <= 0) {
            return find_result;
        }
        if (!fs_entry_is_directory(&entry)) {
            return 0;
        }

        location->starting_block = entry.starting_block;
        location->number_of_blocks = entry.number_of_blocks;
        location->layout = FS_DIRECTORY_FAT_CHAIN;
        location->entry_offset = entry_offset;
    }

    return 1;
}

int fs_find_free_blocks(FILE *image,
                        const struct fs_superblock *sb,
                        uint32_t number_needed,
                        uint32_t **blocks)
{
    uint32_t *result = NULL;
    uint32_t found = 0U;
    uint32_t block_number;

    if (image == NULL || sb == NULL || blocks == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_find_free_blocks.\n");
        return -1;
    }

    *blocks = NULL;
    if (number_needed == 0U) {
        return 0;
    }

    if ((uint64_t)number_needed * sizeof(*result) > SIZE_MAX) {
        fprintf(stderr, "Error: requested block list is too large.\n");
        return -1;
    }

    result = malloc((size_t)number_needed * sizeof(*result));
    if (result == NULL) {
        perror("Unable to allocate memory for free blocks");
        return -1;
    }

    for (block_number = 0U;
         block_number < sb->block_count && found < number_needed;
         ++block_number) {
        uint32_t value;

        if (block_is_structural(sb, block_number)) {
            continue;
        }

        if (fs_read_fat_entry(image, sb, block_number, &value) != 0) {
            free(result);
            return -1;
        }

        if (value == FS_FAT_FREE) {
            result[found] = block_number;
            ++found;
        }
    }

    if (found != number_needed) {
        free(result);
        return 0;
    }

    *blocks = result;
    return 1;
}

int fs_set_chain(FILE *image,
                 const struct fs_superblock *sb,
                 const uint32_t *blocks,
                 uint32_t block_count)
{
    uint32_t index;

    if (block_count > 0U && blocks == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_set_chain.\n");
        return -1;
    }

    for (index = 0U; index < block_count; ++index) {
        uint32_t value = index + 1U < block_count
                             ? blocks[index + 1U]
                             : FS_FAT_EOF;

        if (fs_write_fat_entry(image, sb, blocks[index], value) != 0) {
            return -1;
        }
    }

    return 0;
}

int fs_free_blocks(FILE *image,
                   const struct fs_superblock *sb,
                   const uint32_t *blocks,
                   uint32_t block_count)
{
    uint32_t index;

    if (block_count > 0U && blocks == NULL) {
        fprintf(stderr, "Error: invalid argument to fs_free_blocks.\n");
        return -1;
    }

    for (index = 0U; index < block_count; ++index) {
        if (fs_write_fat_entry(image,
                               sb,
                               blocks[index],
                               FS_FAT_FREE) != 0) {
            return -1;
        }
    }

    return 0;
}

void fs_current_timestamp(struct fs_timestamp *timestamp)
{
    time_t current_time;
    struct tm broken_down;

    memset(timestamp, 0, sizeof(*timestamp));
    current_time = time(NULL);
    if (current_time == (time_t)-1 ||
        localtime_r(&current_time, &broken_down) == NULL) {
        return;
    }

    timestamp->year = (uint16_t)(broken_down.tm_year + 1900);
    timestamp->month = (uint8_t)(broken_down.tm_mon + 1);
    timestamp->day = (uint8_t)broken_down.tm_mday;
    timestamp->hour = (uint8_t)broken_down.tm_hour;
    timestamp->minute = (uint8_t)broken_down.tm_min;
    timestamp->second = (uint8_t)broken_down.tm_sec;
}

void fs_initialize_entry(struct fs_directory_entry *entry,
                         uint8_t type_status,
                         const char *name,
                         uint32_t starting_block,
                         uint32_t number_of_blocks,
                         uint32_t file_size,
                         const struct fs_timestamp *creation_time,
                         const struct fs_timestamp *modification_time)
{
    memset(entry, 0, sizeof(*entry));
    entry->status = (uint8_t)(FS_STATUS_IN_USE | type_status);
    entry->starting_block = starting_block;
    entry->number_of_blocks = number_of_blocks;
    entry->file_size = file_size;
    entry->creation_time = *creation_time;
    entry->modification_time = *modification_time;
    memcpy(entry->name, name, strlen(name) + 1U);
}

static int initialize_empty_directory_block(FILE *image,
                                            const struct fs_superblock *sb,
                                            uint32_t block_number)
{
    unsigned char *buffer;
    size_t offset;
    int status;

    buffer = malloc(sb->block_size);
    if (buffer == NULL) {
        perror("Unable to allocate an empty directory block");
        return -1;
    }

    memset(buffer, 0xFF, sb->block_size);
    for (offset = 0U;
         offset + FS_DIRECTORY_ENTRY_SIZE <= sb->block_size;
         offset += FS_DIRECTORY_ENTRY_SIZE) {
        buffer[offset] = 0U;
    }

    status = fs_write_block(image, sb, block_number, buffer);
    free(buffer);
    return status;
}

static int update_directory_block_count(FILE *image,
                                        struct fs_directory_location *directory,
                                        uint32_t new_count)
{
    struct fs_directory_entry entry;

    if (directory->entry_offset == FS_NO_ENTRY_OFFSET) {
        return -1;
    }

    if (fs_read_directory_entry_at(image,
                                   directory->entry_offset,
                                   &entry) != 0) {
        return -1;
    }

    entry.number_of_blocks = new_count;
    fs_current_timestamp(&entry.modification_time);
    return fs_write_directory_entry_at(image,
                                       directory->entry_offset,
                                       &entry);
}

static int extend_directory(FILE *image,
                            const struct fs_superblock *sb,
                            struct fs_directory_location *directory,
                            uint32_t *new_block)
{
    uint32_t *free_blocks = NULL;
    uint32_t *chain = NULL;
    struct fs_directory_entry dot_entry;
    uint64_t dot_offset;
    uint32_t old_count;
    int find_result;
    int status = -1;

    if (directory->layout != FS_DIRECTORY_FAT_CHAIN ||
        directory->entry_offset == FS_NO_ENTRY_OFFSET) {
        fprintf(stderr, "Error: the root directory is full.\n");
        return -1;
    }

    find_result = fs_find_free_blocks(image, sb, 1U, &free_blocks);
    if (find_result <= 0) {
        if (find_result == 0) {
            fprintf(stderr, "Error: no free block is available.\n");
        }
        return -1;
    }

    if (fs_collect_chain(image,
                         sb,
                         directory->starting_block,
                         directory->number_of_blocks,
                         &chain) != 0) {
        goto cleanup;
    }

    if (initialize_empty_directory_block(image, sb, free_blocks[0]) != 0) {
        goto cleanup;
    }

    if (fs_write_fat_entry(image,
                           sb,
                           free_blocks[0],
                           FS_FAT_EOF) != 0 ||
        fs_write_fat_entry(image,
                           sb,
                           chain[directory->number_of_blocks - 1U],
                           free_blocks[0]) != 0) {
        goto cleanup;
    }

    old_count = directory->number_of_blocks;
    if (update_directory_block_count(image,
                                     directory,
                                     old_count + 1U) != 0) {
        goto cleanup;
    }

    dot_offset = (uint64_t)directory->starting_block * sb->block_size;
    if (fs_read_directory_entry_at(image, dot_offset, &dot_entry) == 0 &&
        fs_entry_is_directory(&dot_entry) &&
        strcmp(dot_entry.name, ".") == 0) {
        dot_entry.number_of_blocks = old_count + 1U;
        fs_current_timestamp(&dot_entry.modification_time);
        if (fs_write_directory_entry_at(image,
                                        dot_offset,
                                        &dot_entry) != 0) {
            goto cleanup;
        }
    }

    directory->number_of_blocks = old_count + 1U;
    *new_block = free_blocks[0];
    status = 0;

cleanup:
    free(chain);
    free(free_blocks);
    return status;
}

int fs_get_free_directory_slot(FILE *image,
                               const struct fs_superblock *sb,
                               struct fs_directory_location *directory,
                               uint64_t *entry_offset)
{
    struct fs_directory_entry *entries = NULL;
    uint64_t *offsets = NULL;
    size_t entry_count = 0U;
    size_t index;
    uint32_t new_block;

    if (image == NULL || sb == NULL || directory == NULL ||
        entry_offset == NULL) {
        fprintf(stderr,
                "Error: invalid argument to fs_get_free_directory_slot.\n");
        return -1;
    }

    if (fs_read_directory(image,
                          sb,
                          directory,
                          &entries,
                          &offsets,
                          &entry_count) != 0) {
        return -1;
    }

    for (index = 0U; index < entry_count; ++index) {
        if (!fs_entry_is_in_use(&entries[index])) {
            *entry_offset = offsets[index];
            free(offsets);
            free(entries);
            return 0;
        }
    }

    free(offsets);
    free(entries);

    if (extend_directory(image, sb, directory, &new_block) != 0) {
        return -1;
    }

    *entry_offset = (uint64_t)new_block * sb->block_size;
    return 0;
}

int fs_create_directory(FILE *image,
                        const struct fs_superblock *sb,
                        struct fs_directory_location *parent,
                        const char *name,
                        struct fs_directory_location *created)
{
    uint32_t *block = NULL;
    uint64_t parent_slot;
    uint64_t dot_slot;
    struct fs_timestamp timestamp;
    struct fs_directory_entry parent_entry;
    struct fs_directory_entry dot_entry;
    int find_result;
    int status = -1;

    if (image == NULL || sb == NULL || parent == NULL || name == NULL ||
        created == NULL || !fs_valid_component(name) ||
        strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        fprintf(stderr, "Error: invalid directory name.\n");
        return -1;
    }

    find_result = fs_find_free_blocks(image, sb, 1U, &block);
    if (find_result <= 0) {
        if (find_result == 0) {
            fprintf(stderr, "Error: no free block is available.\n");
        }
        return -1;
    }

    if (fs_write_fat_entry(image, sb, block[0], FS_FAT_EOF) != 0 ||
        initialize_empty_directory_block(image, sb, block[0]) != 0) {
        goto cleanup;
    }

    if (fs_get_free_directory_slot(image,
                                   sb,
                                   parent,
                                   &parent_slot) != 0) {
        (void)fs_write_fat_entry(image, sb, block[0], FS_FAT_FREE);
        goto cleanup;
    }

    fs_current_timestamp(&timestamp);
    fs_initialize_entry(&parent_entry,
                        FS_STATUS_DIRECTORY,
                        name,
                        block[0],
                        1U,
                        0U,
                        &timestamp,
                        &timestamp);
    if (fs_write_directory_entry_at(image,
                                    parent_slot,
                                    &parent_entry) != 0) {
        (void)fs_write_fat_entry(image, sb, block[0], FS_FAT_FREE);
        goto cleanup;
    }

    fs_initialize_entry(&dot_entry,
                        FS_STATUS_DIRECTORY,
                        ".",
                        block[0],
                        1U,
                        0U,
                        &timestamp,
                        &timestamp);
    dot_slot = (uint64_t)block[0] * sb->block_size;
    if (fs_write_directory_entry_at(image, dot_slot, &dot_entry) != 0) {
        goto cleanup;
    }

    created->starting_block = block[0];
    created->number_of_blocks = 1U;
    created->layout = FS_DIRECTORY_FAT_CHAIN;
    created->entry_offset = parent_slot;
    status = 0;

cleanup:
    free(block);
    return status;
}