#include "shared.h"
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct directory_location {
    uint32_t starting_block;
    uint32_t number_of_blocks;
    enum fs_directory_layout layout;
};

static int entry_is_in_use(const struct fs_directory_entry *entry)
{
    return (entry->status & FS_STATUS_IN_USE) != 0U;
}

static int entry_is_file(const struct fs_directory_entry *entry)
{
    return entry_is_in_use(entry) &&
           (entry->status & FS_STATUS_FILE) != 0U &&
           (entry->status & FS_STATUS_DIRECTORY) == 0U;
}

static int entry_is_directory(const struct fs_directory_entry *entry)
{
    return entry_is_in_use(entry) &&
           (entry->status & FS_STATUS_DIRECTORY) != 0U &&
           (entry->status & FS_STATUS_FILE) == 0U;
}

static int names_match(const char *entry_name,
                       const char *component,
                       size_t component_length)
{
    return strlen(entry_name) == component_length &&
           memcmp(entry_name, component, component_length) == 0;
}

static int find_subdirectory(FILE *image,
                             const struct fs_superblock *sb,
                             const struct directory_location *current,
                             const char *component,
                             size_t component_length,
                             struct directory_location *next)
{
    struct fs_directory_entry *entries = NULL;
    size_t entry_count = 0U;
    size_t index;
    int found = 0;

    if (fs_read_directory(image,
                          sb,
                          current->starting_block,
                          current->number_of_blocks,
                          current->layout,
                          &entries,
                          &entry_count) != 0) {
        return -1;
    }

    for (index = 0U; index < entry_count; ++index) {
        if (entry_is_directory(&entries[index]) &&
            names_match(entries[index].name,
                        component,
                        component_length)) {
            next->starting_block = entries[index].starting_block;
            next->number_of_blocks = entries[index].number_of_blocks;
            next->layout = FS_DIRECTORY_FAT_CHAIN;
            found = 1;
            break;
        }
    }

    free(entries);
    return found;
}

static int resolve_directory(FILE *image,
                             const struct fs_superblock *sb,
                             const char *path,
                             struct directory_location *location)
{
    const char *cursor;

    if (path[0] != '/') {
        fprintf(stderr, "Error: directory path must begin with '/'.\n");
        return -1;
    }

    location->starting_block = sb->root_start;
    location->number_of_blocks = sb->root_blocks;
    location->layout = FS_DIRECTORY_CONTIGUOUS;

    cursor = path;

    while (*cursor != '\0') {
        const char *component;
        size_t component_length;
        struct directory_location next;
        int find_result;

        while (*cursor == '/') {
            ++cursor;
        }

        if (*cursor == '\0') {
            break;
        }

        component = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
        }
        component_length = (size_t)(cursor - component);

        if (component_length == 1U && component[0] == '.') {
            continue;
        }

        if (component_length > FS_MAX_FILENAME_LENGTH) {
            fprintf(stderr, "Directory not found: %s\n", path);
            return -1;
        }

        find_result = find_subdirectory(image,
                                        sb,
                                        location,
                                        component,
                                        component_length,
                                        &next);
        if (find_result < 0) {
            return -1;
        }
        if (find_result == 0) {
            fprintf(stderr, "Directory not found: %s\n", path);
            return -1;
        }

        *location = next;
    }

    return 0;
}

static void print_entry(const struct fs_directory_entry *entry)
{
    char type;

    if (entry_is_file(entry)) {
        type = 'F';
    } else if (entry_is_directory(entry)) {
        type = 'D';
    } else {
        return;
    }

    printf("%c %10" PRIu32 " %30s %04" PRIu16 "/%02" PRIu8
           "/%02" PRIu8 " %02" PRIu8 ":%02" PRIu8 ":%02" PRIu8 "\n",
           type,
           entry->file_size,
           entry->name,
           entry->creation_time.year,
           entry->creation_time.month,
           entry->creation_time.day,
           entry->creation_time.hour,
           entry->creation_time.minute,
           entry->creation_time.second);
}

static int list_directory(FILE *image,
                          const struct fs_superblock *sb,
                          const struct directory_location *location)
{
    struct fs_directory_entry *entries = NULL;
    size_t entry_count = 0U;
    size_t index;

    if (fs_read_directory(image,
                          sb,
                          location->starting_block,
                          location->number_of_blocks,
                          location->layout,
                          &entries,
                          &entry_count) != 0) {
        return -1;
    }

    for (index = 0U; index < entry_count; ++index) {
        print_entry(&entries[index]);
    }

    free(entries);
    return 0;
}

int main(int argc, char *argv[])
{
    FILE *image;
    struct fs_superblock sb;
    struct directory_location location;
    int result = EXIT_FAILURE;

    if (argc != 3) {
        fprintf(stderr,
                "Usage: %s <file-system-image> <directory-path>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    image = fopen(argv[1], "rb");
    if (image == NULL) {
        perror("Unable to open file system image");
        return EXIT_FAILURE;
    }

    if (fs_read_superblock(image, &sb) != 0) {
        goto cleanup;
    }

    if (resolve_directory(image, &sb, argv[2], &location) != 0) {
        goto cleanup;
    }

    if (list_directory(image, &sb, &location) != 0) {
        goto cleanup;
    }

    result = EXIT_SUCCESS;

cleanup:
    if (fclose(image) != 0) {
        perror("Unable to close file system image");
        result = EXIT_FAILURE;
    }

    return result;
}