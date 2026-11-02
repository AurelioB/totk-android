"""The pinned hactool source for the on-device setup: lib/arm64-v8a/libhactool.so (a PIE executable the setup library
spawns for the update ExeFS and the base + update BKTR RomFS merge) and its host build for the importer tests.

prepare_hactool(work, log) -> Path fetches build_app.HACTOOL into <work>/src/hactool-setup, applies build_app's patches
(private title-key file, bounded base range, the BKTR layout relaxations) and the Android patches below; android/setup/
setup.mk compiles it from there (HACTOOL_SRC)."""
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[5]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
import build_app  # noqa: E402

# The setup passes the base XCI/NSP as an inherited descriptor named /proc/self/fd/N. Android can refuse to reopen that
# path for a storage-provider file (the grant is on the descriptor, not the path), so fall back to a duplicate of the
# descriptor; the parent reads its copy only with pread, so the shared file offset does not matter.
FD_HELPER = r'''
#include <limits.h>
#include <unistd.h>
static FILE *mng_fopen_read(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f || strncmp(path, "/proc/self/fd/", 14)) return f;
    char *end; long n = strtol(path + 14, &end, 10);
    if (end == path + 14 || *end || n < 0 || n > INT_MAX) return NULL;
    int fd = dup((int)n);
    if (fd < 0) return NULL;
    if (!(f = fdopen(fd, "rb"))) close(fd);
    return f;
}
'''
HOOK = 'static FILE *mng_open_base(const char *path)'
CALLS = ('if (!start && !size) return fopen(path, "rb");', 'range->file = fopen(path, "rb");')


def patch_descriptor_fallback(source):
    main = source / 'main.c'
    text = main.read_text()
    if 'static FILE *mng_fopen_read(const char *path)' in text:
        return
    if text.count(HOOK) != 1 or any(text.count(c) != 1 for c in CALLS):
        raise build_app.BuildError('pinned hactool base-file hook changed (android/setup/hactool/deps.py)')
    text = text.replace(HOOK, FD_HELPER + HOOK, 1)
    for call in CALLS:
        text = text.replace(call, call.replace('fopen(path, "rb")', 'mng_fopen_read(path)'))
    main.write_text(text)


# Bionic has no fopencookie but has the BSD funopen build_app.py already uses on macOS; and its FILE is opaque, so the
# --basefake "non-NULL" marker cannot increment a FILE pointer (the setup never passes --basefake).
ANDROID_FIXES = (('#ifdef __APPLE__', '#if defined(__APPLE__) || defined(__ANDROID__)', 2),
                 ('nca_ctx.tool_ctx->base_file++;', 'nca_ctx.tool_ctx->base_file = (FILE *)((char *)NULL + 1);', 1))


def patch_android(source):
    main = source / 'main.c'
    text = main.read_text()
    for old, new, count in ANDROID_FIXES:
        if new in text:
            continue
        if text.count(old) != count:
            raise build_app.BuildError('pinned hactool changed: ' + old + ' (android/setup/hactool/deps.py)')
        text = text.replace(old, new)
    main.write_text(text)


# nca_process_bktr_section spreads the BKTR relocation and subsection buckets apart in place (each one moves up by one
# entry) with memcpy, whose source and destination overlap. glibc's x86-64 memcpy happens to survive that; bionic's arm64
# memcpy does not, so with the real 1.4.3 update (several buckets) lookups read a corrupted table and the merge stopped
# with "Failed to read RomFS directory cache!" (a patch read 14 GB into a 524 MB NCA). An overlapping move is memmove.
BUCKET_MOVES = ('memcpy(bktr_get_relocation_bucket(', 'memcpy(bktr_get_subsection_bucket(')


def patch_bucket_memmove(source):
    nca = source / 'nca.c'
    text = nca.read_text()
    for call in BUCKET_MOVES:
        fixed = call.replace('memcpy', 'memmove')
        if fixed in text and call not in text:
            continue
        if text.count(call) != 1:
            raise build_app.BuildError('pinned hactool changed: ' + call + ' (android/setup/hactool/deps.py)')
        text = text.replace(call, fixed)
    nca.write_text(text)


def prepare_hactool(work, log):
    source = Path(work) / 'src/hactool-setup'
    logs = Path(log).parent
    logs.mkdir(parents=True, exist_ok=True)
    build_app.git_fetch(*build_app.HACTOOL, source, Path(log))
    build_app.patch_hactool(source, logs)
    patch_descriptor_fallback(source)
    patch_bucket_memmove(source)
    patch_android(source)
    return source


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: deps.py <work dir>')
    work = Path(sys.argv[1])
    print(prepare_hactool(work, work / 'logs/fetch-hactool-setup.log'))
