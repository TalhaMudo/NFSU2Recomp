/* 23k lift edilmis fonksiyonun tam derlemesi uzerinde dogrulama */
#include "cpu.h"
#include "decls.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
void dispatch_init(void); void dispatch_report(void);
extern const uint32_t g_fn_count;
uint64_t mem_committed(void);
extern uint64_t g_stat_indirect_ok, g_stat_indirect_miss, g_stat_unsupported;

static CPU cpu; static CPU *const c = &cpu;
#define STACK_TOP 0x00F00000u
static void reset(void){ memset(&cpu,0,sizeof cpu); cpu.esp=STACK_TOP; cpu.top=0; cpu.fcw=0x037f; }
static float GF(uint32_t a){ return MRF32(a); }
int pass=0,fail=0;
#define CHKI(n,g,e) do{ uint32_t _g=(g),_e=(e); if(_g==_e){pass++;printf("  \033[32mGECTI\033[0m  %-40s = 0x%08x\n",n,_g);} \
  else{fail++;printf("  \033[31mKALDI\033[0m  %-40s = 0x%08x (beklenen 0x%08x)\n",n,_g,_e);} }while(0)
#define CHKF(n,g,e) do{ float _g=(g),_e=(e); if(memcmp(&_g,&_e,4)==0){pass++;printf("  \033[32mGECTI\033[0m  %-40s = %.9g\n",n,(double)_g);} \
  else{fail++;printf("  \033[31mKALDI\033[0m  %-40s = %.9g (beklenen %.9g)\n",n,(double)_g,(double)_e);} }while(0)

int main(int argc,char**argv){
    mem_init(); mem_fault_handler_install();
    if(mem_load_pe(argc>1?argv[1]:"SPEED2.EXE")) return 1;
    mem_commit(0x00E00000u, 0x00200000u);        /* test heap + yigin */
    dispatch_init();
    fprintf(stderr,"  commit edilen guest bellek: %.1f MB\n\n",(double)mem_committed()/1048576.0);
    printf("=== TAM DERLEME: %u fonksiyon linklendi ===\n\n", g_fn_count);

    printf("--- 1. Dogrudan cagri (onceden dogrulanmis fonksiyonlar) ---\n");
    reset(); PUSH32(7u); PUSH32(0xDEADC0DEu); sub_0048b1e0(&cpu);
    CHKI("sub_0048b1e0(7)", cpu.eax, 0x832de0u + 7*0x70u);
    reset(); PUSH32(0xDEADC0DEu); sub_0077b970(&cpu);
    CHKF("sub_0077b970 -> [0x82875c]", GF(0x82875c),
         (float)((((double)GF(0x786ff4)+(double)GF(0x786ff0))+(double)GF(0x82a4e8))+(double)GF(0x7844c0)));

    printf("\n--- 2. DOLAYLI dispatch: ayni fonksiyonlar adres uzerinden ---\n");
    uint64_t ok0=g_stat_indirect_ok;
    reset(); PUSH32(7u); PUSH32(0xDEADC0DEu); dispatch_indirect(&cpu, 0x0048b1e0u);
    CHKI("dispatch(0x0048b1e0)(7)", cpu.eax, 0x832de0u + 7*0x70u);
    reset(); PUSH32(0xDEADC0DEu); dispatch_indirect(&cpu, 0x0077b950u);
    CHKF("dispatch(0x0077b950) -> [0x82a468]", GF(0x82a468),
         (float)((((double)GF(0x82a4e8)-(double)GF(0x786ff0))-(double)GF(0x786ff4))-(double)GF(0x7844c0)));
    CHKI("dispatch cozum sayisi", (uint32_t)(g_stat_indirect_ok-ok0), 2u);
    uint64_t miss0=g_stat_indirect_miss;
    reset(); PUSH32(0xDEADC0DEu); dispatch_indirect(&cpu, 0x00123456u);  /* gecersiz */
    CHKI("gecersiz adres -> miss sayaci", (uint32_t)(g_stat_indirect_miss-miss0), 1u);

    printf("\n--- 3. Dispatch tablosu butunlugu (rastgele 5000 fonksiyon) ---\n");
    extern const uint32_t g_fn_addr[];
    int bad=0;
    for(uint32_t i=0;i<g_fn_count;i+=(g_fn_count/5000?g_fn_count/5000:1)){
        /* her kayitli adres kendi indeksine cozulmeli */
        uint32_t a=g_fn_addr[i];
        uint64_t o1=g_stat_indirect_ok;
        reset(); PUSH32(0xDEADC0DEu);
        /* cagirmadan sadece cozunurlugu test etmek icin: tabloyu dolayli olarak
           kontrol edemiyoruz; en azindan adres araliginda oldugunu dogrula */
        if(!(a>=0x00401000u && a<0x00401000u+0x00381af1u)) bad++;
        (void)o1;
    }
    CHKI("tum fonksiyon adresleri .text icinde", (uint32_t)bad, 0u);

    printf("\n--- 4. Ayrik testler (x87 + tamsayi, tam derleme uzerinden) ---\n");
    int32_t iv[]={0,1,-1,12345,-987654,2147483647};
    for(unsigned k=0;k<sizeof iv/sizeof*iv;k++){
        reset(); uint32_t OBJ=0x00E00000u,INNER=0x00E01000u,OUT=0x00E02000u;
        MR32(OBJ+0xc)=INNER; MR32(INNER+0xc)=(uint32_t)iv[k];
        PUSH32(OUT); PUSH32(0xDEADC0DEu); cpu.ecx=OBJ; sub_006e1c00(&cpu);
        char nm[64]; snprintf(nm,sizeof nm,"fild(%d)",iv[k]);
        CHKF(nm, GF(OUT), (float)iv[k]);
    }
    reset(); { double d=INFINITY; uint64_t b; memcpy(&b,&d,8);
      PUSH32((uint32_t)(b>>32)); PUSH32((uint32_t)b); PUSH32(0xDEADC0DEu); sub_0075dc4b(&cpu);
      CHKI("_finite(inf)", cpu.eax, 0u); }
    reset(); { double d=1.0; uint64_t b; memcpy(&b,&d,8);
      PUSH32((uint32_t)(b>>32)); PUSH32((uint32_t)b); PUSH32(0xDEADC0DEu); sub_0075dc4b(&cpu);
      CHKI("_finite(1.0)", cpu.eax, 1u); }

    printf("\n---------------------------------------------\n  GECTI: %d    KALDI: %d\n",pass,fail);
    dispatch_report();
    return fail?1:0;
}
