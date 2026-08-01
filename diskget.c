#include "shared.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int split_image_path(const char *path,
                            char **directory_path,
                            char **filename)
{
    char *copy;
    char *last_slash;
    size_t length;

    *directory_path = NULL;
    *filename = NULL;

    if (path == NULL || path[0] == '\0') {
        return -1;
    }

    length = strlen(path);
    if (path[length - 1U] == '/') {
        return -1;
    }

    copy = malloc(length + 1U);
    if (copy == NULL) {
        perror("Unable to allocate path storage");
        return -1;
    }
    memcpy(copy, path, length + 1U);

    last_slash = strrchr(copy, '/');
    if (last_slash == NULL) {
        *directory_path = malloc(2U);
        *filename = malloc(length + 1U);
        if (*directory_path == NULL || *filename == NULL) {
            perror("Unable to allocate path storage");
            free(copy);
            free(*directory_path);
            free(*filename);
            *directory_path = NULL;
            *filename = NULL;
            return -1;
        }
        memcpy(*directory_path, "/", 2U);
        memcpy(*filename, copy, length + 1U);
    } else {
        size_t filename_length = strlen(last_slash + 1);
        size_t directory_length = (size_t)(last_slash - copy);

        *filename = malloc(filename_length + 1U);
        if (*filename == NULL) {
            perror("Unable to allocate path storage");
            free(copy);
            return -1;
        }
        memcpy(*filename, last_slash + 1, filename_length + 1U);

        if (directory_length == 0U) {
            *directory_path = malloc(2U);
            if (*directory_path != NULL) {
                memcpy(*directory_path, "/", 2U);
            }
        } else {
            *directory_path = malloc(directory_length + 1U);
            if (*directory_path != NULL) {
                memcpy(*directory_path, copy, directory_length);
                (*directory_path)[directory_length] = '\0';
            }
        }

        if (*directory_path == NULL) {
            perror("Unable to allocate path storage");
            free(copy);
            free(*filename);
            *filename = NULL;
            return -1;
        }
    }

    free(copy);
    if (!fs_valid_component(*filename)) {
        free(*directory_path);
        free(*filename);
        *directory_path = NULL;
        *filename = NULL;
        return -1;
    }

    return 0;
}

static int copy_file_from_image(FILE *image,
                                const struct fs_superblock *sb,
                                const struct fs_directory_entry *entry,
                                FILE *output)
{
    uint32_t *blocks = NULL;
    unsigned char *buffer = NULL;
    uint64_t remaining = entry->file_size;
    uint64_t required_blocks =
        (remaining + sb->block_size - 1U) / sb->block_size;
    uint32_t index;
    int status = -1;

    if (required_blocks != entry->number_of_blocks) {
        fprintf(stderr,
                "Error: file size and directory block count disagree.\n");
        return -1;
    }

    if (entry->number_of_blocks == 0U) {
        return entry->file_size == 0U ? 0 : -1;
    }

    if (fs_collect_chain(image,
                         sb,
                         entry->starting_block,
                         entry->number_of_blocks,
                         &blocks) != 0) {
        return -1;
    }

    buffer = malloc(sb->block_size);
    if (buffer == NULL) {
        perror("Unable to allocate a file block buffer");
        goto cleanup;
    }

    for (index = 0U; index < entry->number_of_blocks; ++index) {
        size_t amount = remaining < sb->block_size
                            ? (size_t)remaining
                            : sb->block_size;

        if (fs_read_block(image, sb, blocks[index], buffer) != 0) {
            goto cleanup;
        }

        if (amount > 0U && fwrite(buffer, 1U, amount, output) != amount) {
            perror("Unable to write destination file");
            goto cleanup;
        }

        remaining -= amount;
    }

    if (remaining != 0U) {
        fprintf(stderr, "Error: file data ended unexpectedly.\n");
        goto cleanup;
    }

    status = 0;

cleanup:
    free(buffer);
    free(blocks);
    return status;
}

int main(int argc, char *argv[])
{
    FILE *image = NULL;
    FILE *output = NULL;
    struct fs_superblock sb;
    struct fs_directory_location directory;
    struct fs_directory_entry entry;
    char *directory_path = NULL;
    char *filename = NULL;
    int resolve_result;
    int find_result;
    int result = EXIT_FAILURE;

    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s <file-system-image> <image-file-path> "
                "<destination-file>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    if (split_image_path(argv[2], &directory_path, &filename) != 0) {
        fprintf(stderr, "Error: invalid file path: %s\n", argv[2]);
        goto cleanup;
    }

    image = fopen(argv[1], "rb");
    if (image == NULL) {
        perror("Unable to open file system image");
        goto cleanup;
    }

    if (fs_read_superblock(image, &sb) != 0) {
        goto cleanup;
    }

    resolve_result = fs_resolve_directory(image,
                                          &sb,
                                          directory_path,
                                          &directory);
    if (resolve_result < 0) {
        goto cleanup;
    }

    if (resolve_result == 0) {
        printf("Requested file %s not found in %s.\n",
               filename,
               directory_path);
        goto cleanup;
    }

    find_result = fs_find_entry(image,
                                &sb,
                                &directory,
                                filename,
                                &entry,
                                NULL);
    if (find_result < 0) {
        goto cleanup;
    }
    if (find_result == 0 || !fs_entry_is_file(&entry)) {
        printf("Requested file %s not found in %s.\n",
               filename,
               directory_path);
        goto cleanup;
    }

    output = fopen(argv[3], "wb");
    if (output == NULL) {
        perror("Unable to create destination file");
        goto cleanup;
    }

    if (copy_file_from_image(image, &sb, &entry, output) != 0) {
        goto cleanup;
    }

    result = EXIT_SUCCESS;

cleanup:
    if (output != NULL && fclose(output) != 0) {
        perror("Unable to close destination file");
        result = EXIT_FAILURE;
    }
    if (image != NULL && fclose(image) != 0) {
        perror("Unable to close file system image");
        result = EXIT_FAILURE;
    }
    free(filename);
    free(directory_path);
    return result;
}