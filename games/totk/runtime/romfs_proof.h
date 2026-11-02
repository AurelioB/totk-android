/* The RomFS verification records in <TOTK_ROMFS_PROOF_DIR>: after one full SHA-256 of the image, the file's identity and
   times, so later starts can skip the 17 GB hash while the same unchanged file is in place. .romfs-verified-v1 holds one
   record for romfs.bin; .romfs-verified-parts-v1 one record per part of a split image (romfs.bin.000 ...), in part order.
   Written by runtime/romfs.c after its own hash and by the Android setup (android/setup/setup.c, move.c) after it verified
   the image it imported or moved; both must produce the same bytes for the same files, hence this one definition. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>

#define ROMFS_PROOF_FILE ".romfs-verified-v1"
#define ROMFS_PROOF_PARTS_FILE ".romfs-verified-parts-v1"
typedef struct {
 char magic[16], sha256[65];
 uint64_t dev,ino,size;
 int64_t mtime_sec,mtime_nsec,ctime_sec,ctime_nsec,birth_sec,birth_nsec;
} RomfsProof;
/* sha256: the image's expected lowercase hex digest (64 characters). Padding bytes are zeroed so the record's bytes depend
   only on the file. */
static inline RomfsProof romfs_proof_for(const struct stat *s, const char *sha256) {
 RomfsProof p;memset(&p,0,sizeof p);memcpy(p.magic,"TOTKRFS-PROOF-1",16);memcpy(p.sha256,sha256,65);
 p.dev=(uint64_t)s->st_dev;p.ino=(uint64_t)s->st_ino;p.size=(uint64_t)s->st_size;
#if defined(__APPLE__)
 p.mtime_sec=s->st_mtimespec.tv_sec;p.mtime_nsec=s->st_mtimespec.tv_nsec;
 p.ctime_sec=s->st_ctimespec.tv_sec;p.ctime_nsec=s->st_ctimespec.tv_nsec;
 p.birth_sec=s->st_birthtimespec.tv_sec;p.birth_nsec=s->st_birthtimespec.tv_nsec;
#else   /* Linux has no birth time in struct stat: the inode, size and change times identify the file */
 p.mtime_sec=s->st_mtim.tv_sec;p.mtime_nsec=s->st_mtim.tv_nsec;
 p.ctime_sec=s->st_ctim.tv_sec;p.ctime_nsec=s->st_ctim.tv_nsec;
#endif
 return p;
}
/* One part of a split image. ctime is left out: reinstalling the iOS app touches the data container's file metadata
   (ctime) without changing a byte, which made every launch hash 17 GB again. A content change still moves mtime (and
   size). */
static inline RomfsProof romfs_proof_part_for(const struct stat *s, const char *sha256) {
 RomfsProof p=romfs_proof_for(s,sha256);p.ctime_sec=p.ctime_nsec=0;return p;
}

/* Removable cards (exFAT/FAT behind Android's FUSE view) give a file a new device and inode number after every remount
   (measured on the AYN Thor: dev 221 -> 216, ino 1182100 -> 1187143 across `sm unmount` + `sm mount`; size, mtime and
   ctime kept), so the records above stop matching and a start would hash 17 GB from the card again. The sidecar
   .romfs-samples-v1 then lets an earlier full hash stand when every file still has its recorded size and mtime AND
   still holds the same bytes at sampled places: its first and last MiB and 64 blocks of 64 KiB whose offsets come
   from a random 32-byte salt kept only in the app's private folder (so the places cannot be predicted from outside).
   Any difference means one full hash, which writes new records. */
#define ROMFS_SAMPLES_FILE ".romfs-samples-v1"
#define ROMFS_SAMPLE_EDGE (1u << 20)
#define ROMFS_SAMPLE_BLOCK (64u << 10)
#define ROMFS_SAMPLE_COUNT 64
#define ROMFS_SAMPLE_MAX_PARTS 64
typedef struct {
 char magic[16];
 uint8_t salt[32];
 uint32_t nparts, reserved;
 uint8_t digest[ROMFS_SAMPLE_MAX_PARTS][32];
} RomfsSamples;
/* read: 0 when n bytes at off of the file were read; digest: SHA-256 of a buffer. */
typedef int (*RomfsSampleRead)(void *user, int part, uint64_t off, void *buf, size_t n);
typedef void (*RomfsSampleDigest)(const void *data, size_t n, uint8_t out[32]);
static inline uint64_t romfs_sample_mix(uint64_t *s) {   /* splitmix64 */
 uint64_t z=(*s+=0x9e3779b97f4a7c15ull);z=(z^(z>>30))*0xbf58476d1ce4e5b9ull;z=(z^(z>>27))*0x94d049bb133111ebull;return z^(z>>31);
}
/* The digest of one file's samples (salt, part number and size are hashed in too); -1 when a read failed. buf: caller's
   scratch of at least 32 + 16 + 2 * ROMFS_SAMPLE_EDGE + ROMFS_SAMPLE_COUNT * ROMFS_SAMPLE_BLOCK bytes. */
