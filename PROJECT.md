# NFSU2 → macOS ARM64 Native Port (statik rekompilasyon)

`SPEED2.EXE` (32-bit x86 Windows PE) makine kodunu C'ye lift edip **native ARM64
Mach-O** olarak yeniden derleme projesi. Wine yok, emülasyon yok, Windows yok.

Yöntem: N64Recomp (Zelda MM) ve XenonRecomp (Sonic Unleashed) portlarındaki
statik rekompilasyon tekniği — x86 için uyarlanmış.

> **Bu belge kronolojik bir mühendislik günlüğüdür.** Güncel özet, derleme
> tarifi ve ilgili çalışmalar `README.md`'de. Eski bölümlerdeki sayılar (test
> adedi, trap sayısı, kaynak boyutu) yazıldıkları günkü durumu gösterir.
> Güncel değerler (2026-10-12): 23.278 fonksiyon, 132,5 MB üretilmiş C,
> 400 statik trap, 169 ISA testi / 439 denetim.

## Hedef binary

| | |
|---|---|
| SHA-256 | `8f6a9ae2c4cfa01181d2fd755d99c6f4b7103f60273908ea1de1e719dfe2a297` |
| Biçim | PE32, Intel 80386, GUI, ImageBase `0x400000` |
| Entry | `0x0075bcc7` |
| `.text` | 3.676.913 bayt (~1,13 M komut, 337 farklı opcode) |
| Paketli/korumalı | **Hayır** — import tablosu tam ve okunabilir |

