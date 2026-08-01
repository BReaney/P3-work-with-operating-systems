

#include "shared.h"
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

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
    if (!fs_valid_component(*filename) ||
        strcmp(*filename, ".") == 0 ||
        strcmp(*filename, "..") == 0) {
        free(*directory_path);
        free(*filename);
        *directory_path = NULL;
        *filename = NULL;
        return -1;
    }

    return 0;
}

static int resolve_or_create_directory(FILE *image,
                                       const struct fs_superblock *sb,
                                       const char *path,
                                       struct fs_directory_location *location)
{
    const char *cursor = path;

    fs_get_root_location(sb, location);

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
            fprintf(stderr, "Error: '..' is not supported in image paths.\n");
            return -1;
        }
        if (length == 0U || length > FS_MAX_FILENAME_LENGTH) {
            fprintf(stderr, "Error: invalid directory component.\n");
            return -1;
        }

        memcpy(component, start, length);
        component[length] = '\0';

        find_result = fs_find_entry(image,
                                    sb,
                                    location,
                                    component,
                                    &entry,
                                    &entry_offset);
        if (find_result < 0) {
            return -1;
        }

        if (find_result == 1) {
            if (!fs_entry_is_directory(&entry)) {
                fprintf(stderr,
                        "Error: path component %s is not a directory.\n",
                        component);
                return -1;
            }
            location->starting_block = entry.starting_block;
            location->number_of_blocks = entry.number_of_blocks;
            location->layout = FS_DIRECTORY_FAT_CHAIN;
            location->entry_offset = entry_offset;
        } else {
            struct fs_directory_location created;

            if (fs_create_directory(image,
                                    sb,
                                    location,
                                    component,
                                    &created) != 0) {
                return -1;
            }
            *location = created;
        }
    }

    return 0;
}

static int source_file_size(FILE *source, uint32_t *size)
{
    off_t end;

    if (fseeko(source, 0, SEEK_END) != 0) {
        perror("Unable to seek in source file");
        return -1;
    }

    end = ftello(source);
    if (end < 0) {
        perror("Unable to determine source file size");
        return -1;
    }

    if ((uint64_t)end > UINT32_MAX) {
        fprintf(stderr,
                "Error: source file is larger than the file system format supports.\n");
        return -1;
    }

    if (fseeko(source, 0, SEEK_SET) != 0) {
        perror("Unable to rewind source file");
        return -1;
    }

    *size = (uint32_t)end;
    return 0;
}

static int write_source_blocks(FILE *source,
                               FILE *image,
                               const struct fs_superblock *sb,
                               const uint32_t *blocks,
                               uint32_t block_count,
                               uint32_t file_size)
{
    unsigned char *buffer;
    uint64_t remaining = file_size;
    uint32_t index;
    int status = -1;

    if (block_count == 0U) {
        return file_size == 0U ? 0 : -1;
    }

    buffer = malloc(sb->block_size);
    if (buffer == NULL) {
        perror("Unable to allocate a file block buffer");
        return -1;
    }

    for (index = 0U; index < block_count; ++index) {
        size_t amount = remaining < sb->block_size
                            ? (size_t)remaining
                            : sb->block_size;

        memset(buffer, 0, sb->block_size);
        if (amount > 0U && fread(buffer, 1U, amount, source) != amount) {
            if (ferror(source)) {
                perror("Unable to read source file");
            } else {
                fprintf(stderr, "Error: source file ended unexpectedly.\n");
            }
            goto cleanup;
        }

        if (fs_write_block(image, sb, blocks[index], buffer) != 0) {
            goto cleanup;
        }
        remaining -= amount;
    }

    if (remaining != 0U) {
        fprintf(stderr, "Error: not all source data was written.\n");
        goto cleanup;
    }

    status = 0;

cleanup:
    free(buffer);
    return status;
}