static inline int romfs_samples_part(const uint8_t salt[32], int part, uint64_t size, RomfsSampleRead rd, void *user,
                                     RomfsSampleDigest dg, uint8_t *buf, uint8_t out[32]) {
 size_t at=0;memcpy(buf,salt,32);at+=32;
 uint64_t p=(uint64_t)part;memcpy(buf+at,&p,8);memcpy(buf+at+8,&size,8);at+=16;
 uint64_t edge=size<ROMFS_SAMPLE_EDGE?size:ROMFS_SAMPLE_EDGE;
 if(edge&&rd(user,part,0,buf+at,(size_t)edge))return -1;
 at+=(size_t)edge;
 if(edge&&rd(user,part,size-edge,buf+at,(size_t)edge))return -1;
 at+=(size_t)edge;
 uint64_t seed=0;memcpy(&seed,salt,8);seed^=(uint64_t)part*0x100000001b3ull;
 for(int i=0;i<ROMFS_SAMPLE_COUNT&&size>ROMFS_SAMPLE_BLOCK;i++){
  uint64_t off=romfs_sample_mix(&seed)%(size-ROMFS_SAMPLE_BLOCK+1);
  if(rd(user,part,off,buf+at,ROMFS_SAMPLE_BLOCK))return -1;
  at+=ROMFS_SAMPLE_BLOCK;
 }
 dg(buf,at,out);return 0;
}
#define ROMFS_SAMPLE_SCRATCH (32 + 16 + 2 * (size_t)ROMFS_SAMPLE_EDGE + (size_t)ROMFS_SAMPLE_COUNT * ROMFS_SAMPLE_BLOCK)
/* Same size and modification time: what must still hold before samples may stand in for the device and inode. */
static inline int romfs_proof_same_content_times(const RomfsProof *a, const RomfsProof *b) {
 return !memcmp(a->magic,b->magic,sizeof a->magic)&&!memcmp(a->sha256,b->sha256,sizeof a->sha256)&&a->size==b->size&&
  a->mtime_sec==b->mtime_sec&&a->mtime_nsec==b->mtime_nsec;
}
/* Fills s for nparts files (1 for one romfs.bin) with a fresh salt from the caller; -1 on a read failure. */
static inline int romfs_samples_make(RomfsSamples *s, const uint8_t salt[32], int nparts, const uint64_t *sizes,
                                     RomfsSampleRead rd, void *user, RomfsSampleDigest dg, uint8_t *scratch) {
 if(nparts<1||nparts>ROMFS_SAMPLE_MAX_PARTS)return -1;
 memset(s,0,sizeof *s);memcpy(s->magic,"TOTKRFS-SAMPLE1",16);memcpy(s->salt,salt,32);s->nparts=(uint32_t)nparts;
 for(int i=0;i<nparts;i++)if(romfs_samples_part(salt,i,sizes[i],rd,user,dg,scratch,s->digest[i]))return -1;
 return 0;
}
/* 1 when the files still give the recorded sample digests. */
static inline int romfs_samples_match(const RomfsSamples *s, int nparts, const uint64_t *sizes, RomfsSampleRead rd, void *user,
                                      RomfsSampleDigest dg, uint8_t *scratch) {
 if(memcmp(s->magic,"TOTKRFS-SAMPLE1",16)||s->nparts!=(uint32_t)nparts)return 0;
 for(int i=0;i<nparts;i++){uint8_t d[32];if(romfs_samples_part(s->salt,i,sizes[i],rd,user,dg,scratch,d)||memcmp(d,s->digest[i],32))return 0;}
 return 1;
}
