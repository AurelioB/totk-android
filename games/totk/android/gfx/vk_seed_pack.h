#ifndef TOTK_VK_SEED_PACK_H
#define TOTK_VK_SEED_PACK_H

#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Read-only view of the deterministic TOTKVKP1 seed pack. */
typedef struct vk_seed_pack { void *mapping; size_t size; uint64_t count; int fd; } vk_seed_pack;

typedef struct vk_seed_pack_range {
    uint64_t begin, end;
} vk_seed_pack_range;
static inline uint32_t vk_seed_pack_le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t vk_seed_pack_le64(const unsigned char *p) {
    return (uint64_t)vk_seed_pack_le32(p) | (uint64_t)vk_seed_pack_le32(p + 4) << 32;
}

static inline int vk_seed_pack_range_compare(const void *a, const void *b) {
    const vk_seed_pack_range *x = (const vk_seed_pack_range *)a;
    const vk_seed_pack_range *y = (const vk_seed_pack_range *)b;
    if (x->begin < y->begin) return -1;
    if (x->begin > y->begin) return 1;
    if (x->end < y->end) return -1;
    if (x->end > y->end) return 1;
    return 0;
}

static inline int vk_seed_pack_valid_name(const unsigned char *name, size_t *length) {
    size_t n = 0;
    while (n < 84 && name[n]) {
        unsigned char c = name[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
        n++;
    }
    if (!n || n > 83) return 0;
    for (size_t i = n + 1; i < 84; i++) if (name[i]) return 0;
    if ((n >= 4 && !memcmp(name + n - 4, ".spv", 4) && n > 4) ||
        (n >= 3 && !memcmp(name + n - 3, ".vk", 3) && n > 3) ||
        (n >= 3 && !memcmp(name + n - 3, ".vv", 3) && n > 3)) {
        *length = n;
        return 1;
    }
    return 0;
}

static inline int vk_seed_pack_open(vk_seed_pack *pack, const char *path) {
    static const unsigned char magic[8] = {'T','O','T','K','V','K','P','1'};
    struct stat st;
    int fd = -1, result = -1;
    void *mapping = MAP_FAILED;
    vk_seed_pack_range *ranges = NULL;
    uint64_t count = 0;
    size_t file_size = 0;

    if (!pack) return -1;
    memset(pack, 0, sizeof *pack);
    if (!path) return -1;
    pack->fd = -1;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) goto done;
    if (fstat(fd, &st) < 0) goto done;
    if (st.st_size < 0 || (uintmax_t)st.st_size > SIZE_MAX) goto done;
    file_size = (size_t)st.st_size;
    if (file_size < 32) { result = -2; goto done; }
    mapping = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapping == MAP_FAILED) goto done;
    {
        const unsigned char *b = (const unsigned char *)mapping;
        uint32_t version = vk_seed_pack_le32(b + 8);
        uint32_t record_size = vk_seed_pack_le32(b + 12);
        count = vk_seed_pack_le64(b + 16);
        uint64_t total_size = vk_seed_pack_le64(b + 24);
        if (memcmp(b, magic, sizeof magic) || version != 1 || record_size != 104 ||
            total_size != (uint64_t)file_size ||
            count > (uint64_t)((file_size - 32) / 104)) {
            result = -2; goto done;
        }
        uint64_t index_end64 = 32 + count * 104;
        if (index_end64 > SIZE_MAX || index_end64 > file_size) { result = -2; goto done; }
        if (count > SIZE_MAX / sizeof *ranges) { result = -2; goto done; }
        if (count) {
            ranges = (vk_seed_pack_range *)malloc((size_t)count * sizeof *ranges);
            if (!ranges) goto done;
        }
        const unsigned char *previous = NULL;
        size_t previous_length = 0;
        uint64_t range_count = 0;
        for (uint64_t i = 0; i < count; i++) {
            const unsigned char *record = b + 32 + (size_t)i * 104;
            size_t name_length;
            uint64_t offset, length, end;
            if (!vk_seed_pack_valid_name(record, &name_length)) { result = -2; goto done; }
            if (previous) {
                size_t common = previous_length < name_length ? previous_length : name_length;
                int order = memcmp(previous, record, common);
                if (order > 0 || (order == 0 && previous_length >= name_length)) { result = -2; goto done; }
            }
            previous = record;
            previous_length = name_length;
            uint32_t reserved = vk_seed_pack_le32(record + 84);
            offset = vk_seed_pack_le64(record + 88);
            length = vk_seed_pack_le64(record + 96);
            if (reserved || (offset & 7) || offset < index_end64 ||
                offset > (uint64_t)file_size || length > UINT64_MAX - offset) {
                result = -2; goto done;
            }
            end = offset + length;
            if (end > (uint64_t)file_size) { result = -2; goto done; }
            if (length) ranges[range_count++] = (vk_seed_pack_range){offset, end};
        }
        if (range_count > 1)
            qsort(ranges, (size_t)range_count, sizeof *ranges, vk_seed_pack_range_compare);
        for (uint64_t i = 1; i < range_count; i++)
            if (ranges[i].begin < ranges[i - 1].end) { result = -2; goto done; }
    }
    pack->mapping = mapping;
    pack->size = file_size;
    pack->count = count;
    pack->fd = fd; fd = -1; /* pin this inode across atomic deployment replacements */
    mapping = MAP_FAILED;
    result = 0;
done:
    free(ranges);
    if (mapping != MAP_FAILED) munmap(mapping, file_size);
    if (fd >= 0) close(fd);
    if (result) memset(pack, 0, sizeof *pack);
    return result;
}

static inline int vk_seed_pack_entry(const vk_seed_pack *pack, uint64_t index,
                              const char **name, const void **data, size_t *size) {
    const unsigned char *record;
    uint64_t offset, length;
    if (!pack || !pack->mapping || index >= pack->count || !name || !data || !size) return 0;
    record = (const unsigned char *)pack->mapping + 32 + (size_t)index * 104;
    offset = vk_seed_pack_le64(record + 88);
    length = vk_seed_pack_le64(record + 96);
    *name = (const char *)record;
    *data = (const unsigned char *)pack->mapping + (size_t)offset;
    *size = (size_t)length;
    return 1;
}

static inline void vk_seed_pack_close(vk_seed_pack *pack) {
    if (!pack) return;
    if (pack->mapping) munmap(pack->mapping, pack->size);
    if (pack->mapping && pack->fd >= 0) close(pack->fd);
    memset(pack, 0, sizeof *pack);
}

#endif
