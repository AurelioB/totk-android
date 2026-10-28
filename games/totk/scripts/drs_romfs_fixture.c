/* Host-only fixture: exercises the production overlay initializer and every
   RomFS open/read/size path, without a guest process or game executable. The
   Python driver writes its tiny image/index/config only into external scratch. */
#include "../runtime/romfs.c"

_Noreturn void runtime_fail(const char *message) {fprintf(stderr,"FAIL: %s\n",message);exit(7);}
void runtime_boot_progress(const char *stage,uint64_t done,uint64_t total) {(void)stage;(void)done;(void)total;}
void runtime_boot_status(const char *stage,const char *detail) {(void)stage;(void)detail;}
uint64_t runtime_now_ns(void) {return 1;}
uint64_t runtime_output_res_extra_bytes(void) {return 0;}
int runtime_output_res(unsigned *w,unsigned *h) {(void)w;(void)h;return 0;}
int runtime_render_res(unsigned *w,unsigned *h) {(void)w;(void)h;return 0;}
void dm_guest_write_prepare(void *p,size_t n) {(void)p;(void)n;}
void *runtime_fs_host_override(const char *name) {(void)name;return NULL;}

static uint8_t *read_file(const char *path,size_t *size) {
 FILE *f=fopen(path,"rb");need(f!=NULL,"fixture input open");need(!fseek(f,0,SEEK_END),"fixture seek");
 long n=ftell(f);need(n>=0,"fixture input size");*size=(size_t)n;rewind(f);
 uint8_t *data=malloc(*size?*size:1);need(data!=NULL,"fixture allocation");need(fread(data,1,*size,f)==*size,"fixture input read");fclose(f);return data;
}
static void write_file(const char *path,const uint8_t *data,size_t size) {
 FILE *f=fopen(path,"wb");need(f!=NULL,"fixture output open");need(fwrite(data,1,size,f)==size,"fixture output write");need(!fclose(f),"fixture output close");
}
static void check_plain(const char *expected) {
 uint64_t h,n;int64_t size;uint8_t data[64];size_t want=strlen(expected);
 need(!runtime_romfs_open(&h,"rom:/Other/plain.bin",1),"unrelated overlay open");need(!runtime_romfs_size(&size,h)&&size==(int64_t)want,"unrelated overlay size");
 need(!runtime_romfs_read_options(&n,h,0,data,sizeof(data))&&n==want&&!memcmp(data,expected,want),"unrelated overlay read");runtime_romfs_close(h);
}
int main(int argc,char **argv) {
 if(argc==3&&!strcmp(argv[1],"scale")){uint32_t word;if(!totk_drs_scale(argv[2],&word))return 3;printf("%08x\n",word);return 0;}
 if(argc==4&&!strcmp(argv[1],"frame")){
  size_t size;uint8_t *data=read_file(argv[2],&size);size_t n=totk_drs_frame_size(size);uint8_t *out=malloc(n);need(out&&totk_drs_frame(data,size,out,n),"raw frame");write_file(argv[3],out,n);free(out);free(data);return 0;
 }
 if(argc!=5||strcmp(argv[1],"serve"))return 2;
 /* romfs.c reads these from romfs-config.txt in runtime_fs_init, which this fixture bypasses: the driver passes them. */
 romfs_image_size=FIXTURE_IMAGE_SIZE;romfs_image_sha256=romfs_index_sha256="0000000000000000000000000000000000000000000000000000000000000000";
 const char *root=argv[2];char path[4096];
 need(snprintf(path,sizeof(path),"%s/runtime-index.bin",root)<(int)sizeof(path),"fixture index path");index_bytes=read_file(path,&index_size);
 need(index_size>=24&&!memcmp(index_bytes,"TOTKRFS1",8),"fixture index");memcpy(&file_count,index_bytes+8,8);memcpy(&strings_offset,index_bytes+16,8);
 need(snprintf(path,sizeof(path),"%s/romfs.bin",root)<(int)sizeof(path),"fixture image path");image_fd=open(path,O_RDONLY);if(image_fd<0)open_chunks(root);
 if(getenv("TOTK_ROMFS_MMAP")){struct stat st;need(!fstat(image_fd,&st),"fixture mmap stat");image_map=mmap(NULL,(size_t)st.st_size,PROT_READ,MAP_PRIVATE,image_fd,0);need(image_map!=MAP_FAILED,"fixture mmap");}
 strcpy(mount_name,"rom");overlay_init(root);
 check_plain(argv[4]);
 uint64_t h,n;int64_t size;int type;
 need(!runtime_romfs_entry_type(&type,"rom:/" TOTK_DRS_PACK)&&type==1,"Bootup entry type");
 need(!runtime_romfs_open(&h,"rom://" TOTK_DRS_PACK,1),"Bootup open");
 need(!runtime_romfs_size(&size,h)&&size>0,"Bootup size");uint8_t *data=malloc((size_t)size);need(data!=NULL,"Bootup read buffer");
 size_t done=0;while(done<(size_t)size){size_t chunk=(done%4093)+1;if(chunk>(size_t)size-done)chunk=(size_t)size-done;
  need(!runtime_romfs_read_options(&n,h,(int64_t)done,data+done,chunk)&&n==chunk,"Bootup partial read");done+=chunk;}
 need(!runtime_romfs_read(&n,h,size,NULL,0)&&n==0,"Bootup EOF read");
 need(runtime_romfs_read(&n,h,size+1,NULL,0)==INVALID,"Bootup beyond EOF");runtime_romfs_close(h);
 need(!runtime_romfs_open(&h,"rom:/" TOTK_DRS_PACK,1),"Bootup reopen");uint8_t first[257];
 need(!runtime_romfs_read(&n,h,0,first,sizeof(first))&&n==sizeof(first)&&!memcmp(first,data,sizeof(first)),"Bootup reopen data");runtime_romfs_close(h);
 write_file(argv[3],data,(size_t)size);free(data);return 0;
}
