#include "shared.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void print_entry(const struct fs_directory_entry *entry)
{
    char type;

    if (fs_entry_is_file(entry)) {
        type = 'F';
    } else if (fs_entry_is_directory(entry)) {
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

int main(int argc, char *argv[])
{
    FILE *image;
    struct fs_superblock sb;
    struct fs_directory_location location;
    struct fs_directory_entry *entries = NULL;
    size_t entry_count = 0U;
    size_t index;
    int resolve_result;
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

    resolve_result = fs_resolve_directory(image,
                                          &sb,
                                          argv[2],
                                          &location);
    if (resolve_result < 0) {
        goto cleanup;
    }
    if (resolve_result == 0) {
        fprintf(stderr, "Directory not found: %s\n", argv[2]);
        goto cleanup;
    }

    if (fs_read_directory(image,
                          &sb,
                          &location,
                          &entries,
                          NULL,
                          &entry_count) != 0) {
        goto cleanup;
    }

    for (index = 0U; index < entry_count; ++index) {
        print_entry(&entries[index]);
    }

    result = EXIT_SUCCESS;

cleanup:
    free(entries);
    if (fclose(image) != 0) {
        perror("Unable to close file system image");
        result = EXIT_FAILURE;
    }

    return result;
}