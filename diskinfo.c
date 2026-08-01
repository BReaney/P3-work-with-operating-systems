#include <arpa/inet.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define _POSIX_C_SOURCE 200809L
#define SUPERBLOCK_SIZE 512U
#define FILE_SYSTEM_ID_SIZE 8U
#define FAT_ENTRY_SIZE 4U

#define OFFSET_FILE_SYSTEM_ID 0U
#define OFFSET_BLOCK_SIZE 8U
#define OFFSET_BLOCK_COUNT 10U
#define OFFSET_FAT_START 14U
#define OFFSET_FAT_BLOCKS 18U
#define OFFSET_ROOT_START 22U
#define OFFSET_ROOT_BLOCKS 26U

#define FAT_FREE 0x00000000U
#define FAT_RESERVED 0x00000001U

struct superblock {
    uint16_t block_size;
    uint32_t block_count;
    uint32_t fat_start;
    uint32_t fat_blocks;
    uint32_t root_start;
    uint32_t root_blocks;
};

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

static int read_superblock(FILE *image, struct superblock *sb)
{
    unsigned char buffer[SUPERBLOCK_SIZE];
    static const unsigned char expected_id[FILE_SYSTEM_ID_SIZE] = {
        'C', 'S', 'C', '3', '6', '0', 'F', 'S'
    };

    if (fseeko(image, 0, SEEK_SET) != 0) {
        perror("Unable to seek to the superblock");
        return -1;
    }

    if (fread(buffer, 1, sizeof(buffer), image) != sizeof(buffer)) {
        if (ferror(image)) {
            perror("Unable to read the superblock");
        } else {
            fprintf(stderr, "Error: file system image is smaller than one block.\n");
        }
        return -1;
    }

    if (memcmp(buffer + OFFSET_FILE_SYSTEM_ID,
               expected_id,
               FILE_SYSTEM_ID_SIZE) != 0) {
        fprintf(stderr, "Error: invalid file system identifier.\n");
        return -1;
    }

    sb->block_size = read_be16(buffer + OFFSET_BLOCK_SIZE);
    sb->block_count = read_be32(buffer + OFFSET_BLOCK_COUNT);
    sb->fat_start = read_be32(buffer + OFFSET_FAT_START);
    sb->fat_blocks = read_be32(buffer + OFFSET_FAT_BLOCKS);
    sb->root_start = read_be32(buffer + OFFSET_ROOT_START);
    sb->root_blocks = read_be32(buffer + OFFSET_ROOT_BLOCKS);

    if (sb->block_size == 0U || sb->block_count == 0U || sb->fat_blocks == 0U) {
        fprintf(stderr, "Error: invalid zero-valued superblock field.\n");
        return -1;
    }

    return 0;
}

static int count_fat_entries(FILE *image,
                             const struct superblock *sb,
                             uint32_t *free_blocks,
                             uint32_t *reserved_blocks,
                             uint32_t *allocated_blocks)
{
    unsigned char entry_buffer[FAT_ENTRY_SIZE];
    uint64_t fat_capacity;
    uint64_t fat_offset_value;
    off_t fat_offset;
    uint32_t i;

    fat_capacity = ((uint64_t)sb->fat_blocks * sb->block_size) / FAT_ENTRY_SIZE;
    if (fat_capacity < sb->block_count) {
        fprintf(stderr,
                "Error: FAT is too small for the declared block count.\n");
        return -1;
    }

    fat_offset_value = (uint64_t)sb->fat_start * sb->block_size;
    fat_offset = (off_t)fat_offset_value;
    if ((uint64_t)fat_offset != fat_offset_value) {
        fprintf(stderr, "Error: FAT offset cannot be represented on this system.\n");
        return -1;
    }

    if (fseeko(image, fat_offset, SEEK_SET) != 0) {
        perror("Unable to seek to the FAT");
        return -1;
    }

    *free_blocks = 0U;
    *reserved_blocks = 0U;
    *allocated_blocks = 0U;

    for (i = 0U; i < sb->block_count; ++i) {
        uint32_t entry;

        if (fread(entry_buffer, 1, sizeof(entry_buffer), image) !=
            sizeof(entry_buffer)) {
            if (ferror(image)) {
                perror("Unable to read the FAT");
            } else {
                fprintf(stderr, "Error: unexpected end of FAT.\n");
            }
            return -1;
        }

        entry = read_be32(entry_buffer);

        if (entry == FAT_FREE) {
            ++(*free_blocks);
        } else if (entry == FAT_RESERVED) {
            ++(*reserved_blocks);
        } else {
            /* Includes normal chain entries and the 0xFFFFFFFF EOF marker. */
            ++(*allocated_blocks);
        }
    }

    return 0;
}

int main(int argc, char *argv[])
{
    FILE *image;
    struct superblock sb;
    uint32_t free_blocks;
    uint32_t reserved_blocks;
    uint32_t allocated_blocks;
    int result = EXIT_FAILURE;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file-system-image>\n", argv[0]);
        return EXIT_FAILURE;
    }

    image = fopen(argv[1], "rb");
    if (image == NULL) {
        perror("Unable to open file system image");
        return EXIT_FAILURE;
    }

    if (read_superblock(image, &sb) != 0) {
        goto cleanup;
    }

    if (count_fat_entries(image,
                          &sb,
                          &free_blocks,
                          &reserved_blocks,
                          &allocated_blocks) != 0) {
        goto cleanup;
    }

    printf("Super block information:\n");
    printf("Block size: %" PRIu16 "\n", sb.block_size);
    printf("Block count: %" PRIu32 "\n", sb.block_count);
    printf("FAT starts: %" PRIu32 "\n", sb.fat_start);
    printf("FAT blocks: %" PRIu32 "\n", sb.fat_blocks);
    printf("Root directory start: %" PRIu32 "\n", sb.root_start);
    printf("Root directory blocks: %" PRIu32 "\n", sb.root_blocks);
    printf("\n");
    printf("FAT information:\n");
    printf("Free Blocks: %" PRIu32 "\n", free_blocks);
    printf("Reserved Blocks: %" PRIu32 "\n", reserved_blocks);
    printf("Allocated Blocks: %" PRIu32 "\n", allocated_blocks);

    result = EXIT_SUCCESS;

cleanup:
    if (fclose(image) != 0) {
        perror("Unable to close file system image");
        result = EXIT_FAILURE;
    }

    return result;
}