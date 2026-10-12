#include "cpu.h"
#include <stdio.h>
#include <math.h>
#include <string.h>
void sub_0077b970(CPU*); void sub_0077b950(CPU*); void sub_006e1c00(CPU*); void sub_0040ffd0(CPU*);
void mem_commit(uint32_t,uint32_t);
void dispatch_indirect(CPU *c,uint32_t t){ (void)c;fprintf(stderr,"dolayli 0x%x\n",t); }
void cpuid_shim(CPU*c){ (void)c; }
uint64_t rdtsc_shim(void){ return 0; }

static CPU cpu; static CPU *const c = &cpu;
#define STACK_TOP 0x00F00000u
static void reset(void){ memset(&cpu,0,sizeof cpu); cpu.esp=STACK_TOP; cpu.top=0; cpu.fcw=0x037f; }
static float GF(uint32_t a){ return MRF32(a); }
int pass=0,fail=0;
#define CHK(n,g,e) do{ float _g=(g),_e=(e); if(memcmp(&_g,&_e,4)==0){pass++;printf("  \033[32mGECTI\033[0m  %-38s = %.9g\n",n,(double)_g);} \
  else{fail++;printf("  \033[31mKALDI\033[0m  %-38s = %.9g (beklenen %.9g, fark %g)\n",n,(double)_g,(double)_e,(double)(_g-_e));} }while(0)
#define CHKI(n,g,e) do{ uint32_t _g=(g),_e=(e); if(_g==_e){pass++;printf("  \033[32mGECTI\033[0m  %-38s = 0x%08x\n",n,_g);} \
  else{fail++;printf("  \033[31mKALDI\033[0m  %-38s = 0x%08x (beklenen 0x%08x)\n",n,_g,_e);} }while(0)

int main(int argc,char**argv){
    mem_init();
    if(mem_load_pe(argc>1?argv[1]:"SPEED2.EXE")) return 1;
    mem_commit(0x00000000u, 0x01000000u);   /* test alani + yigin (dusuk adresler dahil) */
    mem_fault_handler_install();
    fprintf(stderr,"\n");

    printf("=== x87 TEST 1: sub_0077b970 (fld + 3x fadd + fstp, global sabitler) ===\n");
    printf("  girdi sabitleri: [786ff4]=%.9g [786ff0]=%.9g [82a4e8]=%.9g [7844c0]=%.9g\n",
        (double)GF(0x786ff4),(double)GF(0x786ff0),(double)GF(0x82a4e8),(double)GF(0x7844c0));
    float exp1 = (float)((((double)GF(0x786ff4) + (double)GF(0x786ff0)) + (double)GF(0x82a4e8)) + (double)GF(0x7844c0));
    reset(); PUSH32(0xDEADC0DEu); int top0=cpu.top; sub_0077b970(&cpu);
    CHK("[0x82875c] sonucu", GF(0x82875c), exp1);
    CHKI("x87 yigin dengesi (top)", (uint32_t)cpu.top, (uint32_t)top0);

    printf("\n=== x87 TEST 2: sub_0077b950 (fld + 3x fsub + fstp) ===\n");
    float exp2 = (float)((((double)GF(0x82a4e8) - (double)GF(0x786ff0)) - (double)GF(0x786ff4)) - (double)GF(0x7844c0));
    reset(); PUSH32(0xDEADC0DEu); top0=cpu.top; sub_0077b950(&cpu);
    CHK("[0x82a468] sonucu", GF(0x82a468), exp2);
    CHKI("x87 yigin dengesi (top)", (uint32_t)cpu.top, (uint32_t)top0);

    printf("\n=== x87 TEST 3: sub_006e1c00 (fild: int -> float, thiscall) ===\n");
    /* mov eax,[ecx+0xc]; fild dword[eax+0xc]; mov ecx,[esp+4]; mov al,1; fstp dword[ecx]; ret 4 */
    int32_t vals[] = {0, 1, -1, 12345, -987654, 2147483647, -2147483648};
    for(unsigned k=0;k<sizeof vals/sizeof*vals;k++){
        reset();
        uint32_t OBJ=0x00E00000u, INNER=0x00E01000u, OUT=0x00E02000u;
        MR32(OBJ+0xc)=INNER; MR32(INNER+0xc)=(uint32_t)vals[k];
        PUSH32(OUT); PUSH32(0xDEADC0DEu); cpu.ecx=OBJ;
        sub_006e1c00(&cpu);
        char nm[64]; snprintf(nm,sizeof nm,"fild(%d)",vals[k]);
        CHK(nm, GF(OUT), (float)vals[k]);
    }
    reset(); { uint32_t OBJ=0x00E00000u,INNER=0x00E01000u,OUT=0x00E02000u;
      MR32(OBJ+0xc)=INNER; MR32(INNER+0xc)=42; PUSH32(OUT); PUSH32(0xDEADC0DEu); cpu.ecx=OBJ;
      int t0=cpu.top; sub_006e1c00(&cpu);
      CHKI("x87 yigin dengesi (top)",(uint32_t)cpu.top,(uint32_t)t0);
      CHKI("al=1 dondu", cpu.eax & 0xffu, 1u);
      CHKI("ret 4 sonrasi esp", cpu.esp, STACK_TOP-4u+4u); }

    printf("\n=== x87 TEST 4: sub_0040ffd0 (fld/fstp float kopyalama, thiscall) ===\n");
    float src[]={0.0f,1.5f,-273.15f,3.14159265f,1e-30f,1e30f,INFINITY,NAN};
    for(unsigned k=0;k<sizeof src/sizeof*src;k++){
        reset(); uint32_t ARG=0x00E00000u, THIS=0x00E10000u;
        MR32(ARG)=0xAABBCCDDu; MRF32(ARG+4)=src[k];
        PUSH32(ARG); PUSH32(0xDEADC0DEu); cpu.ecx=THIS;
        sub_0040ffd0(&cpu);
        char nm[64]; snprintf(nm,sizeof nm,"float kopya %.9g",(double)src[k]);
        float got=GF(THIS+0x228), want=src[k];
        if(isnan(want)){ if(isnan(got)){pass++;printf("  \033[32mGECTI\033[0m  %-38s = nan\n",nm);} else {fail++;printf("  \033[31mKALDI\033[0m  %s\n",nm);} }
        else CHK(nm, got, want);
        CHKI("  yanindaki int alani", MR32(THIS+0x224), 0xAABBCCDDu);
    }
    printf("\n---------------------------------------------\n  GECTI: %d    KALDI: %d\n",pass,fail);
    return fail?1:0;
}
