/* SHA-256 for the setup checks. The 17 GB RomFS is hashed on the device, so arm64 uses the ARMv8 SHA2 instructions when
   the CPU reports them (every supported Snapdragon does); the portable rounds are the fallback and the host build. */
#include "setup_internal.h"
#include <stdio.h>
#include <string.h>
#if defined(__aarch64__)
#include <arm_neon.h>
#include <sys/auxv.h>
#ifndef HWCAP_SHA2
#define HWCAP_SHA2 (1 << 6)
#endif
#endif

static const uint32_t K[64]={
 0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
 0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
 0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
 0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
 0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
 0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x,n) (((x)>>(n))|((x)<<(32-(n))))
static void blocks_c(uint32_t h[8],const uint8_t *p,size_t n){
 for(;n;n--,p+=64){uint32_t w[64],a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],k=h[7];
  for(int i=0;i<16;i++)w[i]=(uint32_t)p[4*i]<<24|(uint32_t)p[4*i+1]<<16|(uint32_t)p[4*i+2]<<8|p[4*i+3];
  for(int i=16;i<64;i++){uint32_t s0=ROR(w[i-15],7)^ROR(w[i-15],18)^(w[i-15]>>3),s1=ROR(w[i-2],17)^ROR(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
  for(int i=0;i<64;i++){uint32_t t1=k+(ROR(e,6)^ROR(e,11)^ROR(e,25))+((e&f)^(~e&g))+K[i]+w[i],t2=(ROR(a,2)^ROR(a,13)^ROR(a,22))+((a&b)^(a&c)^(b&c));
   k=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;}
  h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=k;}
}
#if defined(__aarch64__)
__attribute__((target("sha2")))
static void blocks_arm(uint32_t h[8],const uint8_t *p,size_t n){
 uint32x4_t s0=vld1q_u32(h),s1=vld1q_u32(h+4);
 for(;n;n--,p+=64){uint32x4_t a0=s0,a1=s1,m[4];
  for(int i=0;i<4;i++)m[i]=vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(p+16*i)));
  for(int i=0;i<16;i++){uint32x4_t wk=vaddq_u32(m[i&3],vld1q_u32(K+4*i)),t=s0;
   if(i<12)m[i&3]=vsha256su1q_u32(vsha256su0q_u32(m[i&3],m[(i+1)&3]),m[(i+2)&3],m[(i+3)&3]);
   s0=vsha256hq_u32(s0,s1,wk);s1=vsha256h2q_u32(s1,t,wk);}
  s0=vaddq_u32(s0,a0);s1=vaddq_u32(s1,a1);}
 vst1q_u32(h,s0);vst1q_u32(h+4,s1);
}
static int have_arm(void){static int v=-1;if(v<0)v=(getauxval(AT_HWCAP)&HWCAP_SHA2)!=0;return v;}
static void blocks(uint32_t h[8],const uint8_t *p,size_t n){if(have_arm())blocks_arm(h,p,n);else blocks_c(h,p,n);}
#else
static void blocks(uint32_t h[8],const uint8_t *p,size_t n){blocks_c(h,p,n);}
#endif

void sha256_init(Sha256 *s){static const uint32_t iv[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
 memcpy(s->h,iv,sizeof iv);s->bytes=0;s->used=0;}
void sha256_update(Sha256 *s,const void *data,size_t n){const uint8_t *p=data;s->bytes+=n;
 if(s->used){size_t k=64-s->used<n?64-s->used:n;memcpy(s->buf+s->used,p,k);s->used+=k;p+=k;n-=k;if(s->used<64)return;blocks(s->h,s->buf,1);s->used=0;}
 if(n>=64){blocks(s->h,p,n/64);p+=n/64*64;n%=64;}
 if(n){memcpy(s->buf,p,n);s->used=n;}}
void sha256_final(Sha256 *s,uint8_t out[32]){uint64_t bits=s->bytes*8;uint8_t pad[72]={0x80};size_t k=(s->used<56?56:120)-s->used;
 for(int i=0;i<8;i++){pad[k+i]=(uint8_t)(bits>>(56-8*i));}
 sha256_update(s,pad,k+8);
 for(int i=0;i<8;i++){out[4*i]=(uint8_t)(s->h[i]>>24);out[4*i+1]=(uint8_t)(s->h[i]>>16);out[4*i+2]=(uint8_t)(s->h[i]>>8);out[4*i+3]=(uint8_t)s->h[i];}}
void sha256_hex(const uint8_t d[32],char out[65]){static const char x[]="0123456789abcdef";for(int i=0;i<32;i++){out[2*i]=x[d[i]>>4];out[2*i+1]=x[d[i]&15];}out[64]=0;}
void sha256_buf_hex(const void *p,size_t n,char out[65]){Sha256 s;uint8_t d[32];sha256_init(&s);sha256_update(&s,p,n);sha256_final(&s,d);sha256_hex(d,out);}
