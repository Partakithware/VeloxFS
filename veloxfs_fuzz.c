/* veloxfs_fuzz.c -- randomized model-checking test for veloxfs (bare-metal header)
 *
 * Runs random create / append / overwrite / write_file / truncate (shrink+grow) /
 * write-past-EOF / rename / delete against an in-memory model of the filesystem,
 * verifies every file via both read_file and streaming read, runs fsck, and
 * unmount/remounts every 1000 ops to check on-disk persistence.
 *
 *   gcc -g -O1 -fsanitize=address,undefined -DHDR='"veloxfs_bm_patched.h"' veloxfs_fuzz.c -o fuzz
 *   ./fuzz <seed> <ops>                       # default: 400-block volume, 4 KB blocks
 *   -DNB=64 -DMAXB=70                         # tiny volume -> forces ENOSPC paths
 *   -DBS=65536 -DNB=200 -DMAXB=30             # different block size
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int g_log=0;
static void logcap(const char*s){ if(strstr(s,"clean")) return; if(g_log<6) printf("   [lib] %s",s); g_log++; }
#define veloxfs_LOG(...) do{ char _b[256]; snprintf(_b,sizeof _b,__VA_ARGS__); logcap(_b);}while(0)
#define veloxfs_IMPLEMENTATION
#include HDR
#ifndef BS
#define BS 4096
#endif
#ifndef NB
#define NB 400
#endif
#ifndef MAXB
#define MAXB 40
#endif
#define NF 8
static uint8_t *mem;
static int rd(void*u,uint64_t o,void*b,uint32_t n){ if(o+n>(uint64_t)NB*BS){printf("OOB read\n");abort();} memcpy(b,mem+o,n);return 0;}
static int wr(void*u,uint64_t o,const void*b,uint32_t n){ if(o+n>(uint64_t)NB*BS){printf("OOB write\n");abort();} memcpy(mem+o,b,n);return 0;}
typedef struct { int used; uint8_t *d; uint64_t size; } mf;
static mf M[NF];
static uint64_t rs=88172645463325252ULL;
static uint64_t rnd(void){ rs^=rs<<13; rs^=rs>>7; rs^=rs<<17; return rs; }
static void path(int i,char*o){ sprintf(o,"/f%d",i); }
static veloxfs_handle fs;
static long fails=0; static int verbose=1;
#define FAIL(...) do{ fails++; if(fails<=12){ printf("  FAIL @op %ld: ",opn); printf(__VA_ARGS__); printf("\n"); } }while(0)
static long opn=0;
static void verify_file(int i,const char*why){
  char p[32]; path(i,p); veloxfs_stat_t st; int r=veloxfs_stat(&fs,p,&st);
  if(!M[i].used){ if(r==0) FAIL("%s: /f%d should not exist",why,i); return; }
  if(r){ FAIL("%s: /f%d missing (r=%d)",why,i,r); return; }
  if(st.size!=M[i].size){ FAIL("%s: /f%d size %llu != model %llu",why,i,(unsigned long long)st.size,(unsigned long long)M[i].size); return; }
  uint8_t *buf=malloc(M[i].size+1); uint64_t got=0;
  r=veloxfs_read_file(&fs,p,buf,M[i].size,&got);
  if(r||got!=M[i].size||memcmp(buf,M[i].d,M[i].size)){
     uint64_t k=0; while(k<M[i].size&&buf[k]==M[i].d[k])k++;
     FAIL("%s: /f%d read_file mismatch (r=%d got=%llu first diff @%llu)",why,i,r,(unsigned long long)got,(unsigned long long)k); }
  veloxfs_file f; memset(buf,0xEE,M[i].size+1);
  if(veloxfs_open(&fs,p,veloxfs_O_RDONLY,&f)){FAIL("open");free(buf);return;}
  uint64_t tot=0; while(tot<M[i].size){ uint64_t br=0; uint64_t want=1+rnd()%(3*BS); if(want>M[i].size-tot)want=M[i].size-tot;
     r=veloxfs_read(&f,buf+tot,want,&br); if(r||br==0){break;} tot+=br; }
  if(tot!=M[i].size||memcmp(buf,M[i].d,M[i].size)){
     uint64_t k=0; while(k<M[i].size&&k<tot&&buf[k]==M[i].d[k])k++;
     FAIL("%s: /f%d streaming read mismatch (got %llu/%llu, first diff @%llu)",why,i,(unsigned long long)tot,(unsigned long long)M[i].size,(unsigned long long)k);}
  veloxfs_close(&f); free(buf);
}
static void verify_all(const char*why){ for(int i=0;i<NF;i++) verify_file(i,why); }
static void mkdata(uint8_t*p,uint64_t n){ for(uint64_t i=0;i<n;i++) p[i]=(uint8_t)rnd(); }
static void remount(void){ veloxfs_unmount(&fs); if(veloxfs_mount(&fs,(veloxfs_io){rd,wr,NULL})){ printf("REMOUNT FAILED\n"); exit(2);} }
int main(int argc,char**argv){
  uint64_t seed=argc>1?strtoull(argv[1],0,10):1; long N=argc>2?atol(argv[2]):20000; rs^=seed*0x9E3779B97F4A7C15ULL; for(int i=0;i<10;i++)rnd();
  mem=calloc(NB,BS); veloxfs_io io={rd,wr,NULL};
  if(veloxfs_format_ex(io,NB,0,BS,0)){puts("format fail");return 2;}
  if(veloxfs_mount(&fs,io)){puts("mount fail");return 2;}
  long nospc=0, maxchain=0, ops_ok=0;
  for(opn=0;opn<N;opn++){
    int i=rnd()%NF; char p[32]; path(i,p); int op=rnd()%100; uint64_t maxs=(uint64_t)MAXB*BS;
    if(!M[i].used){ if(op<70){ if(veloxfs_create(&fs,p,0644)==0){M[i].used=1;M[i].size=0;M[i].d=malloc(maxs+3*BS);} } continue; }
    if(op<45){ /* append */
      uint64_t n=1+rnd()%(2*BS); if(rnd()%4==0)n=BS; if(M[i].size+n>maxs)continue;
      uint8_t*b=malloc(n); mkdata(b,n); veloxfs_file f; veloxfs_open(&fs,p,veloxfs_O_RDWR,&f); veloxfs_seek(&f,0,2);
      int r=veloxfs_write(&f,b,n); veloxfs_close(&f);
      if(r==0){ memcpy(M[i].d+M[i].size,b,n); M[i].size+=n; ops_ok++; } else if(r==veloxfs_ERR_NO_SPACE) nospc++; else FAIL("append r=%d",r);
      free(b);
    } else if(op<60){ /* overwrite at pos<=size */
      uint64_t pos=M[i].size? rnd()%(M[i].size+1):0; uint64_t n=1+rnd()%(2*BS); if(pos+n>maxs)continue;
      uint8_t*b=malloc(n); mkdata(b,n); veloxfs_file f; veloxfs_open(&fs,p,veloxfs_O_RDWR,&f); veloxfs_seek(&f,pos,0);
      int r=veloxfs_write(&f,b,n); veloxfs_close(&f);
      if(r==0){ memcpy(M[i].d+pos,b,n); if(pos+n>M[i].size)M[i].size=pos+n; ops_ok++; } else if(r==veloxfs_ERR_NO_SPACE) nospc++; else FAIL("overwrite r=%d",r);
      free(b);
    } else if(op<68){ /* whole-file write */
      uint64_t n=rnd()%(8*BS); uint8_t*b=malloc(n+1); mkdata(b,n);
      uint64_t old=M[i].size; int r=veloxfs_write_file(&fs,p,b,n);
      if(r==0){ memcpy(M[i].d,b,n); M[i].size=n; ops_ok++; } else if(r==veloxfs_ERR_NO_SPACE){ nospc++; (void)old; } else FAIL("write_file r=%d",r);
      free(b);
    } else if(op<78){ /* truncate shrink */
      if(!M[i].size)continue; uint64_t ns=rnd()%(M[i].size); veloxfs_file f; veloxfs_open(&fs,p,veloxfs_O_RDWR,&f);
      int r=veloxfs_truncate_handle(&f,ns); veloxfs_close(&f); if(r==0){M[i].size=ns;ops_ok++;} else FAIL("truncate r=%d",r);
    } else if(op<84){ /* delete */
      if(veloxfs_delete(&fs,p)==0){ M[i].used=0; free(M[i].d); M[i].d=NULL; M[i].size=0; } else FAIL("delete");
    } else if(op<88){ /* rename */
      int j=rnd()%NF; if(M[j].used||j==i)continue; char q[32]; path(j,q);
      if(veloxfs_rename(&fs,p,q)==0){ M[j]=M[i]; M[i].used=0; M[i].d=NULL; M[i].size=0; } else FAIL("rename");
    } else if(op<92){ /* grow via truncate: new bytes must be zero */
      if(M[i].size>=maxs-BS)continue; uint64_t ns=M[i].size+1+rnd()%(3*BS); if(ns>maxs)continue;
      veloxfs_file f; veloxfs_open(&fs,p,veloxfs_O_RDWR,&f); int r=veloxfs_truncate_handle(&f,ns); veloxfs_close(&f);
      if(r==0){ memset(M[i].d+M[i].size,0,ns-M[i].size); M[i].size=ns; ops_ok++; } else if(r==veloxfs_ERR_NO_SPACE) nospc++; else FAIL("grow r=%d",r);
    } else if(op<95){ /* write starting past EOF: gap must be zero */
      uint64_t gap=1+rnd()%(2*BS); uint64_t pos=M[i].size+gap; uint64_t n=1+rnd()%BS; if(pos+n>maxs)continue;
      uint8_t*b=malloc(n); mkdata(b,n); veloxfs_file f; veloxfs_open(&fs,p,veloxfs_O_RDWR,&f); veloxfs_seek(&f,pos,0);
      int r=veloxfs_write(&f,b,n); veloxfs_close(&f);
      if(r==0){ memset(M[i].d+M[i].size,0,gap); memcpy(M[i].d+pos,b,n); M[i].size=pos+n; ops_ok++; } else if(r==veloxfs_ERR_NO_SPACE) nospc++; else FAIL("gapwrite r=%d",r);
      free(b);
    } else { verify_file(i,"spot"); }
    { static int lastop; for(int k=0;k<NF;k++){ char pp[32]; path(k,pp); veloxfs_stat_t st2; int rr=veloxfs_stat(&fs,pp,&st2); if(M[k].used && rr==-3){ FAIL("LOST /f%d right after op class %d on /f%d (op value %d)",k,op,i,op); } } (void)lastop; }
    if(opn%250==249){ verify_all("periodic"); }
    if(opn%1000==999){ g_log=0; int fr=veloxfs_fsck(&fs); if(fr||g_log) FAIL("fsck r=%d logs=%d",fr,g_log); remount(); verify_all("after remount"); }
    { veloxfs_alloc_stats s; veloxfs_alloc_stats_get(&fs,&s); if((long)s.fat_overflow>maxchain)maxchain=s.fat_overflow; }
    if(fails>12) break;
  }
  verify_all("final"); g_log=0; int fr=veloxfs_fsck(&fs); if(fr||g_log) FAIL("final fsck r=%d logs=%d",fr,g_log);
  printf("seed %llu: ops_ok=%ld nospace=%ld max_files_with_overflow=%ld FAILS=%ld\n",(unsigned long long)seed,ops_ok,nospc,maxchain,fails);
  return fails?1:0;
}
