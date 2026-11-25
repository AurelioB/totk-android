/* Run the production manifest loader against local fixture files. */
#include "../games/totk/runtime/romfs.c"

_Noreturn void runtime_fail(const char *message) {
    fprintf(stderr, "%s\n", message);
    exit(2);
}

int main(int argc, char **argv) {
    if (argc != 3 && argc != 4) return 1;
    image_fd = open(argv[1], O_RDONLY);
    if (image_fd < 0) return 1;
    if (argc == 4) {
        nchunks = 2;
        chunk_fds[0] = image_fd;
        chunk_fds[1] = open(argv[3], O_RDONLY);
        chunk_size = 3;
        if (chunk_fds[1] < 0) return 1;
    }

    uint8_t index[64] = {0};
    Entry original = {.size = 6, .len = sizeof "original"};
    memcpy(index + 24, &original, sizeof original);
    memcpy(index + 48, "original", original.len);
    index_bytes = index;
    strings_offset = 48;
    file_count = 1;
    romfs_image_sha256 = "4238001a34084a1e8a3e1dc65e6380dca9b4d85cbd878607fc3716695b7d5ee8";
    romfs_index_sha256 = "3075f88218a91e07c5ae879fcceeaf40046999eaa2df4eaae468dba1e9ea2c469";
    const char *index_digest = getenv("TOTK_ROMFS_TEST_INDEX");
    if (index_digest && strlen(index_digest) == 64) romfs_index_sha256 = index_digest;

    overlay_load(argv[2], 0);
    if (noverlay != 1 || overlays[0].index != 0 || strcmp(overlays[0].name, "original")) return 1;
    free(overlays[0].path);
    free(overlays[0].name);
    free(overlays);
    close(image_fd);
    if (nchunks) close(chunk_fds[1]);
    return 0;
}