int main(int argc, char *argv[])
{
    FILE *source = NULL;
    FILE *image = NULL;
    struct fs_superblock sb;
    struct fs_directory_location parent;
    struct fs_directory_entry existing_entry;
    struct fs_directory_entry new_entry;
    struct fs_timestamp now;
    char *directory_path = NULL;
    char *filename = NULL;
    uint64_t target_offset = 0U;
    uint32_t source_size;
    uint32_t required_blocks;
    uint64_t required_blocks_value;
    uint32_t old_count = 0U;
    uint32_t reused_count;
    uint32_t extra_count;
    uint32_t *old_blocks = NULL;
    uint32_t *extra_blocks = NULL;
    uint32_t *selected_blocks = NULL;
    int find_result;
    int free_result;
    int result = EXIT_FAILURE;

    if (argc != 4) {
        fprintf(stderr,
                "Usage: %s <file-system-image> <source-file> "
                "<image-file-path>\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    source = fopen(argv[2], "rb");
    if (source == NULL) {
        printf("Source file %s not found.\n", argv[2]);
        goto cleanup;
    }

    if (source_file_size(source, &source_size) != 0) {
        goto cleanup;
    }

    if (split_image_path(argv[3], &directory_path, &filename) != 0) {
        fprintf(stderr, "Error: invalid destination path: %s\n", argv[3]);
        goto cleanup;
    }

    image = fopen(argv[1], "r+b");
    if (image == NULL) {
        perror("Unable to open file system image");
        goto cleanup;
    }

    if (fs_read_superblock(image, &sb) != 0) {
        goto cleanup;
    }

    if (resolve_or_create_directory(image,
                                    &sb,
                                    directory_path,
                                    &parent) != 0) {
        goto cleanup;
    }

    find_result = fs_find_entry(image,
                                &sb,
                                &parent,
                                filename,
                                &existing_entry,
                                &target_offset);
    if (find_result < 0) {
        goto cleanup;
    }
    if (find_result == 1 && !fs_entry_is_file(&existing_entry)) {
        fprintf(stderr,
                "Error: destination name already belongs to a directory.\n");
        goto cleanup;
    }

    if (find_result == 0 &&
        fs_get_free_directory_slot(image,
                                   &sb,
                                   &parent,
                                   &target_offset) != 0) {
        goto cleanup;
    }

    required_blocks_value = source_size == 0U
                                ? 0U
                                : ((uint64_t)source_size +
                                   sb.block_size - 1U) /
                                      sb.block_size;
    if (required_blocks_value > UINT32_MAX ||
        required_blocks_value > sb.block_count) {
        fprintf(stderr,
                "Error: source file requires too many file-system blocks.\n");
        goto cleanup;
    }
    required_blocks = (uint32_t)required_blocks_value;

    if (find_result == 1) {
        old_count = existing_entry.number_of_blocks;
        if (old_count > 0U &&
            fs_collect_chain(image,
                             &sb,
                             existing_entry.starting_block,
                             old_count,
                             &old_blocks) != 0) {
            goto cleanup;
        }
    }

    reused_count = required_blocks < old_count
                       ? required_blocks
                       : old_count;
    extra_count = required_blocks - reused_count;

    if (extra_count > 0U) {
        free_result = fs_find_free_blocks(image,
                                          &sb,
                                          extra_count,
                                          &extra_blocks);
        if (free_result < 0) {
            goto cleanup;
        }
        if (free_result == 0) {
            fprintf(stderr,
                    "Error: insufficient free space in the file system image.\n");
            goto cleanup;
        }
    }

    if (required_blocks > 0U) {
        uint32_t index;

        selected_blocks = malloc((size_t)required_blocks *
                                 sizeof(*selected_blocks));
        if (selected_blocks == NULL) {
            perror("Unable to allocate a file block list");
            goto cleanup;
        }

        for (index = 0U; index < reused_count; ++index) {
            selected_blocks[index] = old_blocks[index];
        }
        for (index = 0U; index < extra_count; ++index) {
            selected_blocks[reused_count + index] = extra_blocks[index];
        }

        if (fs_set_chain(image,
                         &sb,
                         selected_blocks,
                         required_blocks) != 0 ||
            write_source_blocks(source,
                                image,
                                &sb,
                                selected_blocks,
                                required_blocks,
                                source_size) != 0) {
            goto cleanup;
        }
    }

    fs_current_timestamp(&now);
    if (find_result == 1) {
        fs_initialize_entry(&new_entry,
                            FS_STATUS_FILE,
                            filename,
                            required_blocks == 0U
                                ? 0U
                                : selected_blocks[0],
                            required_blocks,
                            source_size,
                            &existing_entry.creation_time,
                            &now);
    } else {
        fs_initialize_entry(&new_entry,
                            FS_STATUS_FILE,
                            filename,
                            required_blocks == 0U
                                ? 0U
                                : selected_blocks[0],
                            required_blocks,
                            source_size,
                            &now,
                            &now);
    }

    if (fs_write_directory_entry_at(image,
                                    target_offset,
                                    &new_entry) != 0) {
        goto cleanup;
    }

    if (old_count > reused_count &&
        fs_free_blocks(image,
                       &sb,
                       old_blocks + reused_count,
                       old_count - reused_count) != 0) {
        goto cleanup;
    }

    if (fflush(image) != 0) {
        perror("Unable to flush file system image");
        goto cleanup;
    }

    result = EXIT_SUCCESS;

cleanup:
    free(selected_blocks);
    free(extra_blocks);
    free(old_blocks);
    free(filename);
    free(directory_path);
    if (image != NULL && fclose(image) != 0) {
        perror("Unable to close file system image");
        result = EXIT_FAILURE;
    }
    if (source != NULL && fclose(source) != 0) {
        perror("Unable to close source file");
        result = EXIT_FAILURE;
    }
    return result;
}