### Kritik keşif: relocation tablosu sağlam
PE'nin `BASERELOC` dizin girişi sıfırlanmış ve `RELOCS_STRIPPED` bayrağı set
(no-CD exe'lerde tipik), **ama veri fiziksel olarak duruyor** — isimsiz 5. bölümde
(`0x8f5000`, 240.664 bayt). Elle ayrıştırıldı:

* 999 blok, 91.616 giriş, **91.106 HIGHLOW mutlak adres**
* Doğrulama: **91.106/91.106'sı geçerli image adresine işaret ediyor (%100)**
* Dağılım: `.text` 71.094 · `.rdata` 14.601 · `.data` 5.411

Bu, x86 statik rekompilasyonun en büyük problemini çözüyor: *hangi 4 bayt
işaretçi, hangisi sayı?* RISC ISA'larda (MIPS/PPC) sabit 4 baytlık komutlar
sayesinde bu sorun yok; x86'nın değişken uzunluklu (1–15 bayt) komutlarında
kesin çözüm genel halde karar verilemez. Reloc tablosu bunu aşıyor.

## Ölçülen durum

| Metrik | Değer |
|---|---|
| `.text` kod kapsamı (reloc destekli recursive descent) | **%96,08** |
| Açıklanan toplam (kod + hizalama + jump table + veri) | **%98,71** |
| Gerçek belirsizlik | **%1,29** (47.451 bayt / 1.546 blok) |
| Bulunan fonksiyon | **23.274** |
| **Tam lift edilen fonksiyon** | **23.046 / 23.274 = %99,0** |
| **Komut bazında lift kapsamı** | **%98,5** |
| Desteklenmeyen komut trap'i (statik) | 914 (başlangıçta 30.783) |
| Dolaylı çağrı bölgesi (`call reg/[mem]`) | 6.279 |
| Dolaylı atlama | 763 (446 jump table çözüldü) |

### Komut karışımı
`mov` %27,6 · `push` %13,6 · `call` %6,1 · **x87 %9,86** · SSE/MMX %1,25

## Çalışma zamanı mimarisi

**Bellek:** guest 32-bit adres alanı `mmap` ile ayrılır; guest adres → host adres
çevirisi ARM64'ün `uxtw` adresleme kipiyle **tek komutta, sıfır ek maliyetle**:

```asm
ldr w8, [x_base, w_addr, uxtw]     ; MR32(guest_addr)
```

> Not: guest'i kendi adresine (`0x400000`) doğrudan haritalamak denendi —
> `__PAGEZERO` 0x4000'e küçültülse bile macOS ARM64 süreci SIGKILL ediyor.
> Taban+offset yaklaşımı zaten bedava olduğu için bu bir kayıp değil.

**CPU durumu:** `runtime/cpu.h` — 8 GPR, bayraklar ayrı `uint32_t` alanlarda
(clang'ın DCE'si fonksiyon içinde kullanılmayan bayrak hesaplarını eliyor),
8 elemanlı x87 yığını, 8 XMM.

## Dosyalar

```
tools/analyze.py    PE bölümleri, recursive descent, kapsam
tools/reloc2.py     sıfırlanmış reloc tablosunu elle ayrıştır  -> out/relocs.txt
tools/profile.py    komut histogramı, x87/SSE oranı
tools/lift.py       x86-32 -> C lifter (çekirdek)
tools/liftall.py    23k fonksiyonu dene, eksik komutları raporla
runtime/cpu.h       CPU durumu, bellek makroları, bayrak/x87/string yardımcıları
runtime/mem.c       PE bölümlerini guest belleğe yükler
tests/harness.c     tamsayı testleri (22 test)
tests/fptest.c      x87 testleri (30 test)
```

Kurulum: `python3 -m venv .venv && PIP_USER=0 .venv/bin/pip install capstone pefile`

```bash
./.venv/bin/python tools/lift.py <SPEED2.EXE> 48b1e0 631d30 > gen/funcs.c
clang -O2 -Iruntime -o out/test tests/harness.c gen/funcs.c runtime/mem.c -lm
```

## Doğrulanmış (52/52 test geçiyor, native arm64)

* `sub_0048b1e0` — dizi indeksleme, `imul`+`add`, 6 vaka
* `sub_0075dc4b` — `_finite(double)`; C `isfinite()`'e karşı **bit-birebir**
  (inf, -inf, nan, subnormal dahil 8 vaka)
* `sub_00631d30` — 6 bloklu CFG, koşullu dallanma, `ret 8` stdcall temizliği,
  alt fonksiyon çağrısı, yığın dengesi
* `sub_0077b970` / `sub_0077b950` — `fld` + 3× `fadd`/`fsub` + `fstp`,
  image'deki gerçek sabitlerle, float sonuç bit-birebir
* `sub_006e1c00` — `fild` int→float, `INT32_MIN`/`INT32_MAX` dahil 7 vaka
* `sub_0040ffd0` — float kopyalama, inf/nan roundtrip


## Aşama 2 sonuçları: dolaylı dispatch + tam derleme

### Dolaylı çağrı dispatch'i
`.text` boyutunda doğrudan indeks tablosu (14 MB `uint32`), guest adres → native
fonksiyon işaretçisi, **O(1)**. 23.278 fonksiyon kayıtlı, çakışma/aralık dışı yok.
Doğrulandı: aynı fonksiyon doğrudan çağrıldığında ve guest adresi üzerinden
dispatch edildiğinde **birebir aynı sonucu** veriyor. Boot çalıştırmasında
çözülemeyen dolaylı çağrı **0**.

### Tam derleme
| | |
|---|---|
| Üretilen fonksiyon | **23.278** |
| C kaynak | **115,6 MB** (128 shard) |
| Üretim süresi | 21 s (10 çekirdek) |
| Derleme | **130/130 shard, hatasız**, 66 s (571 CPU-s, `-O2`) |
| Link | 43 MB **Mach-O 64-bit arm64** |
| Kesilmiş dallanma hedefi | 0 |
| Eksik sembol (stub) | 4 |
| Desteklenmeyen komut trap'i | 30.783 (yalnız MMX/SSE/3DNow) |

Üretim sırasında bulunan ve düzeltilen üç lifter hatası:
1. `.text` dışına `call` → var olmayan `sub_XXXX` sembolü (artık dispatch'e gider)
2. `.text` dışına koşullu dallanma → var olmayan etiket (artık trap)
3. Kesinti nedeniyle üretilmeyen dallanma hedefleri → stub etiket

### fs:/gs: segment desteği (doğruluk düzeltmesi)
Statik tarama `fs:` önekinin **3.698 kez** kullanıldığını gösterdi (3.613'ü `mov`)
— SEH zinciri `fs:[0]` ve TLS `fs:[0x2c]`. Lifter bunları sessizce yok sayıyordu,
yani `mov eax, fs:[0]` yanlış çevriliyordu. Artık `c->fs_base` ekleniyor.
`es:/ds:/ss:` düz bellek modelinde taban 0 olduğu için yok sayılmaya devam ediyor
(4.097 `es:` erişiminin tamamı string komutlarının örtük segmenti).

### Guest bellek modeli
4 GB adres alanı `PROT_NONE` olarak rezerve edilir, kullanılan bölgeler commit
edilir. Böylece hatalı guest erişimi host'u çökertmek yerine
`SIGSEGV` → guest adresi olarak teşhis edilir. Boot sırasında yığın taşması tam
olarak `0x00cfffec`'te (yığın sınırının 20 bayt altı) yakalandı.

## Aşama 3 tohumu: Win32 katmanı ve boot keşfi

252 IAT girişi, 242'si çağrılıyor, **613 çağrı sitesi**:
KERNEL32 469 · USER32 62 · WS2_32 22 · ADVAPI32 17 · GDI32 14 · WINMM 11 · TAPI32 9

**stdcall argüman sayıları:** çağrı sitelerindeki `push` dizilerinden çıkarım
denendi — bilinen 183 fonksiyona karşı yalnız **%74,9 doğru**. Yanlış her sayı
`esp`'i kaydırıp yığını bozduğu için bu yeterli değil; 183 fonksiyon için
otoriter tablo elle girildi (`tools/argcount.py`), kalan ~59 çıkarımı kullanıyor.

**Boot durumu:** entry point `0x0075bcc7`'den başlayarak MSVC CRT başlangıcı
**73 Win32 çağrısı** derinliğe kadar doğru çalışıyor:

```
GetVersionExA → HeapCreate → 17× InitializeCriticalSection → TlsAlloc/TlsSetValue
→ GetStartupInfoA → stdio handle'lari → GetCommandLineA → ortam degiskenleri
→ GetACP/GetCPInfo → GetStringTypeW → MultiByteToWideChar → LCMapStringW
→ WideCharToMultiByte   (locale/codepage baslatmasi TAM)
```

Implemente edilenler: heap (bump ayırıcı), TLS, kritik bölümler, zaman
(`QueryPerformanceCounter`, `GetTickCount`), `GetStringTypeW` (C1_* bayrakları),
`LCMapStringW/A` (büyük/küçük harf), `GetLocaleInfoA/W`, `MultiByteToWideChar`,
`WideCharToMultiByte`, `GetProcAddress` (isimle çözümleme), `MessageBoxA`.

**Mevcut engel:** CRT `/GS` yığın güvenlik çerezi kontrolü başarısız oluyor ve
oyun kendi hata kutusunu gösteriyor:

> *Buffer overrun detected! — Microsoft Visual C++ Runtime Library*

Teşhis edilenler:
* Çerez global'i **guest `0x81a1d0`**, değeri `0xBB40E64E` (MSVC link-zamanı
  varsayılanı) — çalışma boyunca hiç değişmiyor, yani `__security_init_cookie`
  çalışmamış. Global sabit olduğu için tutarsızlık **yığın kopyasında**.
* `__security_check_cookie` `0x76115e`'de; `__report_gsfailure` `0x76112d`'de ve
  ona **tek bir referans** var (`0x761167`'deki `jmp`).
* Boot yolundaki 27 import'un **tamamı** otoriter argüman sayısına sahip, yani
  bozulmanın kaynağı yanlış stdcall temizliği **değil** — lift edilmiş kodun
  kendisinde.

### Yapısal bulgu: temel bloklar fonksiyonlar arasında kopyalanıyor
Bir fonksiyon başka bir fonksiyona `jmp` ile geçtiğinde (tail-call), lifter hedef
blokları çağıranın gövdesine kopyalıyor. `__security_check_cookie` blokları hem
`sub_0076115e` hem de `sub_00743cd0` içinde ayrı ayrı bulunuyor. Anlamsal olarak
doğru ama **sembol bazlı müdahale tüm yolları yakalayamıyor** — hata ayıklarken
ve ileride fonksiyon yamalarken bunu hesaba katmak gerek.

### Yeni araçlar
```
tools/genall.py     23k fonksiyonu paralel uret, shard'lara bol, fntab uret
tools/imports.py    IAT cikar, import basina cagri sitesi say
tools/argcount.py   stdcall arguman sayilari (otoriter tablo + cikarim)
tools/disasm.py     adres araligi sok
runtime/dispatch.c  O(1) dolayli cagri tablosu + guest yigin izleyici
runtime/win32.c     Win32 API katmani (boot alt kumesi)
build.sh            paralel shard derlemesi
tests/main_all.c    tam derleme dogrulama paketi (15 test)
tests/boot.c        entry point calistirma + izleme
```

```bash
./.venv/bin/python tools/genall.py <SPEED2.EXE> 128 && OPT=-O2 ./build.sh
```

### Sıradaki adımlar
1. `/GS` çerez bozulmasının kaynağını bul — lifter'ın bayrak/yığın semantiğinde
   bir hata olma ihtimali yüksek. Şüpheliler: `inc`/`dec` CF koruma hilesi,
   kaydırma bayrakları, `rep` string komutları, 16/8-bit kısmi kayıt yazmaları.
   Yöntem: guest yürütmesini adım adım izleyip yığın çerezinin hangi komutta
   değiştiğini bulan bir izleyici (watchpoint) yaz.
2. MMX/SSE/3DNow ekle → 30.783 trap'i sıfırla.
3. Kalan ~59 import için otoriter argüman sayısı.

## Yol haritası

**Aşama 1 — lifter tamamlama** *(tamamlandı)*
- [x] Tamsayı çekirdeği, bayraklar, CFG, `call`/`ret`
- [x] x87 FPU yığını + 80-bit `tbyte` dönüşümü
- [x] String komutları (`rep movs/stos/lods/scas/cmps`, `df` yönü)
- [x] MMX (paket tamsayı, doyma, kaydırma, punpck, pack, pcmpeq/gt, pmaddwd, pmulhw)
- [x] SSE/SSE2 (movaps/movups/movdqa, movss/movsd, add/sub/mul/div ps+ss+sd,
      shufps, unpck, sqrt/max/min, cvt*, ucomis*, movhlps/movlhps)
- [x] 3DNow! (pfadd/pfsub/pfmul/pfcmp*/pi2fd/pf2id)
- [x] `pushal`/`popal`, `rcl`/`rcr`, `bt` ailesi, `pushfd`/`popfd`, `sahf`/`lahf`
- [x] Segment kayıtları (düz modelde sabit selektör), ayrıcalıklı komutlar (etkisiz)
- [ ] Kalan %1,5: `pandn` bazı operand biçimleri, `packssdw`, `movq2dq` (9 fonksiyon)

**Aşama 2 — çalıştırılabilirlik**
- [x] Dolaylı çağrı dispatch tablosu (O(1), 23.278 fonksiyon, doğrulandı)
- [ ] SEH (MSVC `__try/__except`) analizi ve desteği
- [x] fs:/gs: segment tabanı (TEB/TLS)
- [ ] Thread'ler → pthreads
- [x] Tüm 23k fonksiyonu üret + derle + link (43 MB arm64 Mach-O, 66 s)

**Aşama 3 — Win32 katmanı** *(230 import)*
- [ ] KERNEL32 (152) — dosya G/Ç → POSIX, sanal bellek, thread, TLS, zaman
- [ ] USER32 (38) — pencere/mesaj döngüsü → Cocoa veya SDL3
- [ ] GDI32 (12), ADVAPI32 (6, registry → plist), WINMM (6)
- [ ] WS2_32 → BSD socket · SHELL32/SHFOLDER → yol eşleme
- [ ] TAPI32 (9) / NETAPI32 (1) → büyük ihtimalle stub

**Aşama 4 — COM/grafik** *(en büyük tek kalem)*
- [ ] **D3D9 → Metal**: `Direct3DCreate9` tek import, geri kalan vtable üzerinden.
      IDirect3DDevice9 + kaynak tipleri, Shader Model 1.1/2.0 → MSL çevirisi.
- [ ] DirectInput8 → GameController / IOKit
- [ ] DirectSound → CoreAudio
- [ ] DirectDraw (`DirectDrawCreate` tek çağrı, muhtemelen GPU tespiti)

**Aşama 5 — doğruluk**
- [ ] x87 80-bit ↔ ARM64 64-bit sapması: fizik/determinizm etkisi ölçülmeli
- [ ] Referans karşılaştırmalı test (aynı girdi → aynı çıktı)

## Dürüst kapsam değerlendirmesi

Aşama 1–2 mekanik ve öngörülebilir. **Aşama 4 projenin ağırlık merkezi** —
D3D9→Metal çevirisi tek başına büyük bir iş; DXVK yıllar aldı, ancak tek bir
oyunun kullandığı SM2.0 alt kümesi çok daha dar.

Tek kişi için gerçekçi tahmin: **ciddi yarı zamanlı 12–24 ay**, tam zamanlı 6–9 ay.
x86 statik rekompilasyonu yeni değil ve bu proje ilk örneği değil:
[M-HT/SR](https://github.com/M-HT/SR) DOS/Windows oyunlarını (Septerra Core,
Battle Isle 3) assembly düzeyinde çevirip macOS arm64 dahil birçok hedefe
taşıyor; [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp)
orijinal Xbox (x86) oyunlarını aynı Python + Capstone → C yöntemiyle lift
ediyor (Burnout 3, 22.097 fonksiyon). Akademik tarafta McSema, rev.ng, RetDec,
BinRec gibi IR tabanlı ikili çeviri çalışmaları yıllardır var.

Bu projenin kendine özgü katkıları daha dar:
(a) dizin girişi sıfırlanmış ama fiziksel olarak duran reloc tablosunu kurtarıp
işaretçi/sabit ayrımında kullanmak, (b) Rosetta 2'yi oracle olarak kullanan
diferansiyel ISA fuzzer'ı (`tools/fuzzisa.py`), (c) oyunun çalışma anında
yığına yazdığı x86 kodu için hibrit yorumlayıcı (`runtime/genexec.c`),
(d) Win32 + Direct3D 9 hedefinden macOS ARM64 / Metal'e uçtan uca oynanabilir
port. Reloc tablosunun sağlam çıkması bu projeyi "araştırma problemi"nden
"çok zor ama çözülebilir mühendislik"e taşıyan şey oldu.

---

# Aşama 3–4: Win32 + D3D9 katmanları, oyun `main()`'e ulaştı

## Nerede duruyoruz

Oyun artık **çalışıyor**: CRT başlangıcı, C++ global kurucuları, `main()`, pencere/registry/dosya
katmanı, 6 guest thread, **Direct3D9 cihazı oluşturuluyor**, ilk kare `Present` ediliyor ve
gömülü shader efektleri (`IDI_WORLD_FX` vb.) yükleniyor.

Tek kalan tıkanma noktası: **D3DX efekt parametre ağacı çevrimli ayrıştırılıyor** (aşağıda).

| Aşama | Durum |
|---|---|
| CRT başlangıcı (heap, TLS, locale, stdio) | ✅ tam |
| C++ statik kurucular (`_initterm`) | ✅ tam |
| `main()` / oyun init | ✅ çalışıyor |
| Registry / pencere / senkronizasyon / thread | ✅ çalışıyor |
| Dosya G/Ç + PE kaynakları | ✅ çalışıyor |
| Direct3D9 cihaz + state block + shader nesneleri | ✅ çalışıyor |
| Efekt (.efx) parametre ağacı | ❌ **çevrimli — tıkanma** |
| Metal ile gerçek çizim | ⬜ başlanmadı |

## Doğrulama altyapısı

### ISA semantik test paketi — `tools/isatest.py`
Elle kodlanmış x86 makine kodu + Intel spec'inden türetilmiş beklenen sonuçlar.
**158 vaka, 417 doğrulama, hepsi geçiyor.** Kapsam: kaydırma/döndürme ve bayrakları,
bit testleri, 8/16/32-bit aritmetik ve bayraklar, çarpma/bölme, işaret genişletme,
tüm `setcc` koşulları, bellek adresleme kipleri, yığın işlemleri, string komutları
(`rep movs/stos/cmps/scas/lods`, `df` yönü dahil), x87 (yükleme/saklama/aritmetik/
karşılaştırma/`fnstsw`/`fistp` yuvarlama), MMX/SSE taşıma.

Bu paket **beş gerçek lifter hatası buldu** (aşağıda). Her derlemede otomatik koşuyor.

### Bulunan ve düzeltilen lifter hataları

1. **`PUSH32` değerlendirme sırası** — makro `esp`'yi azalttıktan *sonra* operandı
   okuyordu. `push [esp+8]` bir slot kayıyordu. MSVC argüman aktarımının temel kalıbı;
   her yeri etkiliyordu.
2. **Dolaylı `call` operand sırası** — `call [esp+0x24]` hedefi dönüş adresi
   itilmeden önce okunmalı. `qsort` gibi geri çağrılar yanlış adrese gidiyordu.
3. **Giriş bloğu sıralaması** — bloklar adrese göre basıldığı için, giriş bloğu en
   düşük adresli olmayan **fonksiyonların %25,2'si yanlış yerden başlıyordu**.
   `__security_check_cookie` doğrudan `__report_gsfailure`'a düşüyor ve oyun
   *"Buffer overrun detected!"* hatası veriyordu.
4. **`shl` OF bayrağı** — sonucun MSB'si yerine orijinalin MSB'si kullanılıyordu.
5. **`fxch` hiç takas etmiyordu** — capstone bu komut için `(st(0), st(i))` verir;
   ilk operand alınınca `ST(0)` kendisiyle takas ediliyordu. x87 hesaplarını
   sessizce bozan bir hata. Ayrıca capstone `fcomip`'i `fcompi` olarak adlandırıyor,
   o dal hiç eşleşmiyordu.

Ek düzeltmeler: `sar` 8/16-bit işaret genişletme, `rol`/`ror` operand boyutu,
`xchg` değerlendirme sırası, `.text` dışına `call`/`jcc`.

### Ekranı kesen çizgiler — örnekleme koordinatı doğrudan interpolant olmalı

Her dörtgenin çerçevesinde ve iki üçgenin paylaştığı köşegende 2 piksellik
çizgi vardı (giriş ekranında köşeden köşeye, yarışta gökyüzünde, harflerin
üzerinde en belirgin).

**Ölçüm zinciri:** mip'i kapatınca (`MIPOFF=1`) çizgi tamamen kayboluyordu →
`lodMaxClamp=1` de siliyordu, yani kenarda LOD **1'in çok üstüne** fırlıyor.
Mip içerikleri eksiksiz (`mip seviyeleri: 0 EKSIK`), üç seviyeli filtre
(`MIPLIN`) ve anizotropi (`NOANISO`) etkisiz, yarım piksel kaydırması
(`HALFPX`) etkisiz.

**Kök neden:** piksel gölgelendiricisinde koordinat `TCRD(k)` makrosuyla
üretiliyordu — dört yollu ternary zinciri artı `abs(w)<1e-6 ? 1.0 : w`
koşullu bölmesi. Türev dörtgeni üçgen kenarına taştığında yardımcı
piksellerin koordinatı bu koşulda ziplayıp altı büyüklük mertebesi
kayıyor, LOD patlıyor ve kenar boyunca çok kaba bir mip seçiliyor.

**Düzeltme:** `D3DTSS_TEXCOORDINDEX` seçimi **köse gölgelendiricisine**
taşındı (seçici `VSPar.tsel`'de 2'şer bit). Piksel tarafında aşama k artık
doğrudan `i.uvk` okuyor; yansıtmalı bölme tek ve sürekli. Yalnızca gerçekten
örnekleme yapılan aşamalar için hesaplanır (dördünü birden üretmek yarış
karesini 15,7 → 18,7 ms yapıyordu). CPU köse yolu aynı permütasyonu uygular.

### Sis (D3DRS_FOGENABLE) — hiç uygulanmamıştı

`RSLOG=1` ölçtü: oyun `FOGENABLE`'ı 5,3 milyon, `FOGCOLOR`'u 1,9 milyon kez
kuruyor; 18 köse gölgelendiricisinden **6'sı `oFog` yazıyor** ama MSL
çevirisinde `oFog` `dummy`'ye gidiyordu. Uzak geometri hiç solmuyordu.
Artık `oFog` tek float varying olarak taşınıyor ve piksel çıkışında
`mix(fogColor, color, saturate(fog))` uygulanıyor (`NOFOG=1` kapatır).
Not: `D3DRS_DEPTHBIAS` ölçüldü — oyun **hiç kurmuyor**, uygulanması gereksiz.

### StretchRect mozaiği — drawable'dan örneklenmemeli

Gökyüzünde ve cam cephelerde 32/64 piksel ızgaralı dikdörtgen mozaik vardı.
Kök neden: sahne kopyası, tam ekran dörtgenle **drawable'dan örnekleyerek**
yapılıyordu; karo sınırlarında bayat içerik geliyordu (blok kenarları tam
32/64 katlarında ölçüldü — Apple GPU karo boyutu). 1:1 kopyalarda artık
`MTLBlitCommandEncoder copyFromTexture` kullanılıyor; ölçekli kopyada önce
gerçek kopyayla ara dokuya alınıyor. `BLITQUAD=1` eski yolu geri getirir.

### Z yüzeyleri — ışıklar ve arabalar duvar arkasından görünüyordu (2026-09-24)

Başından beri şehir ışıklarının parlamaları (ve bazı arabalar) duvarın
arkasından görünüyordu. Oyun görünürlük sorgusu (occlusion query) HİÇ
kullanmıyor (`CreateQuery` 0 kez); parlamalar sıradan Z testiyle gizleniyor.
Kök neden Metal katmanındaydı: **tek bir Z dokusu** vardı ve hedefin boyutu her
değiştiğinde yeniden yaratılıyordu. Bir yarış karesi (`ZSEQ=<kare>` ile ölçüldü):
320x240 ışık hedefi (kendi Z'si) → 6 x 128x128 yansıma küpü yüzü (her birinin
kendi Z'si) → ekran → 640x480 bulanıklık geçişleri (Z yok) ile ekran arasında
gidip gelme. Ekrana her dönüşte Z yeni, tanımsız bir dokudan yükleniyordu;
bulanıklıktan sonra çizilen parlamalar çöp Z'ye karşı test ediliyordu.

Düzeltme D3D9 semantiği:
* `CreateDepthStencilSurface` yüzeye kimlik veriyor (+52, işaret +56);
  1 = örtük yüzey, `GetDepthStencilSurface` bağlı olanı döndürüyor.
* `SetDepthStencilSurface` bağlı yüzeyi seçiyor; NULL → Z testi/yazması kapalı.
* `Clear` bayrağında ZBUFFER (2) varsa o yüzey bir sonraki geçişte verilen Z
  değerine temizleniyor; içerik kareler arası korunuyor (eskiden hedefin
  karedeki ilk kullanımında koşulsuz temizleniyordu).
* `metal.m`: `g_ztex[zid]`, çizim kaydının `depth` alanı bit 8+ = zid; geçiş
  (hedef, Z) değişince yeniden açılıyor. Boyut uyuşmazlığında karalama doku +
  Z kapalı (sayaç raporda: `Z yuzeyi: ... boyut uyumsuz`, ölçülen 0).
Performans değişmedi (yarış 8.34 → 8.34 ms/kare).

### Ayar menüsünden çözünürlük (2026-09-27)

Oyunun Display → Resolution satırı artık altı gerçek seçenek sunuyor:
`720p`, `1080p`, `1440p`, görünümün kendi piksel sayısı (ör. `3024x1836`),
`4K`, `1600x1200` (orijinal 4:3). Ölçülenler:
* Menüdeki yazı `.rdata`'daki hazır metinlerden geliyor (`sub_0049b690`,
  satır `[0x870d1c]` → `0x78fa1c`..`0x78f9e4`; yalnızca gösterim). Metinler
  bellekte yeniden yazılıyor; ilk iki yuva 7 karakter, bu yüzden "720p".
* `EnumAdapterModes` listesi yalnızca SEÇİLEBİLİRLİĞİ belirliyor (oyun
  tablo değerini listede arıyor) → liste oyunun kendi tablosu kalmalı.
  Masaüstü modu 1920x1080 bildirildiği için 1600x1200 satırı (eski "1080p")
  hiç seçilemiyordu; artık ≥1600x1200.
* Satırlar GÖRÜNÜMÜN oranından üretiliyor ("1080p" = pencere oranında 1080
  satır), pencere boyutu sabit; çözünürlük yalnızca kaliteyi değiştiriyor.
  Pencere ilk açılışta kullanılabilir alana göre bir kez boyutlanıyor;
  elle boyutlandırılınca cihaz yeniden kuruluyor.
* Reset'te eski render hedefleri (D3D9'da oyun hepsini bırakmak zorunda)
  3 kare sonra Metal tarafında bırakılıyor; eskiden her değişim ~6 RT
  sızdırıyordu (4K'da >100 MB). Tanı sayacı: bırakılmış RT sonradan
  kullanılırsa rapora `UYARI` düşer (ölçülen 0).

### Tam ekran (2026-09-24)

Yeşil düğme / `Cmd+F` / menüdeki "Tam Ekran" artık gerçek tam ekran: çizim
yüzeyi ekranın **piksel** boyutuna çıkıyor (14" MBP, çentik altı: 3024x1898),
oyunun 4:3 isteği Hor+ ile düzeltiliyor (aspfix 0.837), arayüz ortadaki 4:3
alanda (HUDFIX). Eskiden yüzey 4:3'te kalıp kenarlarda bantla ~1.03x
büyütülüyordu.

**Çözünürlük çalışırken nasıl değişiyor:** oyunun render hedefleri cihaz
boyutuna bağlı; boyutu ancak oyunun kendisi `Reset` ile değiştirebilir.
Windows'taki alt-tab yolu kullanılıyor: geçişte `com_request_reset()` →
`Present` `D3DERR_DEVICELOST`, `TestCooperativeLevel` `D3DERR_DEVICENOTRESET`
döner; oyun (her karede TCL çağırıyor) kaynaklarını bırakıp `Reset` çağırıyor,
`res_pick` → `metal_native_size` tam ekranda ekran boyutunu seçiyor. Menüde ve
yarışta ölçüldü (`RESETAT`, `FSTOGGLE`): oyun iki kez Reset yapıp aynen devam
ediyor. Fare, arayüzün daraltılmış alanına eşleniyor (`metal_set_ui_aspect`).
Tercih `NSUserDefaults` `NFSUFullScreen` ile hatırlanıyor.

**Girdi (2026-09-27 düzeltmesi):** olay döngüsü tüm sol tıkları oyuna
yutuyordu; başlık çubuğundaki yeşil/kırmızı/sarı düğmeler ve pencere
sürükleme hiç çalışmıyordu. Artık oyun alanı dışındaki fare olayları
AppKit'e gidiyor. `fn/Globe+F` (Command taşımadığı için oyuna "F" olarak
gidiyordu) doğrudan yakalanıyor, `Ctrl+Cmd+F` için gizli menü eşi var
(`Cmd+F` de duruyor). `FSSIM` ile üç yol da doğrulandı.
Performans: yarış 8.35 (pencere) / 8.40 ms (tam ekran), GPU 4.9 → 5.1 ms.

### İki imleç / kayma (2026-09-27)

Oyun imleci DirectInput'tan değil `GetCursorPos`'tan geliyor; oyun konumu
`GetClientRect`'e göre kendi arayüz uzayına ölçekliyor. Bu çağrılar sabit
1280x720 dönüyordu; 1280x960'ta imleç dikeyde 1.33x kayıyordu (`MOUSEAT` ile
ölçüldü: (1100,800) ekrandan taşıyordu). Artık oyunun boyutu dönülüyor ve
macOS imleci oyun alanında gizleniyor.

### 4K

`NFSRES=3840x2160` çalışıyor (görüntü temiz). M1 Pro yarış: 8.36 → 8.82 ms/kare,
GPU 4.9 → 6.0 ms. Tam ekranda boyut ekran pikseline eşit ama `NFSMAXPIX`
varsayılanı 6 MP; 4K monitörde tam ekran 3266x1837'ye iniyor.

### 1080p / geniş ekran — `NFSRES`

Oyunun kendi çözünürlük tablosu 6 satır ve tamamı 4:3; menüden 1080p
seçilemiyor (indeks 6+ diziyi taşırır). `NFSRES=1920x1080` (ya da
`./run.sh 1080p`) cihazı istenen boyutta açar. Cihaz oranı oyunun oranından
farklıysa görüntü **yatayda gerilirdi** (ölçüldü: 1080p kare, 4:3 karenin saf
gerdirilmişi — fark 2,03; dikey kırpma varsayımı 34,2), bu yüzden:

* **Hor+ düzeltme:** perspektif çizimlerde kırpma uzayı x'i
  `oyun_oranı / cihaz_oranı` ile ölçeklenir. Oranlar düzelir, yatayda daha
  çok sahne görünür.
* **Perspektif/arayüz ayrımı:** konum matrisinin sabit yazmacı gölgelendirici
  başına bytecode'dan çözülür (`vs_pos_creg`) — c0..c3'te olduğu varsayımı
  YANLIŞ (vs_9'da c10..c13). Ayrım `c[cw].xyz == 0` ile yapılır; `c[cw].w`
  kamera uzaklığıdır, sıfır değildir (ölçüldü: 5,79 / 222,0 / −86,5).
* **Arayüz:** merkeze göre aynı oranla ölçeklenir, daireler dairesel kalır
  (`HUDFIX=0` kapatır).
* **Render hedefleri** cihaz boyutuna çekilir, yoksa son-işlem zinciri
  1280x1024'te kalıp ölçekli blite ve mozaiğe dönüyordu.

Ölçüldü: 1920x1080 ile 1280x1024 **aynı hızda** (15,73 vs 15,72 ms) —
darboğaz GPU değil, yeniden derlenmiş x86 kodu.

### Tembel EFLAGS (bayrak canlılık analizi) — `tools/lift.py`

x86'nın 6 aritmetik bayrağı komut başına eager hesaplanıyordu; oysa çoğu asla
okunmuyor. `flag_liveness()` her fonksiyonun CFG'si üzerinde **geriye doğru
canlılık** çözüyor ve her komut için "bundan sonra hangi bayraklar canlı"
maskesini çıkarıyor. `cpu.h`'deki `FLAGS_*_M` / `SETZSP_M` makroları maskeyi
alıp yalnızca gerekli bayrağı yazıyor.

**Ölçüm:** 385.701 bayrak-yazma yerinde ortalama 6 yerine 2,66 bayrak
(**%56 daha az**), yerlerin %24'ü hiç bayrak hesaplamıyor.
Yoğun sahne kare süresi **18,68 → 15,66 ms (53,5 → 63,9 fps, %19)**.
`FLAGOPT=0` ile kapatılıp A/B yapılabilir.

**Tuzaklar — hepsi gerçek çökmeye yol açtı, hepsi `_flag_defuse()`'da ele alınıyor:**

* **capstone x87'de `eflags` alanını FPU durum bitleri (C0–C3) için yeniden
  kullanıyor** — `insn.eflags` ile `insn.fpu_flags` aynı alan. `fld` 0xd002,
  `fcom` 0x2d döndürüyor; olduğu gibi alınırsa her `fld` "CF yazdı", her
  `fcom` "ZF/SF/AF/OF yazdı" sanılır, o bayrakların gerçek hesabı yukarıda
  atlanır → bayat değer. Oyun x87 dolu olduğu için bu tek başına 12 saniyede
  çökme veriyordu. `f` ile başlayan tüm mnemonic'ler es geçiliyor (capstone'un
  `fpu` grubu güvenilmez: `fldln2` ve `fstp st(0)` grupsuz görünüyor).
  Tek istisna `fcomi`/`fucomi` ailesi — onlar gerçekten 6 bayrağı da yazar.
* **Sayacı 0 olan kaydırma/döndürme bayraklara dokunmaz** (`shl/shr/sar`,
  `rol/ror`, `shld/shrd`; lift'te gövde zaten `if(_s){...}` korumalı).
  capstone yine "yazar" diyor. Sayaç derleme anında sabit ve sıfırdan
  farklıysa güvenilir, aksi halde `def=0`.
* **`rep` önekli `cmps`/`scas` ECX=0 iken hiç çalışmaz** → `def=0`. Ayrıca
  döngü koşulu `if(!c->zf) break;` **kendi yazdığı** ZF'yi okuduğu için ZF
  maskeye zorlanır (`|F_ZF`).
* **capstone bazı bayrak OKUMALARINI hiç bildirmiyor**: `pushfd` (6'sı da),
  `lahf`, `salc`, `rcl`/`rcr`, `cmc`, `daa`/`das`/`aaa`/`aas` → `_EXTRA_USE`.
* **Lifter capstone'un iddiasından az yazıyorsa** bayrak yukarıda öldürülüp
  burada da yazılmaz: `imul`/`mul` yalnızca CF+OF yazıyor → `_DEF_CLAMP`.
* `UNDEFINED_*` **"yazar" sayılmaz** — sayılsaydı bayrağı ölü kabul edip
  gerçek hesabı atlardık ama lift o bayrağı yazmayabilirdi.
* Bilinmeyen sıçrama hedefi / `ret` → `live_out = F_ALL` (muhafazakâr).

**Denetim aracı:** `tools/flagaudit.py` — capstone'un "yazar" dediği ama
lift'in gerçekten yazmadığı her komut biçimini listeler. Bu sınıfın boş
olması gerekiyor (`call`/`ret` hariç, onlar bilerek opak). Tembel bayraklarla
ilgili her hatayı bulan tek araç bu oldu; kör bisect hedefi kaydırıyordu.

### Hata ayıklama araçları
* **Komut izleyici** — son 64K guest komut adresi; çökme anında "buraya nasıl geldik".
* **Watchpoint** — bir guest adresine yazan komutu yakalar (`WATCH_ADDR=0x...`).
* **Guest yığın izi** — `.text` aralığındaki dönüş adreslerini fonksiyon+ofset olarak çözer.
* **Komut kancaları** — `HOOKS=addr,...` ile üretim zamanında istenen adreslere kanca.
* **Fonksiyon override** — `OVERRIDE=addr,...` fonksiyonu `sub_XXXX_orig` olarak üretir,
  runtime `sub_XXXX` sarmalayıcısı sağlar (döngü kırma / hedefli yama).
* **Commit bitmap** — 16 KB sayfa çözünürlüğünde haritalama takibi; hata ayıklayıcının
  haritalanmamış belleğe dokunup ikinci kez çökmesini önler.

## Çalışma zamanı mimarisi (eklenenler)

**Guest bellek haritası**
```
0x00400000-0x00930000  image            0x10000000-0x2F000000  CRT heap (496 MB, baslikli)
0x00900000+            dosya handle'lari 0x30000000-0x38000000  COM nesneleri
0x00A00000+            nesne handle'lari 0x38000000-0x60000000  guest tamponlar (doku/VB)
0x00C00000-0x00D00000  TEB'ler           0x60000000-0x70000000  thread yiginlari (1 MB/thread)
                                         0x7F000000-0x7FF00000  ana yigin (15 MB)
```
4 GB `PROT_NONE` rezerve, kullanılan bölgeler commit edilir.

**Büyük guest kilidi (GIL)** — guest kodu aynı anda tek thread çalıştırır; kilit yalnız
bloklayan çağrılarda (`Sleep`, `SleepEx`, `WaitFor*`) bırakılır. Bu sayede lift edilmiş
kodun paylaştığı bellek/allocator'lar güvenli, komut izi tutarlı kalır.

**Guest thread'leri gerçek pthread** — her biri kendi CPU durumu, guest yığını ve TEB'i ile.
Lift edilmiş kod yeniden girişli olduğu için bu doğrudan çalışıyor. Guest çağrı zinciri host
C yığınında taşındığı için ana guest thread'i **1 GB yığınla** çalıştırılıyor; her thread
kendi `sigaltstack`'ini kurar.

## Win32 katmanı (`runtime/win32.c`, `win32_ext.c`, `fileio.c`, `res.c`)

251/251 import **kesin stdcall argüman sayısına** sahip. (Çağrı sitesindeki `push`
dizisinden çıkarım denendi — bilinen fonksiyonlara karşı yalnız **%74–77 doğru**; her yanlış
sayı `esp`'yi kaydırıp yığını bozduğu için otoriter tablo elle girildi. `wsprintfA` gibi
cdecl variadic'ler 0 olarak işaretli.)

İmplemente edilenler: heap (başlıklı, `HeapSize`/`ReAlloc` doğru), sanal bellek, TLS,
kritik bölümler, zaman, locale/kod sayfası (`GetStringTypeW` C1_* bayrakları,
`LCMapStringW`, `GetLocaleInfo`), `MultiByteToWideChar`/`WideCharToMultiByte`,
`GetProcAddress` (isimle çözümleme), registry (bellek içi, "bulunamadı" varsayılanı →
oyun kendi varsayılanlarını kullanır), pencere/imleç/mesaj döngüsü, event/mutex/semafor,
`CreateThread`, **gerçek dosya G/Ç** (Windows yolları → oyun dizini, büyük-küçük harf
duyarsız çözümleme, dizin tarama), **PE kaynak dizini** (`FindResource`/`LoadResource`/
`LockResource` — gömülü `.efx` shader efektleri buradan geliyor).

## COM / D3D9 katmanı (`runtime/com.c`, `d3d9_tables.h`)

Guest'e native implementasyonlu COM nesnesi sunan köprü: nesnenin vtable'ı guest belleğinde
`0xE0000000|(arayüz<<16)|metot` sihirli adresleriyle doldurulur, `dispatch_indirect` bunları
`com_call`'a yönlendirir. Arayüzler ve **tam metot tabloları** (ad + argüman sayısı):
`IDirect3D9` (17), `IDirect3DDevice9` (119), `Texture9`, `VertexBuffer9`, `IndexBuffer9`,
`Surface9`, `VertexShader9`, `VertexDeclaration9`, `StateBlock9`, `SwapChain9`, `Query9`.

Oyunun fiilen kullandığı yol doğrulandı: `Direct3DCreate9` → `GetDeviceCaps` →
`GetAdapterIdentifier` → `EnumAdapterModes` → `CheckDeviceMultiSampleType` →
**`CreateDevice`** → `BeginScene`/`Clear`/`EndScene`/**`Present`** →
`CreateVertexDeclaration` → `CreateVertexShader`/`CreatePixelShader` →
`BeginStateBlock`/`SetVertexShader`/`EndStateBlock` → `StateBlock::Apply`.

Shader nesneleri gerçek bytecode'u saklıyor (token akışını `0x0000FFFF` END token'ına kadar
tarayarak) ve `GetFunction` onu geri veriyor.

## AÇIK SORUN: efekt parametre ağacı çevrimli

`sub_0065ae35` bir parametre ağacını dolaşıyor (arg3 = ±1, referans sayacı benzeri).
Ağaç **çevrimli**: düğüm 6'nın çocukları 4,5,6,7,8 — yani kendisi. Sonuç: sınırsız
özyineleme, 286.134 çağrıda 15 MB guest yığını tükeniyor.

Kesin olarak elenenler:
* Efekt verisi sağlam — `IDI_WORLD_FX` imzası `0xFEFF0901`, başlık 14 parametre / 2 teknik / 9 nesne.
* Shader yeteneklerine bağlı değil — `D3D_SM=0/11/14` üçünde de **birebir aynı** 286.134 çağrı.
* Kaynak yükleme doğru, dosya G/Ç doğru.
* ISA semantiği 367 testle doğrulanmış.
* Fonksiyon kesintisi yok (`discover` limiti hiçbir fonksiyonu kesmiyor, 0 "kesildi" stub).
* Çözülemeyen dolaylı çağrı 0, desteklenmeyen komut trap'i 0.

İzlenen zincir: `edi = [eax+0x48] + [esi]`, `[edi]` = çocuk taban indeksi.
`[eax+0x48]` ayrıştırıcıda `sub_00667d61` içinde `0x6688cc`'de yazılıyor
(`esi = [ebp-0x40] + word[edi+6]*4`), `[ebp-0x40]` ise `0x6682e9`'da kuruluyor.
Sorunlu düğümde tüm sayaçlar 0 → tüm ofsetler 0 → `[edi]` blob'un başını gösteriyor.
Blob'a değer `0x6688de`'de yazılıyor: `blob[[edx+0x48]] = word[eax+0x44] + 1`.

**Geçici önlem:** `runtime/overrides.c` iki koruma uyguluyor — özyineleme derinliği 32 ile
sınırlı, ve sayaç>0 iken indeks dizisi NULL olan düğümler güvenle atlanıyor. Bu çökmeyi
önlüyor ama çağıran fonksiyon sonsuz tekrar denediği için oyun bu noktayı geçemiyor
(koruma tavanı 200.000'de temiz çıkış yapıyor).

### Bağımsız efekt ayrıştırıcı — `tools/efxparse.py`
D3DX9 `.efx` ikili formatını Python'da ayrıştırır (yer doğrulaması için referans).
`IDI_WORLD_FX` çıktısı:

```
parametre=14 teknik=2 nesne=9
  0 SCALAR  INT    5 eleman   BlendState :BLENDSTATE      <- TEK dizi parametresi
  1 SCALAR  INT    0          BaseMinTextureFilter
  ...
  6 MATRIX_ROWS FLOAT 0       TextureOffsetMatrix         <- cevrimde suclanan dugum
  ...
 13 OBJECT  SAMPLER 0         samp
```

**Hiçbir parametre STRUCT değil; yalnız parametre 0 dizi (5 eleman).** Yani doğru
ayrıştırmada düğüm 6'nın hiç çocuğu olmamalı. Yürüyücünün okuduğu `[edi]=5, icSayi=5`
tam olarak parametre 0'ın eleman sayısı — yani yanlış girdiye bakıyor.

### İzin son halkası (bulundu)
Çocuk taban indeksi zinciri sonuna kadar sürüldü:

```
0x0066fbf8  mov ax, word [ebx+0x30]      ; ebx = efekt nesnesi, +0x30 = tahsis sayaci
0x0066fbfc  mov word [edi+0x44], ax      ; her dugume "cocuklarim buradan baslar"
0x006688d3  mov cx, word [eax+0x44]
0x006688de  mov word [blob + ofset], cx+1
0x0065aecd  movzx ecx, word [edi]        ; yuruyucu bunu okur
0x0065aed3  lea eax, [ecx+eax-1]         ; cocuk = [edi] + i - 1
```

Ölçüm: `[ebx+0x30]` sayacı gerçekten artıyor (0→1→2→3→4→…→8, artiran komutlar
`0x0066b7cb`, `0x006655df`, `0x00665305`), **ama 14 düğümün tamamına yazma işlemi
sayaç 3'teyken yapılıyor.** Yani tüm düğümler "çocuklarım 3'ten başlar" diyor; düğüm 6
de 4..8 aralığını kendine çocuk sayıyor ve çevrim oluşuyor.

### Mekanizma çözüldü (ama hata hâlâ açık)

İlgili fonksiyonlar:

| Adres | Fonksiyon | Rol |
|---|---|---|
| `0x0066fbfc` | `sub_0066f6ac` | `[edi+0x44] = [ebx+0x30]` — düğüme "çocuklarım buradan başlar" yazar |
| `0x0066fc10` | → `sub_0066549b` | hemen ardından çocukları tahsis eder, sayacı ilerletir |
| `0x006655df` | `sub_0066549b` | `add word [edi+0x30], si` — si = çocuk sayısı |
| `0x00665305` | `sub_0066521d` | `inc word [esi+0x30]` |
| `0x0066b7cb` | `sub_0066b61d` | `inc word [ebx+0x30]` |

Tasarım **doğru**: önce mevcut taban kaydedilir, sonra çocuklar tahsis edilip sayaç
ilerletilir. Ölçüm de bunu doğruluyor — sayaç 3'ten 8'e çıkıyor, tam olarak
`BlendState`'in **5 elemanı** kadar (`0x006655df`'de +1, `0x00665305`'te 4 kez +1).
Elemanı olmayan 13 parametre için `si=0` olduğundan sayaç ilerlemez; bu da doğru.

Sayaç 8'e ulaşıyor ve efektte **9 nesne** var — yani `[ebx+0x30]` nesne/eleman sayacı
ve `[this+0x2c]` 9 elemanlı nesne dizisi. Yürüyücünün gezdiği indeks aralığı (0..8) buna oturuyor.

**Açık kalan:** Çocuğu olmayan düğümlerde de taban 3 (veya 4) olarak yazılı kalıyor ve
yürüyücü `icSayi==0` durumunda onu **1'e zorluyor** (`0x0065aeb8`–`0x0065aebe`), sonra
`cocuk = taban + i - 1` ile özyineleniyor. Yani elemanı olmayan bir düğüm bile bir
çocuğa iniyor. Doğru davranışta bu düğümlerin `[edi]` girdisi 0 olup
`0x0065aea4`'teki `cmp word [edi], dx; je` ile atlanmalıydı — ama blob girdileri
çakıştığı için 0 yerine 5 okunuyor.

**Sonraki adım (net):** Blob girdilerinin neden çakıştığını bulmak. `[edx+0x48]`
ofsetleri düğüm başına ayrı olmalı; ölçümde 0 ve 4 değerleri **aynı blob içinde iki
farklı düğüme** yazılıyor ama üçüncü düğüm yine ofset 0'ı okuyor. `sub_00667d61`
içindeki 4 yollu tip anahtarının (`0x0066888e` sonrası: `0x668b62`/`0x668ab7`/
`0x668934`/düşme) hangi dalının hangi parametre sınıfı için çalıştığını
`tools/efxparse.py` çıktısıyla eşleştirmek gerekiyor — 14 parametrenin sınıfları
biliniyor (SCALAR/VECTOR/MATRIX_ROWS/OBJECT), dolayısıyla beklenen dal dizisi
hesaplanabilir ve gerçekle karşılaştırılabilir.

## Komutlar

**Sürümler:** depo kökündeki `SURUM` dosyası (şu an `2`). Paket bunu sürüm
bilgisine yazar (Finder'da "2.0"), ikili açılışta günlüğe yazar, oturum kayıtları
`~/nfs_oturum/<tarih>_v2` diye etiketlenir.
- **v1:** temel çevirici, `-fno-strict-aliasing` yok. Arşiv: `arsiv/v1/` (git dışı,
  içeriği `arsiv/v1/OKUBENI.txt`).
- **v2 (2026-10-18):** FLAGABI + X87OPT + RETEIP=0 + MEMARG=1, her şey
  `-fno-strict-aliasing` ile. Oyun kodu kare başına −%22 işlemci süresi
  (`LIFTER_VERIMLILIK.md`).

Yeni bir lifter adımı önce ayrı bir `gen/<ad>` dizininde denenir. Ana sürüme
alınınca eski sürüm `arsiv/vN/` altına taşınır ve `SURUM` bir artırılır.

### Çalıştırma
```bash
./run.sh window      # pencereli (Metal)
./run.sh headless    # penceresiz, 60 sn
./run.sh profile     # ornekleyici profil
./run.sh test        # 439 ISA + 15 fonksiyon testi
./run.sh hd          # HD build: grafik iyilestirmeleri (NFSHD=1 her modla calisir)
```

**İki build:** `out/nfsu2_boot` oyunun orijinal görüntüsü, `out/nfsu2_hd_boot`
grafik iyileştirmeleri. Lift edilmiş shard'lar ortak; yalnızca runtime
`-DNFS_ENHANCED=1` ile ikinci kez derlenir (`./link.sh [orig|hd]`). Orijinal
build'de iyileştirme kodu derlenmez. Paket: `./pack.sh --hd` → `NFSU2 HD.app`.
Ayrıntı ve sonuçlar: `GRAFIK_ARASTIRMA.md` §6 (yansıma çözünürlüğü), §7 (MSAA),
§8 (SSR + yansıma ayrıştırma), §10 (ışın izlemeli yansıma + sabit sahne önbelleği).

```bash
# tam yeniden uretim + derleme (~2.5 dk).  Depo kokunden calistirin.
# 1) reloc tablosu -> out/relocs.txt  (genall.py ve argcount.py bunu okur)
./.venv/bin/python tools/reloc2.py <SPEED2.EXE>
# 2) v2: bayrak canliligi + 23k fonksiyonu C'ye cevir -> gen/all/, import
#    arguman tablosu -> gen/all/impargs.c.  Hepsi tek betikte; tarif gen/all'i
#    birebir uretir (2026-10-18 dogrulandi: 131 dosya, 0 fark).
# DIKKAT: betikteki OVERRIDE listesi runtime/overrides.c + runtime/d3dx_hle.c
# ile AYNI olmali, yoksa sub_XXXX_orig sembolleri uretilmez ve link kirilir.
# d3dx_hle.c sarmalayicilari makroyla uretiyor (grep bulamaz); guncel liste
# D3DX_HLE.md "Yeniden uretim" bolumunde.
tools/uret_v2.sh <SPEED2.EXE>
# 3) derle + linkle
./build.sh && ./link.sh          # OPT: -O3 -mcpu=apple-m1 -fno-strict-aliasing

# sadece runtime degistiyse (~10 sn)
./link.sh

# calistir / teshis
./diag.sh                          # cokme adresini bul + cevresini sok
ALARM=60 ./out/nfsu2_boot <EXE>    # zaman asimli calistirma
ITRACE=40 ./out/nfsu2_boot <EXE>   # son 40 guest komutu
WATCH_ADDR=0x... ./out/nfsu2_boot  # bellek watchpoint
COMTRACE=1 ./out/nfsu2_boot        # D3D9 cagri izi
./out/isatest                      # 367 ISA semantik testi
./out/nfsu2_all <EXE>              # 15 fonksiyon dogrulama testi
```

---

# DÖNÜM NOKTASI: oyun çalışıyor ve kare üretiyor

25 Ağustos 2026 sabahı itibarıyla **NFSU2 motoru native ARM64 kodunda çalışıyor**:
CRT → statik kurucular → `main()` → D3D9 cihazı → efekt sistemi → veri yükleme →
**oyun döngüsü ve ön yüz (menü)**.

## Ölçülen çalışma durumu (60 sn'lik koşu)

| | |
|---|---|
| **Render edilen kare (`Present`)** | **373** |
| **Çizim çağrısı (`Draw*`)** | **69.540** |
| Toplam COM/D3D9 çağrısı | **1.729.435** |
| Toplam Win32 çağrısı | 54.369 |
| Açılan veri dosyası | **64** (12,7 MB okundu) |
| Guest thread | 9 başlatıldı, 5 sonlandı |
| Yürütülen guest komutu | ~1,1 milyar |
| Çözülemeyen dolaylı çağrı | 0 |
| Desteklenmeyen komut trap'i | 0 |

Yüklenen gerçek oyun verisi: `GLOBAL/GLOBALA.BUN`, `GLOBAL/GLOBALB.LZC`,
`GLOBAL/GLOBALTEXTURESROW.BIN`, **`FRONTEND/FRONTB.LZC`** (menü),
`LANGUAGES/ENGLISH.BIN`, `memcard/LOCALE_ENGLISH.loc`, `sdata/sdat.viv` (ses),
`Global/FrontEndMemoryFile.bin`, `Global/CarHeadersMemoryFile.bin`.

CPU kullanımı %2 — oyun çizim döngüsünde girdi bekleyerek boşta dönüyor,
yani menüde duruyor.

## Bu aşamada bulunan ve düzeltilen hatalar

**Lifter (6. ve 7. gerçek hata):**
6. **`int3` blok sonlandırıcı sanılıyordu.** Assertion amaçlı tek `int3`'ten sonraki
   epilog hiç üretilmiyordu ve yürütme bir sonraki fonksiyonun gövdesine düşüyordu.
   Artık tek `int3` etkisiz sayılıp devam ediliyor; ardışık `0xCC` dizisi (fonksiyonlar
   arası dolgu) hâlâ blok sonu.

**Runtime:**
* **`ExitThread` gerçekten sonlandırmıyordu.** `_endthreadex` sonunda `ExitThread`
  çağrılıyor; stub sadece dönünce yürütme `_threadstartex` prologuna **düşüyor** ve
  thread kendini yeniden başlatıyordu. Artık `pthread_exit` ile sonlanıyor.
* **TLS thread'e özel değildi.** `TlsGetValue`/`TlsSetValue` global bir dizi
  kullanıyordu; artık TEB'deki per-thread dizi (`fs:[0x2c]`) kullanılıyor.
* **`CREATE_SUSPENDED` yok sayılıyordu.** Artık thread `ResumeThread`'e kadar bekliyor.
* **Büyük guest kilidi (GIL) açlığa yol açıyordu.** Oyun `EnterCriticalSection` ile
  dönen bir bekleme döngüsündeydi; kilit yalnız `Sleep`/`Wait` çağrılarında
  bırakıldığı için beklenen thread hiç çalışamıyordu (188 milyon boş çağrı).
  Artık her 1024 Win32 çağrısında bir kilit adilce bırakılıyor.
* **`GetCurrentThreadId` tüm thread'ler için 1 dönüyordu** — artık TEB'den okunuyor.

**Yeni katmanlar:** `IDirectInput8` + `IDirectInputDevice8` (11 + 32 metot),
`IDirectSound` + `IDirectSoundBuffer` (11 + 21 metot), gerçek dosya G/Ç, PE kaynakları.

## Yeni araçlar

* **Örnekleyici profil** (`SAMPLE=1`) — ayrı thread periyodik olarak çalışan guest
  komutunu örnekler. Sonsuz döngüleri ve sıcak noktaları saniyeler içinde bulur;
  GIL açlığını bununla yakaladık.
* **Çalışma zamanı yama tablosu** (`dispatch_patch`) — dolaylı çağrılan bir guest
  fonksiyonunu, **yeniden üretim/derleme gerektirmeden** C sarmalayıcısıyla
  değiştirir. İterasyonu 5 dakikadan 10 saniyeye indirdi.
* **Heap blok sınır denetimi** — bir guest adresini içeren heap bloğunu başlıktan
  bulur; dizi taşmalarını kesin tespit eder.

## AÇIK: D3DX efekt ağacı çevrimli (geçici koruma ile aşıldı)

Ağaç düğüm 0 → 6 → 4..8 → 6 şeklinde kendine dönüyor ve düğüm dizisi kapasitesi 8
iken indeks 8 isteniyor. `runtime/overrides.c` iki koruma uyguluyor: dizi sınırı ve
**ziyaret edilmiş düğüm kümesi** (her düğüm üst-seviye yürüyüşte bir kez). Koruma
60 sn'lik koşuda yalnız 1.295 kez devreye giriyor, yani sınırlı ve oyun ilerliyor.
Kök neden hâlâ açık — ayrıntılı iz yukarıdaki bölümde.

## SONRAKİ AŞAMA: D3D9 → Metal

Oyunun **fiilen kullandığı** D3D9 yüzeyi ölçüldü (60 sn, tek menü ekranı):

| Metot | Çağrı |
|---|---|
| `SetRenderState` | 646.609 |
| `SetSamplerState` | 360.874 |
| `SetTextureStageState` | 243.416 |
| `SetTexture` | 69.779 |
| `SetVertexShader` / `SetPixelShader` | ~69.700 |
| **`DrawPrimitiveUP`** | **69.526** |
| `SetVertexShaderConstantF` | 69.257 |
| `StateBlock::Capture` / `Apply` | ~11.300 |
| `SetRenderTarget` / `SetDepthStencilSurface` | 1.386 |
| `CreateTexture` + `LockRect`/`UnlockRect` | 776 / 1.205 |
| `SetViewport`, `SetTransform` | 1.064 / 966 |
| `Present` / `BeginScene` | 372 |

**Kritik kolaylık:** ana çizim yolu `DrawPrimitiveUP` — köşe verisi doğrudan bir
bellek işaretçisinden geliyor, vertex buffer yönetimi yok. Metal'de her çizimde
tamponu yükleyip çizmek yeterli.

Gereken sıra:
1. ✅ **Gerçek `NSWindow` + `CAMetalLayer`** — `runtime/metal.m`. Ana thread
   NSApplication döngüsünü sürer, guest ayrı thread'de çalışır.
2. ✅ **`Clear` + `Present` → Metal render pass.** Oyunun `Present()` çağrıları
   gerçek `CAMetalDrawable`'ları sürüyor: 30 sn'de 456 oyun karesi, **138 Metal
   sunumu** (Apple M1 Pro). Pencere şu an yalnız oyunun `Clear` rengini gösteriyor.
3. ✅ **`CreateTexture`/`LockRect`/`UnlockRect` → `MTLTexture`.**
   A8R8G8B8→BGRA8Unorm, DXT1→BC1, DXT3→BC2, DXT5→BC3 (Apple Silicon BC destekliyor).
   Mip seviyeleri ve blok-sıkıştırılmış pitch hesabı dahil.
   30 sn'de **767 doku oluşturuldu, 1.186 yükleme**.
4. ✅ **`DrawPrimitiveUP` → Metal çizim yolu.** Köşe düzeni ölçülerek çıkarıldı:
   `POSITION float3 @0, COLOR D3DCOLOR @12, TEXCOORD float2 @16`, adım 32 bayt,
   `FVF=0x142` (XYZ|DIFFUSE|TEX1). **Köşeler zaten NDC'de (-1..1)**, yani vertex
   shader çevrilmeden geometri doğrudan çizilebiliyor. Metal'de triangle fan
   olmadığı için fan/strip/list → üçgen listesine dönüştürülüyor.
   30 sn'de **41.126 üçgen** ekrana çizildi.
5. ⬜ Shader Model 2.0 bytecode → MSL çevirisi (en büyük kalan kalem).
   Şu an sabit bir vertex/fragment shader çifti kullanılıyor: pozisyon geçişli,
   `renk × doku` ile alpha blend.
6. ⬜ `SetRenderState`/`SetSamplerState` → `MTLRenderPipelineState` /
   `MTLSamplerState` önbelleği (çağrı sayıları yüksek, önbellek şart).
7. ✅ **Girdi: `NSEvent` → DirectInput klavyesi.** macOS sanal tuş kodları
   DirectInput `DIK_*` tarama kodlarına eşleniyor (harfler, rakamlar, oklar,
   Enter/Space/Esc/Tab/Shift/Ctrl). Oyun `GetDeviceState`'i **her karede**
   çağırıyor (30 sn'de 399 kez) ve 256 baytlık tabloyu okuyor.
   Cmd+Esc pencereyi kapatır.

### Ölçülen DirectInput/DirectSound kullanımı
```
IDirectInput8        CreateDevice 1, EnumDevices 1, EnumDevicesBySemantics 1
IDirectInputDevice8  SetDataFormat 1, SetCooperativeLevel 1, Acquire 1,
                     GetDeviceState 399      <- her kare klavye yoklamasi
IDirectSound         CreateSoundBuffer 7, GetCaps 1, SetCooperativeLevel 2
```

### Render mimarisi
macOS pencereyi ana thread'de ister; guest zaten ayrı thread'de çalışıyor.
Guest thread çizimleri çift tamponlu bir komut kuyruğuna biriktirir
(`BeginScene` sıfırlar, `Present` tamponu ana thread'e devreder), ana thread
`CAMetalDrawable` üzerine encode eder. Kilit yalnız kuyruk devrinde tutulur.

---

# Performans: 60.3 → 98.8 fps (yarış karesi 16.59 → 10.12 ms)

**Uçtan uca rakam** A/B/A/B dönüşümlü ölçüldü (eski = `PACE60=1`, yeni =
varsayılan), yani ısıl sürükleme iki yapılandırmaya eşit dağıldı:
eşleştirilmiş kare süresi **−%39.2**. Sanal/gerçek saat oranı 0.998 vs 0.999
— **oyun hızı değişmiyor**, yalnızca akıcılık artıyor.

> Ardışık ölçümlere dikkat: makine saatlerce yük altında kalınca ısıl
> sürükleme %3–4'e ulaşıyor. Aşağıdaki adım adım tablo saatler içine yayılmış
> koşulardan; adımların *sırası* ve *büyüklük mertebesi* güvenilir, mutlak
> değerleri dönüşümlü ölçümle karşılaştırmayın.

Ölçüm penceresi: yarışın ortası, `cizim > 400` olan kareler, gerçek-zaman
kovalarına eşleştirilmiş (bkz. "Ölçüm yöntemi"). Sıra, uygulanma sırası:

| adım | ms | fps |
|---|---|---|
| başlangıç (60 Hz freni açık) | 15.70 | 63.7 |
| kare freni kaldırıldı | 12.60 | 79.4 |
| COM dağıtımı isim → sayısal kimlik | 11.06 | 90.4 |
| üçgen listesinde tek blok köşe kopyası | 10.84 | 92.3 |
| zincir sırası + küp doku koşul sırası | 10.63 | 94.1 |
| `restrict`, kodlama kilidi, `rs_apply` kirli bayrağı | 9.82 | **101.8** |

## 1. Kare freni: karenin %61'i uykuydu

`frame_pace()` (`runtime/win32_ext.c`) kareyi 60 Hz'e sabitliyordu. Gerekçesi
şuydu: sanal saat kare başına TAM 16.667 ms ilerlediği için oyun hızı =
(gerçek fps / 60). **O gerekçe artık geçersiz** — sanal saat `VCLOCK=real`
moduna geçtiğinden oyun gerçek zamanda akıyor.

Profil bunu doğrudan gösterdi: konuk thread'in 18671 örneğinin **11438'i**
(%61) `frame_pace` → `nanosleep` → `__semwait_signal` idi.

Fren artık **varsayılan olarak kapalı**; `PACE60=1` eskisini geri getirir.
Oyun hızlanmıyor: sanal/gerçek oranı 63.7 fps'te de 101.8 fps'te de **0.999**.
Kaçak fps'i vsync kesiyor (menülerde 120 fps = ekran tazeleme sınırı) ve sanal
saatin kare başı artışı 4 ms'de (250 fps) tabanlı.

> Bu, oturumun ilk yarısındaki bütün "optimizasyon hiçbir şey değiştirmedi"
> sonuçlarını da açıklıyor: karenin %61'i uyku olduğu için her CPU tasarrufu
> frenleyici tarafından yutuluyordu. **Fren varken CPU ölçümü yapılamaz.**

## 2. COM dağıtımı: isim karşılaştırmasından sayısal kimliğe

Bir yarış koşusunda **377 milyon `com_call`** var (kare başına ~43, çizim
başına ~43). Dağıtım metot ADINI karşılaştırıyordu. Önce `NMEQ` (ilk harf
kontrolü + `strcmp`) denendi, **hiçbir şey kazandırmadı**: D3D9 cihaz
metotlarının neredeyse hepsi `Set`/`Get`/`Draw` ile başlıyor, yani ilk-harf
filtresi elemiyor ve her adımda yine tam `strcmp` çalışıyor. Profilde konuk
thread'in %11'i `strcmp`'ti, 2335 örneği `com_call`'dan.

Çözüm: her metoda bir `MID_*` sabiti; kimlik `(iface, slot) → id` önbelleğiyle
metot başına **bir kez** çözülüyor, zincir tamamen tamsayı karşılaştırması.
Ağırlıklı zincir derinliği 26.4 karşılaştırma/çağrı, toplam **9.9 milyar**
karşılaştırma idi.

Ardından zincir çağrı sıklığına göre yeniden sıralandı (en sıcak 11 segment
öne). Güvenlik ölçütü: `MID` testleri karşılıklı dışlayan; taşınabilmesi için
segmentin `strncmp(nm,"Draw",4)` / `strncmp(nm,"Get",3)` önek bloklarının
üzerinden atlamaması gerekiyor — `Draw*` bloğu bütün olarak taşındı.

## 3. Ölçüm yöntemi (bu kadar önemli olduğu ortaya çıktı)

Tek koşu karşılaştırmaları **±%7 gürültülüydü** — bir değişikliğin 94.1'den
87.6'ya "düşürdüğü" görüldü, oysa o değişiklik ancak yardım edebilirdi.
Sebep: koşular arası sahne farkı.

Denenen ve **elenen** yol: `VCLOCK=fixed` ile kare-indeksine göre eşleştirme.
Sabit saatte oyun 2× hızlanıyor, AUTOKEY dizisi yarışa hiç girmiyor (kare
başına 83 çizim = menü).

Kullanılan yol: gerçek saatte oyun durumu **geçen gerçek zamana** bağlı, yani
aynı saniye = aynı sahne. Kareler 0.5 sn'lik kovalara atılıp kova başına
medyan alınıyor, iki yapı kova kova eşleştiriliyor. Koşu tekrarı: **10.32 /
10.33 ms** (%0.1). Betikler: `scratchpad/bench.sh`, `bcmp.py`.

## 4. Küçük ama gerçek olanlar

- **Tek blok köşe kopyası** (`metal_draw_raw`): üçgen listesinde kaynak zaten
  bitişik, oysa KÖŞE BAŞINA `memcpy` çağrılıyordu (~112 bin küçük çağrı/kare).
  `_platform_memmove` %4.6 → %2.8.
- **`restrict`** (`tools/lift.py` artık `CPU *restrict c` üretiyor): konuk
  kayıtları `CPU` yapısında, `MR32()` ise `g_mem`'e AYNI türden (`uint32_t`)
  yazıyor; derleyici bu yüzden her konuk bellek yazmasından sonra `c->eax`
  gibi alanları yeniden yüklemek zorundaydı. CPU yapısı `g_mem`'in içinde
  olmadığı için `restrict` doğru. −%1.7.
- **Kodlama kilidi** (`runtime/metal.m`): ana thread `g_qlock`'u TÜM kare
  kodlaması boyunca (0.4–2 ms) tutuyordu; `metal_draw_raw` çizim başına aynı
  kilidi istediği için konuk thread o pencerede her çizimde blokluyordu — çift
  tamponlamanın amacı tam da bu örtüşmeydi. Kilit artık yalnızca `g_ready`
  okuması/yazması için. Kilit beklemesi %2.2 → %1.2.
- **`rs_apply` kirli bayrağı**: `SetRenderState` koşuda 110M kez çağrılıyor ve
  **%96.5'i aynı değeri yeniden yazıyor**. `rs_apply` tamamen `g_rs[]`'ten
  türediği için atlamak birebir aynı sonucu verir; 110M → 3.85M uygulama.
  (Bayrak kalıcı: metal hazır değilken yapılan değişiklikler cihaz gelince
  yine uygulanır.)
- **`is_cube_tex` koşul sırası**: konuk BELLEĞİ okuyan bu test her doku
  çağrısında çalışıyordu; `IDirect3DTexture9` AddRef+Release tek başına 96.8M
  çağrı. Ucuz metot kimliği testi öne alındı.
- **`ENVC()` / `mem_is_committed` satır içi**: `getenv` ve `strcmp` profilden
  tamamen kayboldu.

## 5. Ölçülüp ELENENLER (tekrar denemeyin)

- **PGO** (`-fprofile-generate` → `llvm-profdata` → `-fprofile-use`): `restrict`
  üzerine yalnızca **%0.9**. İki aşamalı derleme + yeniden üretimde bayatlayan
  profil dosyası buna değmiyor. Altyapı duruyor: `link.sh` `$LDOPT` alıyor,
  `QUITFRAMES=<n>` temiz çıkış veriyor (ALARM yolu sinyal işleyicisinden
  `_exit` ediyor ve `atexit`'i atlıyor), `tests/boot.c` profili `dlsym` ile
  yazıyor (`weak_import` Mach-O'da link hatası veriyor).
- **`NOVSYNC=1`**: 15.71 → 15.95 ms, yani hiçbir şey. Vsync bizi hiç
  kısıtlamıyor (`nextDrawable` 0.02 ms/kare).
- **`dispatch_indirect` yama tablosu sırası** ve **`mem_is_committed` satır
  içi**: +%0.3, gürültü içinde. `g_patch_n` aslında 1; kaldırılan tarama iki
  komuttu. (Değişiklikler kod olarak doğru, duruyorlar.)

- **Metal sistem iş parçacıkları (2026-10-18, v2).** Yarışta süreç işlemcisinin
  ~%30'u macOS'un Metal/CoreAnimation GCD iş parçacıklarında (0.34–0.39 çekirdek,
  %90'ı çekirdek modu; `sample`: `IOGPUCommandQueueSubmitCommandBuffers`). Denenenler:
  - **Kalıcı yerleşim kümesi** (`MTLResidencySet`, tüm doku ve tamponlar): etki yok
    (0.384/0.389 → 0.380/0.362, dönüşümlü 2+2).
  - **Kareye +20/+40 boş geçiş:** +0.04 / +0.01 çekirdek, yani geçiş başına teslim
    maliyeti küçük.
  - **Mikro karşılaştırma** (oyunun deseni: 1400 çizim, 25 geçiş, 200 doku, satır içi
    548 B sabit, tam ekran hedef + derinlik, 120 Hz `CAMetalLayer` sunumu): 0.39–0.41
    çekirdek. Bu, oyundaki ~0.45'e (render + GCD) çok yakın; boş kareyi 120 Hz'de
    sunmak bile 0.15 çekirdek. Yani anormallik yok, desenin bedeli bu.
  - **`setVertexBufferOffset` + sabitleri kare tamponundan vermek:** en iyi ihtimalle
    −0.02 çekirdek (~0.05 W), gürültü sınırında; yapılmadı.

  Bulunan tek gerçek kirlilik ölçüm aracıydı: PERFLOG ekran görüntüsü GPU'nun
  sıkıştırdığı dokuyu satır satır `getBytes` ile okuyordu (~0.08 çekirdek, yalnızca
  kayıt sırasında). Artık doğrusal `MTLBuffer`'a kopyalanıyor.

- **GEÇİŞ YÜKLEMESİ ve Z SAKLAMA (2026-10-18):** render geçişlerinde gereksiz
  görünen renk/Z yükleme-saklamayı kesmek. Koşu içi A/B'de GPU enerjisi ve DRAM
  trafiği değişmedi (Apple GPU bunları zaten atlıyor); Z saklama D3D9 sözleşmesine
  aykırı (oyun örtük Z'yi kareler arası koruyor). Ayrıntı: "GPU enerjisi ve bellek
  trafiği".

## 6. Şu anki profil (konuk thread, yarış)

```
79.1%  lifted x86 (oyunun kendisi)
 8.5%  com_call
 3.4%  _platform_memmove
 3.0%  dispatch_indirect
 1.2%  __psynch_cvwait
```

Konuk thread beklemesi 0.29 ms/kare, ana thread kare gelsin diye 9.88 ms/kare
BOŞ bekliyor, GPU 2.19 ms, CPU kodlama 0.60 ms. **Darboğaz tamamen konuk
thread**; grafik katmanı bedava. Sıradaki gerçek kaldıraç `com_call`'ın kalan
gövdesi (%8.5, 377M çağrı) ya da lifter'ın kod üretimi.

## 6b. D3DX efekt kütüphanesi: konuk sürenin %47'si (2026-09-23)

Yeni profilde çevrilmiş kodun büyük kısmı oyunun kendi mantığı değil, oyuna
statik bağlı **D3DX9 efekt çerçevesi** çıktı: `ID3DXEffect::BeginPass`
(vtable 0x7b6fe8, yuva 66) alt çağrılarıyla konuk thread'in %48'i. Oyun her
çizimde BeginPass çağırıyor (1398/kare), D3DX de her seferinde ~20 durumu ve
~8.5 preshader ifadesini yeniden hesaplıyor. Oysa ifade girdilerinin %95.8'i
bir önceki değerlendirmeyle aynı.

İlk HLE adımı (durum değeri notu, `runtime/d3dx_hle.c`) konuk CPU süresini
yarışta **10.9–12.0 → 8.1 ms/kare (−%29)** indirdi; GPU boşken yarış
90 → 118.9 fps (120 Hz sınırı). Bellek karşılaştırmalı doğrulamada 2.4M+
kontrol, 0 fark.

Aşama 2 (durum atama notu) D3DX'in geçiş başına ~27 cihaz çağrısına
dayanıyor; bunların %96'sı gereksiz. Kaydedilen çağrılar şu an etkisizse
durum atlanıyor. 60 fps'e sabitlenmiş ölçümde konuk CPU'yu bir %5 daha
indirdi; 120 Hz'de yarış zaten sınırda olduğu için fps değişmedi. 6.6M+
doğrulama kontrolü, 0 fark.

Aşama 3 (parametre sürümleriyle geçiş düzeyinde atlama) olumsuz çıktı.
Dokular parametre verisinden değil efektin nesne tablosundan geliyor ve oyun
her çizimde doku değiştiriyor; doğrulamada tahmini isabetlerin %78'i yanlıştı.
Kod duruyor, varsayılan kapalı.

Harita, veri yapıları, aşamalar ve ilerleme günlüğü: **`D3DX_HLE.md`**.

## 7. `sub_0050d510` hatası: oyun hatası DEĞİL, kapanış yarışı

Koşuların bir kısmında görülen

```
*** HATA GUEST FONKSIYONU: sub_0050d510
*** HOST SEGV: 0x... (guest alani disi)
```

**oyun sırasında değil, `ALARM` kapanışında** oluyor. Kanıt: hata çıktısının
hemen ardından rapor dökümü geliyor, ve çöken koşu en FAZLA kareyi üreten
koşu. `on_alarm` sinyal işleyicisinden `all_reports()` çağrılıyor ve konuk
thread'ler hâlâ koşarken COM/rapor yapıları geziliyor.

Ölçüm: temiz çıkışta (`QUITFRAMES=<n>`) **5/5 koşu, 0 hata** (üçü birebir
10301 kare); `ALARM` ile 7 koşunun 2'si çöküyor. Bildirilen fonksiyon adı da
yanıltıcı — `sub_0050d510` `overrides.c`'deki bir C override'ı, host PC ondan
sonraki fonksiyonlara düşünce ona atfediliyor.

**Sonuç:** ölçüm ve otomatik koşularda `ALARM` yerine `QUITFRAMES` kullanın.
(Ayrıca bu oturumda konuk işçi thread'lerinin yığını 512 KB → 256 MB
yapıldı; `CreateThread` `pthread_create(...,0,...)` ile macOS varsayılanını
alıyordu, oysa ana konuk thread 1 GB alıyor. Doğru bir düzeltme ama bu
hatanın nedeni o değildi.)

## 8. Menüde 1920x1080

Vanilya NFSU2'nin kendi çözünürlük tablosu 6 satır ve tamamı 4:3.
`EnumAdapterModes` ile **7.** bir mod ilan etmek menüyü o satırı seçebilir
hale getiriyor ama oyun kendi dizisini taşırıyor ve çöp boyut üretiyor (her
seferinde tam olarak `8126268x268571776`). Çöp yalnızca `CreateDevice`'a
gelmiyor — oyun onu **saklıyor** ve render hedeflerini de o boyutla yaratmaya
çalışıyor (Metal doku doğrulaması patlıyor).

Tabloyu konuk bellekte yamalamak temiz çözüm olurdu; **bulunamadı** (hem
12-uint32 dizisi araması hem taşma imzası `1600,1200,0x007C08FC,0x10021C00`
arandı, ikisi de tutmadı — tablo bitişik bir `{w,h}` dizisi olarak durmuyor).

Uygulanan çözüm: taşma **hiç tetiklenmiyor**. Menüdeki 6. satır oyuna
`1600x1200`, kullanıcıya `1920x1080` olarak görünüyor; oyun kendi tablosundan
geçerli 4:3 değeri okumaya devam ettiği için iç durumu tutarlı kalıyor, biz
cihazı 1920x1080'e kurup farkı kırpma uzayında telafi ediyoruz — `NFSRES`
yolunun aynısı. `NOWIDE=1` gerçek 1600x1200'e döner. Ölçüm: 97.6 vs 99.1 fps,
yani bedava.

## 9. Ölçüm tuzağı: `$OPT` boşsa `-O0`

`build.sh` derleme komutunu `xargs -I{} sh -c "..."` ile kuruyor ve `$OPT`'a
uzun bir bayrak eklenince "command line cannot be assembled, too long" hatası
veriyor; bu yüzden sarmalayıcı betik (`scratchpad/cc1.sh`) yazıldı. Betik
`$OPT`'u **doğrudan** kullanıyordu, yani `unset OPT` ile çalıştırılınca
shard'lar sessizce **`-O0`** derlendi: yarış karesi 10 ms yerine 37 ms
(3.7x), ve bir saatlik ölçüm çöpe gitti.

Belirtisi profilde nettir: `parity8`, `x87_r2`, `fcompare` **ayrı fonksiyon
olarak** görünüyorsa shard'lar optimize edilmemiştir (optimize halde satır
içi olurlar). Betik artık `${OPT:--O3 -mcpu=native}` kullanıyor.

## 10. Bayrak canlılığı: ölçüldü, KAZANÇ YOK

Üretilen kodda `FLAGS_*` çağrılarının **%37.8'i `0x3f` maskesiyle altı
bayrağı birden** hesaplıyor, yalnızca %25.3'ü bedava. Sebep: canlılık analizi
`call`'ı "tüm bayrakları okur" sayıyor, yani bayraklar her çağrı sınırında
canlı kalıyor.

`call`'ı "bayrak okumaz" yapmak statik dağılımı ciddi biçimde düzeltiyor
(bedava %25.3 → %35.6, altı-bayrak %37.8 → %16.8) ama **kare süresine
etkisi yok**: 10.02 → 10.24 ms. Sebep: clang zaten okunmayan bayrak
yazmalarını kendisi eliyor; lifter'ın maskesi büyük ölçüde onunla mükerrer.

> İlk deneme bunun 3.3x yavaşlattığını gösterdi — o ölçüm `-O0` kazasıydı
> (yukarı bkz.), sonuç geçersizdi. Doğru derlemeyle kazanç sıfır.

**Sonuç:** bayrak emülasyonunu daha agresif budamak yol haritasından
çıkarıldı. Kalan maliyet çevirinin doğasında: konuk kayıtları bellekte,
konuk bellek erişimi `g_mem + ofset`, ve oyunun kendi çok konuşkan
render'ı (kare başına ~1250 çizim, ~62 bin COM çağrısı).

## 11. Yeni ortam anahtarları

| anahtar | işlev |
|---|---|
| `PACE60=1` | eski 60 Hz kare frenini geri getirir (varsayılan: fren yok) |
| `SPINSLEEP=<µs>` | oyunun 120 fps kare sınırlayıcısı (`sub_0057eae0`) dönme yerine uyur, son pay (varsayılan 200 µs) dönerek, uyku `kevent` `NOTE_CRITICAL` ile; `0` kapatır. Yarışta −1.2 W (bkz. `D3DX_HLE.md`) |
| `QPCWHO=1` | QueryPerformanceCounter çağıranlarının dökümü (tanı) |
| `PERFLOG=<dizin>` | gerçek oyun oturumu kaydı (kullanıcı oynarken); bkz. "Gerçek oturum kaydı" |
| `NOVSYNC=1` | vsync'i kapatır — YALNIZCA ölçüm için, görüntü yırtılır |
| `QUITFRAMES=<n>` | n kare sonra TEMİZ çıkar (PGO/ölçüm için) |
| `LDOPT=...` | `link.sh`'in son link satırına eklenir |
| `NOWIDE=1` | menüdeki 1920x1080 satırını gerçek 1600x1200 yapar |
| `ZSEQ=<kare>` | o karede SetRenderTarget / SetDepthStencilSurface / Clear / Z durumu sırasını basar |
| `NFSFULL=1/0` | tam ekranda / pencerede başlat (hatırlanan tercihi geçersiz kılar, değiştirmez) |
| `FSTOGGLE=<kare>[,...]` | o karelerde tam ekran geçişi (test) |
| `FSSIM=<kare>:<k\|m\|c>[,...]` | o karede sentetik girdi: `k` fn+F, `m` Ctrl+Cmd+F, `c` yeşil düğmeye tık (olay döngüsünden geçer) |
| `RESETAT=<kare>` | o karede cihaz kaybı üret; oyunun Reset yolunu test eder |
| `MOUSEAT=x,y[@kare,x,y]` | oyunun gördüğü imleç konumunu sabitle (imleç eşleme testi) |
| `NOFOCUS=1` | pencereyi arkada aç, odağı çalma (kullanıcı çalışırken test) |
| `NOSOUND=1` | ses çıkışı yok |
| `NORTFREE=1` | Reset'te eski render hedeflerini bırakmayı kapat (tanı) |
| `NORTRESET=1` | Reset'te hedefi/Z'yi geri tampona döndürme (eski davranış; araba bozulur) |
| `NORSRESET=1` | Reset'te render/sampler/aşama durumlarını varsayılana çekme (eski davranış) |
| `NOTEXZERO=1` | yeni dokuların yalnız 0. seviyesini sil, küpü hiç silme (eski davranış) |
| `FSFAKE=<kare>[,...]` | pencereye dokunmadan tam ekran geçişini oyunun gözünden taklit et (test) |

### Reset sonrası içi dışına dönük araba (2026-09-27) — ÇÖZÜLDÜ

Belirti: tam ekrana girip çıktıktan (ya da çözünürlük değiştirdikten) sonra
ana menüdeki araba içi dışına dönük/saydam (fren diskleri gövdeden görünüyor),
döner zemin kara. Bazen oluyor, bazen olmuyordu.

**Kök neden:** D3D9 `Reset` render hedefi 0'ı geri tampona, Z yüzeyini örtük
Z'ye döndürür; oyun buna güvenip ikisini bir daha kurmuyor. Bizde Metal
tarafında son hedef/Z olduğu gibi kalıyordu. Tam ekran geçişi cihaz kaybı
yolundan Reset'i **kare ortasında** yakalıyor: o an doku 7'ye Z'siz (NULL)
çizim yapılıyordu (ölçüldü, `com: Reset: bagli ...` satırları). Sonrasında
ana sahne Z testi olmadan çiziliyordu.

**Ölçüm:** aynı tam ekran + 8 çözünürlük değişimi senaryosu; eski davranış
(`NORTRESET=1`) 3/3 bozuk (parlaklık 29.97), düzeltme 7/7 sağlam (37.2).
İkiye bölme: yalnız hedef/Z sıfırlaması belirleyici.

**Tuzak (zaman kaybettirdi):** Bash kabuğu zsh; tırnaksız `$E` kelimelere
**bölünmüyor**. `E="NFSFULL=1 NORTFREE=1"` tek argüman olup
`NFSFULL="1 NORTFREE=1"` oldu, yani "karşılaştırma" koşuları aslında aynı
yapılandırmaydı ve hatanın rastgele olduğu sanıldı. Anahtarları ayrı argüman
olarak geçir.

### 32. çözünürlük değişiminde çökme (2026-09-27) — ÇÖZÜLDÜ

Kullanıcı serbest dolaşımda 720p↔4K arasında defalarca gidip gelince oyun
çöktü (`sub_005b9920`, adres 0x80). Logda tam **64 Reset** vardı.

**Kök neden:** küp doku kaydı (`g_cube_obj`) 64 girişle sınırlıydı ve her
değişim (iki Reset) 2 küp dokuyu yeniden yaratıyor: 2 + 32×2 = 66. Kayda
girmeyen küp "2B" sanılıyor, `GetCubeMapSurface` 2B arg sayısıyla dönüyor,
yığın kayıyor → çökme. Yarışta çevre haritası her kare çizildiği için orada
çıkıyor; menüde küp hiç çizilmediğinden görünmüyordu.

**Düzeltme:** kayıt bit haritası (COM nesneleri hiç bırakılmıyor, adres
yeniden kullanılmıyor → sınırsız ve O(1)). `MAXTEX` 4096 → 16384 (kullanıcı
oturumu 2504 doku yaratmıştı).

**Tekrar üretim:** Quick Race tarifi + yarışta `RESETAT` ile 35 cihaz kaybı
(6000'den 40 karede bir). Eski: 64. Reset'ten sonra aynı fonksiyon/adres/
yazmaçlarla çöktü. Yeni: 90 Reset, çökme yok, görüntü Reset öncesiyle aynı.
Ders: Reset başına yeniden yaratılan her şey için sabit boyutlu tablo = saatli
bomba; uzun oturum testini `RESETAT` listesiyle yap.

Aynı yolda düzeltilen iki D3D9 kuralı daha (gözle görülür fark ölçülmedi):
- Reset tüm render/sampler/aşama durumlarını varsayılana döndürür.
- Yeni doku içeriği tanımsız: artık **tüm** mip seviyeleri ve küpün 6 yüzü
  sıfırlanıyor. Menüde araba çevre haritası (256/128 küp RT) hiç çizilmiyor
  ama gövde onu örnekliyor (ps 265, aşama 0); eskiden küp hiç silinmiyordu ve
  bellek yeniden kullanılırsa çöp taşıyabilirdi (kasıtlı çöple doğrulandı:
  gövdede gürültü).

---

# Araba yansımaları (küp doku) — ÇÖZÜLDÜ

Kullanıcının kayıtlı ayarında `g_CarEnvironmentMapEnable=3` zaten AÇIKTI;
yansımalar isteniyordu ama yanlış çıkıyordu. Eski notlardaki "1 yapınca
çöküyor" durumu ise önceki oturumların küp-vtable düzeltmeleriyle kapanmış
(bu oturumda 2959 kare sorunsuz koştu).

**Ölçülen gerçek durum:** oyun çevre haritasını **6 küp yüzüne ayrı ayrı
çiziyor** (`SetRenderTarget` 6 farklı küp yüzeyiyle), sonra dokuyu aşama 0'a
bağlayıp örnekliyor. Bizde ise:

1. `CreateCubeTexture` **düz 2B** Metal dokusu yaratıyordu → altı yüz üst üste
   aynı yüzeye yazılıyordu.
2. Piksel gölgelendiricisinde tüm yuvalar `texture2d<float>` bildiriliyordu →
   küp doku bağlanınca Metal çizimi reddediyor
   (`validateCommonDrawErrors`).

**Yapılanlar:**
- `metal_create_texture_cube()` — gerçek `MTLTextureTypeCube`.
- Render hedefi artık `doku | ((yüz+1) << 16)`; render geçişi
  `colorAttachments[0].slice`'ı kuruyor. `touched[]` (temizle/yükle kararı)
  artık `(doku, yüz)` çiftine göre.
- `GetCubeMapSurface` yüz indeksini yüzey nesnesinde (+44) saklıyor.
- `metal_upload_texture_face()` — küp dokuda `replaceRegion` **dilim**
  ister; ayrıca depolama `Private` olamaz (oyun yüzlere `LockRect` ile de
  yazıyor → `_validateReplaceRegion` assert'i, 9. karede çökme).
- **Küp gölgelendirici varyantı:** D3D9'da `ps_1_x` doku TİPİNİ bildirmez,
  tip bağlamadan gelir. Bu yüzden her ps iki kez üretiliyor: normal ve
  `psid + PSCUBE` (256). Çizim anında bağlı doku küpse varyant seçiliyor.
  `ps_to_msl_ex(..., cubemask)` yuvaları `texturecube<float>` bildiriyor ve
  `SMP<k>(uv2, uv3)` makrosu 2B'de `.xy`, küpte `.xyz` örnekliyor.
- Yol boyunca: **`com_new` artık fazladan alanı sıfırlıyor.** Nesne alanları
  hep "0 = kurulmadı" varsayımıyla okunuyor; çöp değer okumak bir hata
  sınıfıydı (küp yüz indeksi bunu ilk yakalayan oldu).

**Maliyet: sıfır.** Dönüşümlü ölçüm (NOCUBE=1 ile düz 2B'ye karşı):
9.90 vs 9.89 ms, eşleştirilmiş +%0.0.

`dcl` örnekleyici tipi de artık okunuyor (`p->smptype[]`) — ölçüldü: oyunun
34 piksel gölgelendiricisinden **hiçbiri** küp bildirmiyor, hepsi `ps_1_x`.
Tuzak: tip **DCL token'ının** 27-30. bitlerinde, dst token'ında değil
(SM2+'da dst'in 28-30. bitleri kayıt tipinin üst bitleri; oradan okuyunca
her şey "4" çıkıyor).

| anahtar | işlev |
|---|---|
| `NOCUBE=1` | küp dokuyu düz 2B yapar (eski davranış, karşılaştırma için) |
| `CUBELOG=1` | küp yüzeyi render hedefi ve küp doku bağlamalarını basar |
| `SMPTYPE=1` | her ps için `dcl` örnekleyici tiplerini basar |

## Kalan grafik maddeleri: neyin ölçülebildiği, neyin ölçülemediği

Bu oturumda araba yansımaları çözüldü (yukarı bkz.). Diğer dört madde için o
zaman **nesnel bir sinyal bulunamadı**. 2026-10-17 güncellemesi: Windows
referansı (aşağıda "Windows referansıyla karşılaştırma") geldikten sonra
HUD dikişleri ve ışık çizgileri kapandı. Kalabalık ve TRAX hâlâ açık.

Çalışma raporu bu sahnede tamamen temiz: 0 desteklenmeyen komut, 0
çözülemeyen dolaylı çağrı, `[matris] bozuk=0`, `mip seviyeleri: 0 EKSIK`,
`kose shader: 0 bilinmeyen`, 0 düşen kare, Metal doğrulama katmanı sessiz.

- **HUD satır dikişleri — KAPANDI (2026-10-17): tasarım, hata değil.**
  Windows referans karesi (NFSRef, kare 5800, 1280x960) ile tur listesi 2x
  büyütülüp yan yana kondu: yatay satır ayraçları ve numara/isim sütunu
  arasındaki dikey dikiş Windows'ta da aynı yerde, aynı yoğunlukta var.
- **Kalabalık / karakterler düz levha.** Kemikli (skinned) mesh gerekir; bu
  sahnede kalabalık gölgelendiricisi HİÇ yüklenmiyor (`VSDUMP` çıktısında
  BLENDINDICES/BLENDWEIGHT yok, yalnızca 2 köşe gölgelendiricisi dökülüyor).
  Otomatik tuş sürücüsü arabayı hareket ettiremiyor (gaz tuşu DirectInput
  eylem eşlemesinden geliyor, DIK_UP çalışmıyor), yani kalabalığın olduğu
  yere programatik olarak ulaşılamıyor. **Oynayarak ulaşmak gerekiyor.**
- **Işık çizgilerinin aşırı kaplaması — KAPANDI (2026-10-17): fark yok.**
  Windows yarış karesiyle (5800) lamba bölgeleri yan yana kondu. Hale ve
  çizgi boyu/yoğunluğu aynı: >200 parlaklıkta piksel sayısı iki bölgede
  664'e 695 ve 146'ya 119. Trafik lambasının yanıp yanmaması faz farkı
  (Windows'un kendi iki koşusunda da değişiyor).
- **TRAX hayalet dikdörtgeni — hâlâ AÇIK.** Kutu, müzik başlarken geri
  sayımda çıkıyor (~kare 4100–4700). Windows referans setinde 3050 ile 5000
  arasında kare yok, kare 5000'de iki koşuda da kutu yok. Ayrıca port
  koşuları `NOSOUND=1`, yani müzik ve TRAX tetiklenmeyebilir. Doğrulamak
  için o aralıkta ve sesli bir referans karesi gerekiyor.

**Sonraki oturum için en verimli yol:** kullanıcıdan Windows'ta aynı
sahnelerin ekran görüntüsü, ya da oyunu o noktaya sürüp `DUMPFRAME` ile
kare aldırmak.

## Windows referansıyla karşılaştırma (NFSRef, 2026-10-17)

`tools/nfsref/` Windows'ta oyunun yanına konan bir ASI eklentisi (Ultimate
ASI Loader / `dinput8.dll`). Sanal saat (kare x 16667 µs), DirectInput
eylem eşlemesi üzerinden tuş sentezi ve `kare_NNNNN.bmp` dökümü yapıyor.
Port aynı koşullarda koşturuluyor: `NFSRES=1280x960 VCLOCK=fixed PACE60=1`,
aynı `AUTOSEQ`, aynı `DUMPFRAME` listesi. Windows menüleri deterministik.
Yarış kareleri Windows'un **kendi** iki koşusu arasında da değişiyor
(yükleme süresi, RNG): parlaklık Y 74.8'e karşı 67.1. Yarışta piksel farkı
değil, görsel öğelerin varlığı karşılaştırılmalı.

Bulunan ve düzeltilen iki gerçek hata:

1. **Küp doku yüz yüklemesi** (`com.c`, commit dbd21e0). `UnlockRect`
   küp dokuda yüzü yok sayıp her şeyi 0. yüze yazıyordu.
   `GetCubeMapSurface` yüz indisini yüzeye kaydetmiyordu. Menüdeki araba
   yansıması bu yüzden yanlıştı.
2. **Ekran dışı hedefler karenin ilk kullanımında temizleniyordu**
   (`metal.m`, ana geçişin yükleme eylemi). D3D9'da render hedefi kareler
   arasında **korunur**, yalnızca açık `Clear` temizler. Oyunun parlama
   zinciri bir geri besleme: RT4 her kare (0.47, 0.35, 0.67) tonuyla
   çarpılıp üstüne ekleniyor. Port her kare `MTLLoadActionClear` verdiği
   için geri besleme sıfırlanıyordu. Sonuç: pembe/mor haleler ve lavanta
   yansımalar yoktu. Artık hedef ancak bu çizim açık bir renk `Clear`'ı ise
   ve onun rengiyle temizleniyor, aksi halde `Load`. `OFFCLEAR=1` eski
   davranışı geri getirir (A/B için).
3. **Yarım piksel: D3D9 piksel merkezi tam sayıda, Metal'de +0.5'te**
   (`metal.m`, ana geçiş kodlayıcısında `setViewport`). Oyun tam ekran
   dörtgenlerini D3D9 kuralına göre −0.5 kaydırıyor. Kaydırmasız Metal'de
   iki sonucu vardı:
   - Her son işlem geçişi texel'lerin **arasından** örnekliyor ve bilinear
     filtre her geçişte yarım texel bulanıklık ekliyordu. Zincirde birkaç
     geçiş var, sonuç tüm görüntünün yumuşaması.
   - Son satır ve sütun hiç yazılmıyordu. Çeyrek çözünürlüklü parlama
     hedefinde bu, büyütmeden sonra alt ve sağ kenarda 4 piksellik
     kararmaydı (yarışta alt satır 96'ya karşı 49).

   Düzeltme: viewport kökeni (0.5, 0.5). Bu, D3D9 kapsama ve interpolasyon
   kuralını kaydırıcı eklemeden birebir verir. Port'un kendi blit
   dörtgenleri ayrı kodlayıcıda, onlar kaydırılmıyor. `HALFPX=0` eski
   davranış.

   Ölçüm, kenar ve son işlem zinciri olan karelerin keskinliğinin
   (ortalama |gradyan|) Windows'unkine eşitlendiğini gösteriyor:

   | kare | Windows | port eski | port yeni | piksel farkı eski → yeni |
   |---|---|---|---|---|
   | 1300 (2B, zincirsiz) | 1.31 | 1.31 | 1.31 | 1.09 → 0.10 |
   | 1650 menü | 3.22 | 2.51 | 3.20 | 6.79 → **0.70** |
   | 2000 menü | 2.98 | 2.27 | 2.98 | 6.62 → **0.31** |
   | 3050 menü | 3.16 | 2.57 | 3.14 | 6.75 → **0.44** |
   | 5800 yarış | 4.39 | 2.96 | 4.39 | 19.0 → 18.3 |

   Daha önce menüdeki ~6.7'lik farkı "döner tabladaki arabanın açısı"
   sanmıştım. **Yanlıştı**: farkın tamamı bu bulanıklıktı. Yarışta kalan
   fark, Windows'un kendi iki koşusu arasındaki farkla aynı türden (farklı
   rakipler, farklı ışık anı); keskinlik orada da birebir.
   (Not: `HALFPX` adı daha önce "ekranı kesen çizgiler" için denenip etkisiz
   bulunmuştu. O hata başkaydı; bu anahtar yeniden ve varsayılan açık
   eklendi.)

Sonuç olarak menü ortalama renkleri birebir tutuyor (R G B):

| kare | Windows | port |
|---|---|---|
| 1650 | 45 43 39 | 45 43 39 |
| 2000 | 44 42 37 | 44 42 38 |
| 3050 | 43 41 40 | 43 41 40 |

**Ölçüm tuzağı — `DUMPTEX` alfa karışımı:** eski doku dökümü dokuyu
alfa karışımlı `g_pso` ile çiziyordu. Alfası < 1 olan hedefler (RT4/RT5)
bu yüzden **siyah** görünüyordu ve yanlış bir "t2 siyah" teorisine götürdü.
Döküm artık ham blit kopyası alıyor, alfa istatistiğini de basıyor.
`DUMPOLD=1` eski yolu verir.

Yeni teşhis anahtarları:

| anahtar | işlev |
|---|---|
| `PSMSL=<id>[,..]` | ps'nin disasm + üretilen MSL'ini basar (küp varyantı `id+256`) |
| `DUMPFACE=<0..5>` | `DUMPTEX` küp dokuda hangi yüzü döksün |
| `SKIPDRAW=a-b,n` | çizim atlama artık aralık ve liste kabul ediyor |
| `OFFCLEAR=1` | ekran dışı hedefleri her kare temizle (eski, yanlış davranış) |
| `HALFPX=0` | D3D9 yarım piksel kaydırmasını kapat (eski, bulanık davranış) |
| `DUMPOLD=1` | eski (alfa karışımlı) doku dökümü |

## macOS uygulama paketi (`./pack.sh`)

Port artik cift tiklanabilir bir `.app` olarak paketleniyor: `./pack.sh`
`out/NFSU2.app` (44 MB), `out/NFSU2-dagitim/` ve `--zip` ile 15 MB'lik
`out/NFSU2.zip` uretir.  `run.sh` gelistirme yolu olarak aynen duruyor.

**`runtime/bundle.m`** paket ici davranisi topluyor.  Her yol once
"paketten mi calisiyoruz" diye bakar (`_NSGetExecutablePath` ->
`.../X.app/Contents/MacOS`), degilse hemen doner -- terminalden calistirma
hicbir sekilde degismedi.

* **Ayarlar.**  `run.sh`'in export ettigi degiskenler (`WINDOW`, `FIXMAT=1`,
  `FIXMAT_MODE=3`, `NOMOVIES`, `SNDSYNC`, `GPUVS`) paket icinde
  kendiliginden kurulur.  `ALARM` ozellikle **0**: `boot.c`'nin 20 sn'lik
  tani zaman asimi oyunu kullanicinin elinden alirdi.
* **Oyun verisi.**  Sirasiyla `NFSU2_DATA`, daha once secilmis yol
  (NSUserDefaults), paketin ici, `.app`'in yani ve bir ust dizini, ev
  dizinindeki birkac aday; bulunamazsa NSOpenPanel ile kullaniciya sorulur
  ve secim saklanir.
* **Log.**  Finder'dan acilinca stderr `/dev/null`'a gider ve elde hicbir
  tani kalmaz; `~/Library/Logs/NFSU2/nfsu2.log`'a yaziliyor (bir onceki
  kosu `nfsu2-onceki.log`).  stderr bir tty ise dokunulmuyor.

### Dagitim: iki sessiz engel

Ikisi de "calisiyor" gorunen ama baska makinede oyunu hic acmayan turden:

1. **`minos 26.0`.**  `-mmacosx-version-min` verilmezse ikili derleyen
   makinenin OS surumuyle damgalanir.  macOS 26'dan eskisinde uygulama
   hic baslamaz ("daha yeni bir surum gerekiyor").  Taban **11.0**'e
   cekildi; kullanilan hicbir API'de erisilebilirlik uyarisi yok.
2. **`-mcpu=native`.**  Bu makine M1 Pro oldugu icin `apple-m1` cozuluyor
   ve sorun gorunmuyordu; port yeni bir Mac'te derlenirse uretilen ikili
   M1'de gecersiz komutla coker.  `MCPU` varsayilani **`apple-m1`** yapildi
   (M1'de olculen fark sifir; `MCPU=native` ile eski davranis).

Ikisi de `build.sh` ve `link.sh`'te varsayilan.  `$OPT` uzadigi icin
`build.sh`'teki `xargs -I{}` yine "command line too long" verdi; `$OPT`
artik komut satirina gomulmuyor, ortamla gecirilip ic kabukta genisliyor.

**Gatekeeper.**  Ad-hoc imza notarizasyon degildir: indirilen paket icin
`spctl` **rejected** verir (olculdu).  Kullanicinin
`xattr -dr com.apple.quarantine` calistirmasi gerekiyor; `OKUBENI.txt`
bunu ilk adim olarak yaziyor.  Apple Developer hesabi varsa
`SIGN_ID="Developer ID Application: ..." ./pack.sh` gercek sertifikayla
imzalar.

**Veri paketin ICINE degil YANINA.**  `--with-data` oyunu `.app`'in
yanindaki klasore kopyalar.  Bundle'in icine koymak iki seyi bozardi:
kayitli oyunlar (`memcard/`) imzali bir pakete yazilamaz ve yazma imzayi
gecersiz kilar.  Uygulama zaten `.app`'in yanini tariyor.

### Kisisel tek parca uygulama (`./pack.sh --embed`)

`./pack.sh --embed` (ve `--embed --hd`) oyun verisini paketin ICINE
(`Contents/Resources/Game`) gomup `out/kisisel/` altina kurar (~1.5 GB;
`cp -Rc` APFS klonu, ayni diskte ek yer kaplamaz).  **Oyunun kendisini
icerdigi icin dagitilamaz**; betik `--zip` / `--with-data` ile birlikte
calismayi reddeder ve dagitim klasoru uretmez.  MOVIES (kod cozucu yok) ve
Windows eklentileri (SCRIPTS, dinput8.dll, server.*) gomulmez.

Imzali bir paketin icine yazmak imzayi bozar, bu yuzden `fileio.c`'ye bir
**yazma koku** eklendi (`fileio_set_write_root`): yazma / olusturma / silme
`~/Library/Application Support/NFSU2 Kayitlar`'a gider, okuma once oraya
sonra oyun kokune bakar, dizin listesi ikisinin birlesimidir.  Yalnizca
veri paketin icinden bulundugunda (ya da `NFSU2_SAVEDIR` verilince) etkin;
terminalden calistirma ve verisi disarida olan paket eskisi gibi tek kokte
kalir.  Gomulu veri, kayitli klasor tercihinden once gelir.

Dogrulama: paket proje agacinin disina kopyalanip `AUTOSEQ` ile yarisa
sokuldu (6000 kare, 0 cozulemeyen cagri; acilamayan 12 dosya normal kosuyla
ayni: filmler ve oyunun kendi yoklamalari).  Oyun kayit klasorunu dis
dizinde yaratti; kosudan sonra `codesign --verify` gecerli, paket icinde
degisen dosya yok.  Gercek profil kaydetme/yukleme denenmedi.

### `.app` olmak icin gereken davranislar

`metal_run_main_loop` olay dongusunu elle surdugu icin AppKit'in normalde
`[NSApp run]` icinde yaptigi isler yapilmiyordu:

* **Menu cubugu yoktu** -> Cmd-Q, Cmd-H, Cmd-M calismiyordu.  Menu eklendi;
  ayrica Cmd'li tuslar artik oyuna degil `[NSApp sendEvent:]`'e gidiyor.
* **`[NSApp finishLaunching]` cagrilmiyordu** -> AppleEvent isleyicileri
  hic kaydedilmiyor, uygulama "Cik" komutuna (Dock > Cik, oturum kapatma)
  yanit vermiyor, surec asili kaliyordu.
* **Run loop pompalanmiyordu** -> yalnizca `nextEventMatchingMask`
  cagriliyordu; AppleEvent'ler olay kuyrugundan degil run loop
  kaynaklarindan gelir.

`applicationShouldTerminate:` **`NSTerminateCancel`** donuyor: `g_should_quit`
kuruluyor, cizim dongusu cikiyor, `main()` raporu yazip sonlaniyor.
**`NSTerminateLater` denendi ve kullanilamaz** -- AppKit onayi beklerken
kendi run loop'unu suruyor, bizim dongumuz hic geri donmuyor, kapanis
asiliyor.  Cancel'in tek yan etkisi oturum kapatmada bir uyari; uygulama
zaten hemen ardindan kendi kendine cikiyor.

Yol boyunca cikan gercek hata: **fare Y'si `720.0 - pt.y` ile sabit
yaziliydi.**  1080p secilince (menuden secilebilir hale geldiginden beri)
imlec ekranin disina dusuyordu.  Pencerenin icerik yuksekligine baglandi.

Ayrica: `NSHighResolutionCapable` olmadan pencere 1x cizilip WindowServer
tarafindan buyutulurdu; tam ekranda `kCAGravityResizeAspect` en-boy oranini
koruyor (5:4 goruntu genis ekranda yayilmiyor).

**Olcum.**  Tasinabilir bayraklarla tam yeniden derleme sonrasi 900 kare
7.91 sn -- degisiklik oncesiyle ayni (7.9 sn).  Baslik ekraninda 2077 kare
/ 17.6 sn (118 fps), sanal saat 0.998, 0 desteklenmeyen komut, 0
cozulemeyen dolayli cagri.  439 ISA + 15 fonksiyon testi geciyor.

## Retina cozunurlugu: bulaniklik ve `NFSNATIVE`

**Belirti.**  Oyun en yuksek kipte (1280x960) calisirken goruntu Windows
ekran goruntulerine gore belirgin bulanikti; en cok yazilarda, farlarda ve
jant tellerinde.

**Olculen kok neden.**  Pencere icerigi oyunun cozunurlugu kadar NOKTA,
`CAMetalLayer.drawableSize` ayni sayida PIKSEL yapiliyordu; `contentsScale`
1.0 kaliyordu.  14" MacBook Pro'da (1512x982 nokta, 2x) ayri bir sinama
programiyla olculdu: 1280x960 noktalik icerik ekrana sigmadigi icin
1280x918'e kirpiliyor, arka alan 2560x1836 piksel oluyor ve 1280x960'lik
yuzey oraya ~1.91x bilinear buyutuluyor -- her piksel yeniden orneklenip
yumusuyordu.  (Build'den ya da `.app`'ten calistirmak fark etmiyordu;
ciplak ikili de 2x olcekli pencere aliyor.)

**Cozum.**
* `win_apply_size` (metal.m): pencere icerigi = piksel / ekran olcegi,
  `contentsScale` acikca ayarlaniyor.  Ekrana sigmiyorsa oran korunarak
  KUCULTULUYOR (buyutme asla).  Log satiri:
  `cizim WxH piksel, pencere wxh nokta -> 1:1, yeniden ornekleme YOK`.
* `metal_native_size` + `res_pick` (com.c): oyunun istedigi cozunurluk,
  indirgenmis oranin TAM kati secilerek ekranin gercek piksel sayisina
  cikariliyor.  Bu Mac'te 1280x960 -> **2448x1836**, `aspfix=1.0000`;
  kirpma uzayi duzeltmesi devreye girmiyor, geometriye dokunulmuyor.  Son
  islem render hedefleri mevcut NFSRES yoluyla ayni boyuta cikiyor.
  `NFSNATIVE=0` kapatir, `NFSRES` pinler, `NFSMAXPIX` (varsayilan 6 MP)
  5K/6K ekranlarda ust sinir.
* CreateDevice ve **Reset ayni kurali** kullaniyor; Reset eskiden ham
  degeri geciriyordu (oyun icinden cozunurluk degisince duzeltme geri
  alinirdi).  Gecersiz kilma yoksa `g_req`/`aspfix` sifirlaniyor.
* `GetDisplayMode` gercek boyutu degil oyunun sectigi boyutu bildiriyor:
  ayar ekrani bu degeri kendi tablosunda ariyor, 2448x1836 orada yok.
* Fare: on yuz imleci DirectInput GORELI hareketiyle suruluyor
  (`GetCursorPos` hic cagrilmiyor, `GetClientRect` sabit 1280x720).
  Pencere artik oyunun cozunurlugunden bagimsiz oldugu icin deltalar
  goruntunun kapladigi alana gore olcekleniyor; mutlak esleme de oyunun
  uzayina cevrildi.  Gercek fareyle test edilmedi.
* Yer tutucu pencere (CreateDevice'tan once) 1280x960 oraninda native
  aciliyor; en yaygin durumda pencere ziplamiyor.

**Tuzak: baslik cubugu tahmini.**  Ilk surumde kullanilabilir yukseklik
tahminle (menu + 28 nokta baslik) hesaplandi; centikli ekranda baslik
cubugu **32 nokta**.  3 noktalik fark pencereyi 921 -> 918'e kirpti ve
yeniden ornekleme geri geldi.  Artik alan ana thread'de AppKit ile
olculuyor (`scr_cache`; `metal_early_init` guest thread'den once cagriliyor).

**Olcum (M1 Pro, 120 Hz ekran, en yuksek grafik ayarlari).**  `AUTOSEQ` ile
Quick Race'e (Resort Loop) girildi; akis deterministik, iki kosu kare kare
ayni ekranlardan geciyor:

| bolum | native kapali (1280x960) | native acik (2448x1836) |
|---|---|---|
| menuler, 3000-3499. kare | ort 8.34 ms, p95 8.48 | ort 8.34 ms, p95 8.50 |
| yaris, 5000-6999. kare (~1300 cizim/kare) | ort 10.47-10.64 ms, p95 11.5-11.8 | ort 10.56-10.77 ms, p95 11.6-11.9 |
| GPU mesgul (tum kosu) | ort 2.00 ms, en fazla 3.95 | ort 4.88 ms, en fazla 9.35 |

Kare suresini CPU (lifted kod) belirliyor; 3.66x piksel kareyi uzatmadi.
**Olcum tuzagi:** ilk karsilastirmada native'de 170-220 ms sicramalar
goruldu -- hepsi `DUMPFRAME` karelerine denk geliyordu (PNG yazimi, piksel
sayisiyla olcekleniyor).  Olcumde dokum karelerini disarida birakin.

**Sinirlar.**  Arayuz fontlari sabit cozunurluklu dokular: ikinci ornekleme
kalktigi icin keskinlesiyorlar ama dokunun kendi detayini asamazlar.  Daha
zayif GPU'lu Mac'lerde olculmedi; GPU suresi kare butcesini asarsa
`NFSNATIVE=0` ya da `NFSMAXPIX` ile dusurulebilir.

**Test notu.**  Test pencereleri odak aliyor; oyunun ayarlari
`~/.nfsu2_registry`'de.  Bu calisma sirasinda `g_RacingResolution` bir kez
3'ten (1280x960) 4'e (1280x1024) kaydi ve kontrollu kosularda tekrar
uretilemedi (en olasi neden test penceresine gelen gercek klavye girisi).
Cok kosulu testlerden once dosyayi yedekleyin.

## Gerçek oturum kaydı (`PERFLOG`, `tools/oturum.sh`)

Otomatik koşular (`AUTOSEQ` yarışı) birebir A/B için; bu kayıt kullanıcının
kendi oynadığı oturumda (hızlı sürüş, efektler, kaza, menü) yükün **nerede ve
ne zaman** oluştuğunu görmek için.

```bash
./tools/oturum.sh            # normal sürüm; ./tools/oturum.sh hd
python3 tools/oturum_analiz.py ~/nfs_oturum/<tarih>   # -> <dizin>/rapor.md
```

Betik powermetrics'i (CPU/GPU watt, termal basınç; parola bir kez) ve oyunu
`open --env PERFLOG=<dizin>` ile başlatır, oyundan çıkınca kapatır.
`runtime/perflog.c` yazar:

| dosya | içerik |
|---|---|
| `konuk.csv` | konuk kare başına duvar/CPU süresi, kare sınırlayıcı uykusu (boş pay), Present beklemesi, COM/Win32 çağrıları, VS/PS/PSO derleme, doku yaratma/yükleme |
| `render.csv` | ana thread kare aralığı, `nextDrawable` beklemesi, kodlama süresi, çizim/köşe, düşen kare |
| `gpu.csv` | kare başına GPU süresi |
| `saniye.csv` | süreç enerjisi (`ri_energy_nj`, P çekirdek), komut/çevrim (IPC), termal durum, bellek |
| `thread.csv` | saniyede bir thread başına kullanıcı/sistem süresi |
| `ornek.bin` | 1 ms'de bir konuk, 4 ms'de bir ana thread: PC + çerçeve zinciri (`thread_suspend` → durum → `resume`; askıdayken kilit/malloc yok) |
| `ekran/` | 2 sn'de bir ~480 px JPEG (GPU kopyası, arka planda yazılır) |

Ek yük (120 fps yarış, `ps -M`): konuk thread +%1.6, süreç +%5 (çoğu
örnekleyicinin kendisi); fps, en uzun kare ve sapma değişmiyor.
`task_gpu_utilisation` 0 dönüyor: GPU watt yalnızca powermetrics'ten.


## GPU enerjisi ve bellek trafiği (2026-10-18)

Hedef: 120 fps ve özgün görüntü korunarak watt düşürmek. Yarışta (120 fps)
GPU ~4.9 W, sistemin ~8.8 W'ının yarısından fazlası.

### Ölçüm: neden yeni yöntem gerekti

- **GPU ms/kare iş ölçüsü değil.** macOS GPU saatini yüke göre ayarlıyor (DVFS):
  iş azalınca saat düşüyor, aynı kare daha UZUN sürebiliyor (bellek trafiğini
  kesen bir değişiklikte 5.3 → 5.8 / 7.6 ms ölçüldü).
- **Ayrı koşuların enerjisi karşılaştırılamaz.** Aynı ayarla üç koşu 30.8 /
  34.9 / 40.0 mJ/kare verdi (DVFS durumu + sistemin geri kalanı).
- **Sudo'suz sayaçlar** (IOReport, `runtime/metal.m` `ior_read`):
  - `Energy Model / GPU Energy` (nJ): GPU çekirdeğinin modellenmiş enerjisi
    (powermetrics'in "GPU Power"ı). **DRAM'i içermez.**
  - `AMC Stats / Perf Counters`: bellek denetleyicisinde ajan başına bayt.
    `GFX RD/WR` = GPU'nun bellek sistemine okuma/yazması, `GFX DCS RD/WR` =
    bunun DRAM'e giden kısmı. DVFS'ten bağımsız bir iş ölçüsü.
  - SMC `PSTR` (sistem toplam gücü) de sudo'suz okunuyor ama saniyede bir
    güncelleniyor; A/B için kaba (araç: scratch `smc.c`, kalıcı değil).
- **Koşu içi A/B: `ABTEST=AD[,N[,ATLA]]`** (`GPUWIN` penceresinde). Pencere N
  karelik dilimlere bölünür (varsayılan 120; kısa dilim komşu dilimleri daha iyi
  eşleştiriyor, ölçümlerde `32,4` kullanıldı). Çift dilimde özellik açık, tek
  dilimde kapalı. Dilimin ilk ATLA karesi atılır (kuyrukta önceki kipin işi).
  Komşu dilim çiftlerinin farkı ortalama ± standart hata olarak raporlanır.
  Özellik kendi anahtarını `abt_off("AD")` ile sorar (`SCRREDIR`, `ALFAVAR`).
  `GPUWIN` raporu da artık enerji ve MB/kare veriyor.

```bash
# yarış, gerçek çözünürlük, 60 Hz sabit (AUTOSEQ: Quick Race tarifi, gaz basılı)
env FIXMAT=1 FIXMAT_MODE=3 NOMOVIES=1 SNDSYNC=1 GPUVS=1 WINDOW=1 ALARM=0 \
    NOFOCUS=2 NOSOUND=1 NFSFULL=0 MOUSEAT=5,5 PACE60=1 \
    AUTOSEQ="1000:0x1C,1350:0x1C,1700:0x1C,2050:0x1C,2400:0x1C,2750:0x1C,3100:0x1C,3700:0x1C,4000:0x1C,4700:0xC8:5300" \
    QUITFRAMES=10000 GPUWIN=6200,9800 ABTEST=ALFAVAR,32,4 \
    out/nfsu2_boot "<SPEED2.EXE>" 2>&1 | grep -a "ABTEST\|PENCERE"
```

### Sonuçlar (normal sürüm, yarış, 3024x1836, 60 fps, 56 dilim çifti)

| özellik | GPU enerjisi mJ/kare | GPU okuma / yazma MB/kare | DRAM okuma / yazma MB/kare | karar |
|---|---|---|---|---|
| ALFA TESTİ VARYANTI (`ALFAVAR`) | **−6.32 ± 0.48 (−%17)** | −2.8 ± 1.1 / −0.3 ± 0.8 | −1.8 ± 1.7 / +0.8 ± 1.7 | varsayılan açık (HD: −%27) |
| EKRAN YÖNLENDİRME (`SCRREDIR`) | −0.02 ± 0.88 | −10.7 ± 1.1 / −10.1 ± 0.8 | **−20.4 ± 1.9 / −21.0 ± 1.8** | varsayılan açık |
| GEÇİŞ YÜKLEMESİ (DontCare / karalama Z) | +0.6 ± 0.8 | +0.9 ± 1.1 / +0.6 ± 0.6 | +1.6 ± 1.8 / +1.2 ± 1.4 | elendi, kod kaldırıldı |
| SON İŞLEM ZİNCİRİ (`FBZINCIR`), 2 koşu | −1.07 ± 0.53 / +2.39 ± 1.44 (ort. ≈ −0.7 ± 0.5) | −11.1 / −9.6 okuma, −5.8 / −4.2 yazma | **−12.6 / −9.8 okuma, −12.9 / −10.8 yazma** | varsayılan açık |

GPU süresi (DVFS'li, yalnız bilgi): `ALFAVAR` açık 5.28 / kapalı 6.08 ms/kare.

### ALFA TESTİ VARYANTI (`ALFAVAR`)

Çevrilen her parça gölgelendiricisi alfa testini kendisi yapıyordu:
`if (res.a < P.aref) discard_fragment()`. Test kapalıyken (aref 0) hiçbir parça
atılmaz, ama gölgelendiricide `discard` olduğu için Apple GPU çizimi opak
saymıyor ve **gizli yüzey elemesi (HSR) kapanıyor**: sonradan örtülecek parçalar
da gölgeleniyor. Şimdi her ps (ve sabit işlevli `fmain`) bir işlev sabitiyle
(`kAT`) iki kez özelleştiriliyor; aref 0 olan çizim `discard`'sız varyantla
çiziliyor (boru hattı anahtarında bit 52). Fark yalnız `res.a < 0` üreten
parçada olabilirdi; D3D9'da alfa testi kapalıyken o parça zaten yazılır, yani
yeni varyant D3D9'a daha sadık.

Doğrulama: `ALFAVAR=2` testsiz varyantta "atılacak" parçayı GPU'da bir sayaçla
sayar (parça tampon 6) ve macenta boyar. Tüm koşu (9000 kare, menü + yarış)
sonucu **0 parça**. `ALFAVAR=3` pozitif kontrol (alfa testli çizimler de tanı
varyantıyla) 2.98 milyar parça saydı.

**HD sürümü:** kazanç daha büyük (−30.1 ± 1.7 mJ/kare, −%27). `ALFAVAR=2`
sayacı HD'de önce 12.6 milyon parça farkı buldu. Gölgelendirici başına sayım
(sayaç dizisi ps kimliğiyle) kaynağı gösterdi: neredeyse tamamı sabit işlevli
`fmain`, sebep negatif köşe rengi alfası. D3D9 köşe rengini [0,1]'e kırpar
(aşağıda RENK KIRPMA); kırpmayla HD'de de **0 parça**, iki sürümde açık.

### RENK KIRPMA (`RENKSAT`)

D3D9'da ps_1_1 – ps_2_0 köşe rengini (v0/v1) [0,1]'e kırpılmış görür. Microsoft
belgesi "8 bit unsigned, range (0, +1)" ve "saturated before use by the shader"
diyor. Bizim köşe gölgelendiricimiz oD0/oD1'i kırpmadan iletiyordu. Tanı
sayacı yarış + menüde **1.18 milyar parçanın** [0,1] dışı renkle geldiğini
gösterdi; çoğu araba boyası (ps 265, küp varyantı), sonra ps 7 ve `fmain`.

Kırpma iki yerde yapılıyor:
- köşe başına: `vsemu.c` `vs_to_msl` ve `com.c` CPU yolu, vs < 3.0. Dönemin
  donanımında renk enterpolatörleri zaten [0,1] taşıyordu.
- parçada: `kCS` işlev sabiti; kayan nokta taşmaları ve gözden kaçan yollar için.

Windows referansı (1280x960, aynı tuş dizisi): menü kareleri değişmedi, Windows'a
uzaklık öncesiyle aynı (0.1 – 0.5). Yarış kareleri Windows'ta koşudan koşuya
değiştiği için orada kıyas yok. Bu bir sadakat düzeltmesi: D3D9 sözleşmesine
uyuyor ve alfa varyantını HD'de birebir yaptı. `RENKSAT=0` eski davranış.

**Macentayı görüntüde aramak yarışta kördür:** son işlem renk tablosu (ps 26/28, LUT) macentayı başka renge
çeviriyor; menü karesi dışında hiç macenta görünmedi. Sayaç bu yüzden var.

### EKRAN YÖNLENDİRME (`SCRREDIR`)

Yarış (ve menü) karesi sahneyi ekrana çiziyor, sonra aynı boyutta #2'ye
kopyalıyor (son işlem #2'yi okuyor), ekranı en sonda tam ekran bir dörtgenle
baştan yazıyor. Kopya 22 MB okuma + 22 MB yazma, 0.59 ms/kare (PASSPROF).
Koşullar sağlanınca sahne doğrudan #2'ye çiziliyor ve kopya atlanıyor.
Koşullar: kopya ekranı okuyan ilk blit; hedefi ekranla aynı boyut ve biçimde;
kopyadan önce #2 ne örnekleniyor ne hedef oluyor; kopyadan sonra ekrana ilk
dokunan çizim ekranı tamamen eziyor (TAM EKRAN KAPLAMA kuralı). Bu durumda #2
kopyanın vereceği içeriğin aynısını alıyor: aynı çizimler, boyut, biçim, Z.

Doğrulama: `SCRREDIR=2` kopyadan sonraki ilk ekran geçişini macentayla
temizler. Kaplama yanlışsa macenta son görüntüye sızar; bu test son işlemden
SONRA ekrana yazılanı sınadığı için kör değil. 16 karede 0 macenta. Görüntü
ayrıca gözle kontrol edildi. HD'de ekran çok örnekli ve G-tamponlu olduğu için
orada kapalı.

Kazanç DRAM'de: kare başına −41 MB (120 fps'te −4.9 GB/s). GPU çekirdek
enerjisi değişmedi: kopya bellek işi, gölgelendirici işi değil.

### UV ÖLÇEĞİ (`UVFIX`) — yerel çözünürlükte son işlem bulanıklığı (2026-10-19)

NFSRES / Retina'da oyun 1280x960 sandığı için çiziyor; tam ekran hedefler
(#2/#3/#4, ekran) ise 3024x1836. Son işlem dörtgenleri D3D9 yarım teksel
kuralıyla `u = (x + 0.5) / 1280` eşlemesini taşıyor (ölçüldü: u 0.0003906 ..
1.0003906 = 0.5/1280, v 0.0005208 .. 1.0005208 = 0.5/960). Gerçek hedefte bu,
piksel merkezini teksel merkezinden (0.68, 0.46) teksel kaydırıyor. Her tam
çözünürlüklü geçiş görüntüyü kaydırıp bilinear ile yumuşatıyordu; ana görüntü 4
geçişten geçiyor (renk tablosu zinciri + ekrana kopya). 1280x960'ta (Windows
kıyas çözünürlüğü) eşleme birebir olduğu için bu hiçbir karşılaştırmada
görünmemişti.

Düzeltme: oyunun niyetindeki eşleme gerçek boyutta kuruluyor. Köşe
gölgelendiricisi kalıba uyan koordinat kümesine `0.5/Wgerçek − 0.5/Woyun`
ekliyor, yani `u = (x + 0.5) / Wgerçek`. Koşullar:
- dörtgen hedefi tam kaplıyor (NDC ±1);
- hedef ölçekli (oyun boyutu ≠ gerçek);
- kümenin köşe UV'leri tam bu kalıpta.

Kalıba uymayan kümeye (çok dokunuşlu bulanıklık ofsetleri, 640x480 hedefler)
dokunulmuyor. Kare başına ~2.5 çizim.

Sonuç (aynı tuş dizisi, yerel çözünürlük; netlik = ortalama parlaklık
gradyanı):

| kare | önce | sonra |
|---|---|---|
| 1650 (3B menü, araba) | 1.367 | 1.637 (+%20) |
| 2000 | 1.259 | 1.542 (+%22) |
| 3050 | 1.359 | 1.590 (+%17) |
| 5000 / 6200 (yarış) | 2.33 / 2.19 | 3.17 / 2.99 |
| 1300 (2B menü, son işlem yok) | 0.569 | 0.569 |

Gözle: kaputun karbon dokusu, jant telleri ve fren diski delikleri artık keskin.
`UVFIX=0` eski (bulanık) davranış.

### SIRALI DOKU YÜKLEME (`TEXORD`) — doku yarışı (2026-10-19)

Oyun bazı dokuları her karede yeniden yazıyor: son işlem renk tabloları #8 ve
#9, 5467 karede 5467 yükleme (`TEXUPSAY=1` sayar). Yükleme `replaceRegion` ile
anında dokuya gidiyordu. Konuk thread kare N+1'i kaydederken ana thread kare
N'i kodluyor ya da GPU onu çiziyor. Kare N bu yüzden bazen kare N+1'in
tablosunu, hatta yarım yazılmış tabloyu görüyordu. D3D9'da her çizim
gönderildiği andaki içeriği görür.

Belirtisi şuydu: aynı karede aynı çizimi iki kez kodlamak bile farklı sonuç
veriyordu (son işlem zinciri doğrulamasında 0.6 M piksel, en çok 2/255).

Düzeltme: bir dokunun ikinci ve sonraki yüklemeleri çizim listesine sıralı kayıt
olarak giriyor. Veri karenin yükleme alanına kopyalanıyor (çift tampon, 8 MB),
kodlamada çizimlerle sıralı bir blit (tampondan dokuya) oluyor. İlk yükleme
(yükleme ekranı, henüz kullanılmamış doku) eskisi gibi anında. Atlanan
(kodlanmayan) karenin bekleyen yüklemeleri, tampon yeniden kullanılmadan önce
anında uygulanıyor. Kare başına ~2.3 yükleme, taşma 0. Sonra aynı çizim iki kez
kodlanınca **0 piksel fark**. `TEXORD=0` eski davranış.

### SON İŞLEM ZİNCİRİ (`FBZINCIR`) — üç geçiş tek geçişte (2026-10-19)

Son işlem üç tam ekran geçişle renk tablosu uyguluyor: `#3 = ps28(#2)`,
`#2 = ps27(#3)`, `#3 = ps26(#2)`. Her adımın girdisi bir öncekinin aynı
pikseldeki çıktısı. Ölçüldü (`ABTEST=SKIPPS` + `SKIPPS=26,27,28`): üç geçiş GPU
enerjisinin ~%16'sı, DRAM'de ~33 MB/kare.

Zincir şimdi iki hedefi birden bağlayan tek bir geçişte çiziliyor. Her adım
girdisini dokudan değil karo belleğinden okuyor (`[[color(n)]]`, Apple GPU'da
çizim sırasıyla). Gölgelendirici varyantı (`vsemu.c` `ps_to_msl_fb`) aşama 0
örneğini karo bellek girdisiyle değiştiriyor; arka planda derleniyor, hazır
olana kadar eski yol kullanılıyor.

Koşullar (kayıt başına):
- tek dörtgen, hedefi tam kaplıyor;
- Z, karışım ve alfa testi yok;
- aşama 0 dokusu diğer zincir hedefi, aynı boyut ve biçimde;
- UV teksel merkezine 1:1 (UV ÖLÇEĞİ sonrası);
- ps aşama 0'ı tek yerde `TCRD(0)` ile örnekliyor;
- diğer aşamalar zincir hedeflerini okumuyor;
- ardışık kayıtlar iki hedef arasında gidip geliyor.

Doğrulama: `FBZINCIR=2` zinciri eski yoldan çiziyor, aynı girdiyle birleşik
yolu karalama dokularına çiziyor ve ikisini GPU'da piksel piksel
karşılaştırıyor. Sonuç: 5454 zincirde **0 fark**, hem son çıktıda hem ara
hedefte. `FBZCTRL=1` yardımcıyı normal boru hattıyla, `FBZCTRL=2` iki kez
çalıştırıp kendisiyle karşılaştırıyor; determinizm testi bu ikisi.
Doku yarışı düzeltilmeden önce sonuç 1–3/255'lik farklar veriyordu. O
farkların kaynağı zincir değil, yukarıdaki yarıştı.

Kazanç: DRAM −23 MB/kare (iki koşu); GPU çekirdek enerjisi ölçüm gürültüsü
içinde (ort. ≈ −0.7 ± 0.5 mJ/kare). Kare başına ~1.3 zincir (yarış + menü).

### Elenenler

- **GEÇİŞ YÜKLEMESİ:** tam ekran kaplayan çizimle başlayan geçişte renk
  `Load` → `DontCare`, Z kullanmayan geçişte gerçek Z yerine karalama derinlik.
  Zehir testiyle doğruydu ama hiçbir ölçüde etkisi yok (tablo). Apple GPU tam
  örtülen karonun yüklemesini HSR ile zaten atlıyor, dokunulmayan Z'yi de
  taşımıyor. Kod kaldırıldı; kaplama hesabı (`com.c` → `metal_set_next_cov_clip`)
  EKRAN YÖNLENDİRME için duruyor.
- **Z SAKLAMA:** son kullanımdan sonra Z'yi `DontCare` ile saklamamak. Oyun
  cihazı `D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL` olmadan açıyor (sunum
  parametreleri: takas 1, Z D24S8, bayrak 0; artık açılışta loglanıyor). Yani
  örtük Z kareler arası korunmalı; kare içinde sonradan temizlenen Z 0/kare
  çıktı. Uygulanamaz, kod kaldırıldı.

Yeni anahtarlar:

| anahtar | işlev |
|---|---|
| `ABTEST=AD[,N[,ATLA]]` | koşu içi A/B (yukarıda) |
| `SCRREDIR=0/2` | ekran yönlendirmeyi kapat / zehir testi |
| `ALFAVAR=0/2/3` | testsiz varyantı kapat / sayaçlı tanı (ps başına; ayrıca [0,1] dışı köşe rengi sayımı) / tanının pozitif kontrolü |
| `RENKSAT=0` | köşe rengi kırpmasını kapat (eski, D3D9'a aykırı) |
| `UVFIX=0` | ölçekli hedeflerde yarım teksel düzeltmesini kapat (eski, bulanık) |
| `TEXORD=0` | tekrar yüklenen dokuları anında yaz (eski, yarışlı) |
| `TEXUPSAY=1` | çıkışta doku başına yükleme sayısı (en çok 12) |
| `FBZINCIR=0/2` | son işlem zinciri birleştirmesini kapat / birebirlik doğrulaması |
| `FBZCTRL=1/2` | (`FBZINCIR=2` ile) yardımcıyı normal boru hattıyla çalıştır / kendisiyle karşılaştır |
| `SKIPPS=…` + `ABTEST=SKIPPS` | o ps'lerin bedelini koşu içinde ölç (yalnız KAPALI dilimlerde atlar) |
