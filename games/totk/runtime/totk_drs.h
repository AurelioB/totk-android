/* Independent 1.4.3 handheld DRS edit, from authenticated local game-data facts.
   No game bytes or external optimizer implementation are distributed here.
   Header-only so every existing RomFS build uses the same checked transform. */
#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define TOTK_DRS_PACK "Pack/Bootup.Nin_NX_NVN.pack.zs"
#define TOTK_DRS_PACK_SIZE 286256u
#define TOTK_DRS_PACK_SHA "e551acaff6bf6cbb68fab1ad931adfb9307c21c7671d7474cb6a093252e74441"
#define TOTK_DRS_TEMPLATE_SIZE 1795492u
#define TOTK_DRS_TEMPLATE_SHA "96ca09d2dd2309ce7f1df9d16d699cf0ff231b5f2e47ebeea697243f450389a1"
#define TOTK_DRS_MEMBER "System/GameGraphics/ModelSceneExtensionParam/Default.game__gfx__ModelSceneExtensionParam.bgyml"
#define TOTK_DRS_MEMBER_SIZE 12144u
#define TOTK_DRS_MEMBER_SHA "70b9008243e8c80d6d3aef0272553676eaa0698386755a72fb84142137db91d5"
#define TOTK_DRS_FLOOR_OFFSET 0x14a4u
#define TOTK_DRS_FLOOR_WORD 0x3f4ccccdu
#define TOTK_DRS_BLOCK_SIZE 131072u

typedef void (*TotkDrsHash)(const void *,size_t,char [65]);

static inline uint32_t totk_drs_u32(const uint8_t *p) {
 return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static inline unsigned totk_drs_u16(const uint8_t *p) {return (unsigned)p[0]|(unsigned)p[1]<<8;}
static inline void totk_drs_put32(uint8_t *p,uint32_t v) {for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
/* Reject values that round back to the native .8f floor or to zero as well as
   non-finite, non-positive, out-of-range and partially parsed values. */
static inline int totk_drs_scale(const char *text,uint32_t *word) {
 if(!text||!*text||strchr(" \t\r\n\v\f",*text))return 0;
 char *end;double value=strtod(text,&end);
 if(end==text||*end||!isfinite(value)||value<=0||value>=0.8)return 0;
 float scale=(float)value;if(!isfinite(scale)||scale<=0||scale>=0.8f)return 0;
 memcpy(word,&scale,sizeof(*word));return 1;
}
/* Hash the complete template before interpreting offsets. These structural
   checks additionally bind the selected member, BYML version, float types and
   both native handheld words. No byte is mutated by validation. */
static inline const char *totk_drs_validate(const uint8_t *sarc,size_t size,TotkDrsHash hash,size_t *floor) {
 if(size!=TOTK_DRS_TEMPLATE_SIZE)return "DRS template size mismatch (regenerate from the original 1.4.3 RomFS)";
 char digest[65];hash(sarc,size,digest);
 if(strcmp(digest,TOTK_DRS_TEMPLATE_SHA))return "DRS template SHA-256 mismatch (requires the original unmodified Bootup SARC)";
 if(memcmp(sarc,"SARC",4)||totk_drs_u16(sarc+4)!=20||totk_drs_u16(sarc+6)!=0xfeff||totk_drs_u32(sarc+8)!=size)
  return "DRS template SARC header mismatch";
 size_t data=totk_drs_u32(sarc+12),fat=totk_drs_u16(sarc+4);
 if(data>size||fat+12>data||memcmp(sarc+fat,"SFAT",4)||totk_drs_u16(sarc+fat+4)!=12||totk_drs_u16(sarc+fat+6)!=292)
  return "DRS template must preserve all 292 Bootup members";
 size_t names=fat+12+292*16;
 if(names+8>data||memcmp(sarc+names,"SFNT",4)||totk_drs_u16(sarc+names+4)!=8)return "DRS template SARC name table mismatch";
 names+=8;size_t selected=0;unsigned found=0;
 for(unsigned i=0;i<292;i++){
  const uint8_t *node=sarc+fat+12+i*16;uint32_t attr=totk_drs_u32(node+4);
  size_t lo=totk_drs_u32(node+8),hi=totk_drs_u32(node+12),name=names+(size_t)(attr&0xffffff)*4;
  if((attr>>24)!=1||name>=data||!memchr(sarc+name,0,data-name)||lo>hi||hi>size-data)return "DRS template member bounds mismatch";
  if(!strcmp((const char *)sarc+name,TOTK_DRS_MEMBER)){
   if(++found!=1||hi-lo!=TOTK_DRS_MEMBER_SIZE)return "DRS graphics member identity mismatch";
   selected=data+lo;
  }
 }
 if(found!=1)return "DRS graphics member missing";
 const uint8_t *member=sarc+selected;hash(member,TOTK_DRS_MEMBER_SIZE,digest);
 if(strcmp(digest,TOTK_DRS_MEMBER_SHA))return "DRS graphics member SHA-256 mismatch";
 if(memcmp(member,"YB\7\0",4)||member[0x147f]!=0xd2||member[0x14a3]!=0xd2||
    totk_drs_u32(member+0x1480)!=0x3f666666u||totk_drs_u32(member+TOTK_DRS_FLOOR_OFFSET)!=TOTK_DRS_FLOOR_WORD)
  return "DRS graphics member BYML version, float types or native words mismatch";
 *floor=selected+TOTK_DRS_FLOOR_OFFSET;return NULL;
}
/* Zstandard single-segment frame: four-byte content size, dictionary ID 3,
   raw blocks (<=128 KiB), no checksum. The game still selects its own dictionary
   ID 3; raw blocks need no compression library in the native host. */
static inline size_t totk_drs_frame_size(size_t size) {
 if(size>UINT32_MAX)return 0;
 size_t blocks=size?(size-1)/TOTK_DRS_BLOCK_SIZE+1:1;
 if(size>SIZE_MAX-10-3*blocks)return 0;
 return 10+size+3*blocks;
}
static inline int totk_drs_frame(const uint8_t *sarc,size_t size,uint8_t *out,size_t capacity) {
 size_t total=totk_drs_frame_size(size);if(!total||capacity<total)return 0;
 memcpy(out,"\x28\xb5\x2f\xfd\xa1\x03",6);totk_drs_put32(out+6,(uint32_t)size);
 size_t pos=10,done=0;
 do{
  size_t n=size-done;if(n>TOTK_DRS_BLOCK_SIZE)n=TOTK_DRS_BLOCK_SIZE;
  uint32_t block=(uint32_t)n<<3|(done+n==size);out[pos++]=(uint8_t)block;out[pos++]=(uint8_t)(block>>8);out[pos++]=(uint8_t)(block>>16);
  if(n)memcpy(out+pos,sarc+done,n);
  pos+=n;done+=n;
 }while(done<size);
 return 1;
}
static inline const char *totk_drs_transform(uint8_t *sarc,size_t size,uint32_t word,TotkDrsHash hash,uint8_t **out,size_t *out_size) {
 *out=NULL;*out_size=0;
 float scale;memcpy(&scale,&word,sizeof(scale));
 if(!isfinite(scale)||scale<=0||scale>=0.8f)return "DRS minimum must be finite, positive and below native 0.8";
 size_t floor=0;const char *error=totk_drs_validate(sarc,size,hash,&floor);if(error)return error;
 size_t packed_size=totk_drs_frame_size(size);uint8_t *packed=malloc(packed_size);
 if(!packed)return "DRS packed Bootup allocation failed";
 /* Only the minimum changes. The .9 intermediate step, all other ladders,
    thresholds, priorities, member sizes and SARC accounting stay untouched. */
 totk_drs_put32(sarc+floor,word);
 (void)totk_drs_frame(sarc,size,packed,packed_size);
 *out=packed;*out_size=packed_size;return NULL;
}
