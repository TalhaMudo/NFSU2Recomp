# Çeviricinin (lifter) ürettiği kodun verimliliği

Amaç: x86 → C statik çevirisinin ürettiği kodun maliyetini **ölçmek**, kaynağını
**komut düzeyinde** bulmak ve çeviriciyi iyileştirmek. Her adım ayrı dizinde
üretilir; temel kod (`gen/all`) değişmez, adımlar `diff` ile karşılaştırılabilir.

| dizin | ikili | içerik |
|---|---|---|
| `gen/all` | `out/nfsu2_boot` | temel (üretim) |
| `gen/prof` | `out/nfsu2_prof_boot` | temel + her x86 komutu yorum (`COMMENTS=1`), `-gline-tables-only` — yalnız profil |
| `gen/opt` | `out/nfsu2_opt_boot` | + FLAGABI |
| `gen/opt2` | `out/nfsu2_opt2_boot` | + FLAGABI + X87OPT |
| `gen/opt3` | `out/nfsu2_opt3_boot` | opt2 + INLINE=50 (olumsuz sonuç, karşılaştırma için) |
| `gen/opt4` | `out/nfsu2_opt4_boot` | opt2 + RETEIP=0 |
| `gen/opt5` | `out/nfsu2_opt5_boot` | opt4 + FCMPBL=1 (dalsız fcompare) |
| `gen/opt6` | `out/nfsu2_opt6_boot` | opt4 + MEMARG=1, `-fno-strict-aliasing` ile derlenir |
| `gen/cov` | `out/nfsu2_cov_boot` | opt2 + çağrı sayacı (`COVERAGE=1`) — yalnız ölçüm |
| (aynı kaynaklar) | `out/nfsu2_{nsa,opt4nsa,opt5nsa}_boot` | `gen/all`/opt4/opt5, `-fno-strict-aliasing` ile (runtime da: `out/obj_rtnsa`) |

Yeniden üretim (depo kökünden; `OVERRIDE` listesi `PROJECT.md` ile aynı):

```bash
./.venv/bin/python tools/flagabi.py <SPEED2.EXE> out/flagabi.json      # ~30 sn
OVERRIDE=... GENOUT=gen/opt2 FLAGABI=out/flagabi.json X87OPT=1 \
  ./.venv/bin/python tools/genall.py <SPEED2.EXE> 128
# gen/opt2 -> out/obj_opt2 (build.sh ile aynı bayraklar), runtime nesneleri out/obj'dan
```

Anahtarlar varsayılan olarak kapalı: anahtarsız `genall.py` bugünkü `gen/all`'ı aynen üretir.

## Ölçüm yöntemi

- **Metrik:** oyun thread'inin kare başına **yürüttüğü ARM komutu ve çevrim sayısı**
  (`thread_selfcounts`, `CPUWIN` raporu). Zamandan çok daha kararlı: aynı ikilinin
  iki koşusu %0.7 (komut) / %3.6 (zaman) farklı.
- **Senaryo:** otomatik Quick Race (`AUTOSEQ`), `PACE60=1 CPUWIN=6200,8800`, 3024x1964,
  her sürüm dönüşümlü 2 koşu.
- **Doğruluk:** diferansiyel fuzzer (`tools/fuzzisa.py`, gerçek x86 = Rosetta; 3969 durum),
  kare 8900 görüntü karşılaştırması (aynı ikilinin iki koşusu arasındaki farkla kıyas).
- **Komut düzeyi profil:** `PERFLOG` örnekleri (1 ms, konuk thread) → `atos -i` →
  `gen/prof` satırı → bir önceki `/* adres komut */` yorumu (`tools/lift_profil.py`).

## Başlangıç: zaman nereye gidiyor (yarış, lift edilmiş kod içinde)

| tür | pay |
|---|---|
| satır 0 + fonksiyon girişi (derleyici: yazmaç saklama, `c->` alanlarının bellek trafiği) | 20.2% |
| `mov` bellek | 14.9% |
| `push`/`pop` | 11.2% |
| `cmp`/`test` (bayrak) | 10.3% |
| x87 (yükle/sakla 8.7, aritmetik 8.5, karşılaştırma 5.8) | 23.0% |
| `call` + `ret` | 6.8% |

Sıcak lift fonksiyonlarındaki bellek erişimlerinin hedefi: `c->esp` %10.1, bayraklar
~%12 (`zf` 4.4, `sf` 3.3, `af` 2.5, `cf` 1.9), genel yazmaçlar ~%10.

## 1. FLAGABI — fonksiyonlar arası bayrak canlılığı

**Sorun:** bayrak canlılık analizi `call`'da "tüm bayraklar okunur", `ret`'te "tüm
bayraklar canlı" sayıyordu. Çağrıdan önceki son bayrak üreten komutun (çok sık:
`add esp,N`, `xor eax,eax; ret`) altı bayrağının hepsi hesaplanıp belleğe yazılıyordu.

**Değişiklik** (`tools/lift.py`: `abi_cfg`, `abi_liveness`, `_flag_defuse_abi`;
`tools/flagabi.py`): MSVC kodu çağrıdan dönen bayrağı okumaz, çağrılan da girişte
çağıranın bayrağını okumaz. Bu varsayım **bütün program üzerinde kanıtlanır**:
sabit noktada her fonksiyonun giriş-canlı bayrakları ve "çağrı sonrası bayrak okunan"
çağrı noktaları hesaplanır; yalnızca bunlar için muhafazakâr davranış korunur.
x86'da tanımsız bırakılan bayrak tanımlandı sayılır (aksi halde `and/test` AF'yi
hiç öldürmüyor, AF sahte biçimde her yere yayılıyordu: ilk denemede 3171 sahte
"bayrak döndüren" fonksiyon).

**Denetim sonucu:** 23274 fonksiyonda 14 "bayrak döndüren", 210 giriş-canlı, dolaylı
çağrı sonrası okuyan 12 nokta; incelenen dolaylı noktaların hepsi veri baytının kod
diye çözülmesi (`retf 0x50ff`, `int3`, rastgele `adc`) ya da muhafazakâr dış kenar.

## 2. X87OPT — blok içi x87 yazmaç tahsisi

**Sorun:** `ST(n) = c->st[(c->top+n)&7]` çalışma anında indisli; clang x87 değerlerini
hiç yazmaca alamıyor, her işlem `top` okuma + adres + yükle/sakla.

**Değişiklik** (`tools/lift.py`: `X87Bolge`): bir temel blokta yığın derinliği
derleme anında bilinir. Bölge başında `_T = c->top`; `ST(n)` → `_x[off+n+8]` (sabit
indis → ARM FP yazmacı); `FPUSH/FPOP` yalnız statik `off`'u değiştirir. Yazılmadan
okunan yuva bir kez yüklenir; bölge sonunda (dal, çağrı, ret, `c->st/c->top`'a
dokunan komut, blok sonu) yalnızca hâlâ yığındaki değiştirilmiş yuvalar geri yazılır,
`c->top` bir kez güncellenir. `fnstsw` statik top'u `fsw_get_top` ile alır.

## Sonuç

| sürüm | komut/kare | çevrim/kare | CPU ms/kare | ikili |
|---|---|---|---|---|
| temel | 43.22 M | 16.50 M | 5.97 | 46.1 MB |
| + FLAGABI | 40.24 M (−%6.9) | 15.76 M (−%4.5) | 5.86 | 40.3 MB |
| + X87OPT | **38.51 M (−%10.9)** | **14.83 M (−%10.1)** | **5.44** | **37.3 MB (−%19)** |

Fuzzer: 3969/3969 eşleşti (X87OPT açık ve kapalı). Görüntü farkı (ort. 0.21) aynı
ikilinin iki koşusu arasındaki farkla (0.16) aynı düzeyde.

## 3. Çağrı modeli

**Ölçüm** (`gen/cov`, `COVREPORT` + `COVRESET_AT=6200`, 2800 kare): yarışta kare başına
**~104 bin** lift edilmiş fonksiyon çağrısı (çağrı başına ortalama ~370 ARM komutu).
Çağrıların %37'si başka çağrı yapmayan ≤50 komutluk yaprak fonksiyonlara (kare başına
~38 bin); en çok çağrılan `__ftol2` (`sub_0075bed4`, 39 komut) 10.5 bin/kare.
Küçük bir fonksiyonun makine kodunda çağrı bedeli: giriş/çıkışta 3 `stp` + 3 `ldp`,
her çağrıdan sonra `c->esp` ve `g_mem` yeniden yüklemesi, her `ret`'te dönüş adresinin
`c->eip`'e yazılması.

**3a. INLINE (olumsuz).** `genall.py INLINE=50`: 4703 yaprak fonksiyonun gövdesi her
shard'a `static inline __attribute__((always_inline)) inl_X` kopyası olarak konur,
doğrudan çağrılar ona yönlendirilir. Sonuç (2 koşu): komut 37.82 → 38.20 M (+%1),
çevrim 14.44 → 14.93 M (+%3.4), ikili 37.3 → 43.6 MB. Çağrı başına sabit bedel
tahmin edildiğinden küçük; gömülen gövdeler zaten büyük goto fonksiyonlarında yazmaç
baskısını artırıyor. ThinLTO da (−%1.8, gürültü içinde) aynı sonucu vermişti.

**3b. RETEIP.** `ret` → `POP32(c->eip); return;` yerine `c->esp += 4(+n); return;`.
`c->eip`'i lift edilmiş çağrıdan sonra okuyan tek yer `genexec.c` idi; artık ittiği
dönüş adresini (`next`) kullanıyor (model değiştirilmiş dönüş adreslerini zaten yok
sayıyordu). Sonuç (opt2 → opt4, 2 koşu): komut 37.83 → 37.59 M (−%0.7), çevrim
13.95 → 13.58 M (**−%2.7**), görüntü aynı.

## 4. Katı takma ad (strict aliasing) tanımsız davranışı

**Belirti.** opt5 (`FCMPBL=1`, `fcompare` → dalsız `fcompare_bl`) yarış karesini belirgin biçimde
değiştirdi (Y 57.3 → 59.5, iki koşuda aynı). İki fonksiyon her `double` çifti için aynı bitleri
üretiyor (kapsamlı test), üretilen C yalnızca fonksiyon adında farklı.

**Daraltma** (tahminsiz, her adım ölçüm):
1. shard ikiye bölme (opt4/opt5 nesnelerini karıştırıp kare 8900 parlaklığı) → `shard_0058`;
2. fonksiyon ikiye bölme (yalnız seçili fonksiyonlarda `fcompare_bl`) → `sub_005f0a40`
   (bir dörtlü ağaç araması: noktayı kutu orta noktalarıyla karşılaştırıp çocuğa iner);
3. fonksiyon düzeyi diferansiyel düzenek: iki derlemeyi rastgele ağaç/nokta/kutu (±0, NaN, eşitlik
   sık) ile yan yana çalıştırıp son durumu karşılaştırır. opt5 `-O3` **kendi `-O0`'ından** sapıyor
   (100 binde 7854), opt4 sapmıyor; fark yığındaki orta noktanın işareti (−0/+0);
4. `-mllvm -opt-bisect-limit` → bozan geçiş **GVNPass** (386 geçişin 262.'si);
5. IR: aynı misafir yığın adresine `i32` yazılıp `float` okunuyor, farklı TBAA etiketleriyle.

**Kök neden.** Lift kodu misafir belleğine aynı adresten farklı tiplerle erişir (`MR32` yazıp
`MRF32` okumak, `MR16`/`MR32` örtüşmesi). C'de bu katı takma ad ihlalidir; derleyici farklı tipli
erişimlerin çakışmadığını varsayıp yükü yazmanın önüne taşıyabilir. Derleme bayraklarında
`-fno-strict-aliasing` **hiç yoktu** — temel `gen/all` ve runtime dahil. Bayrakla opt5 = opt4 =
temel (görüntü), fark 0. `fcompare_bl` doğrudur; UB yalnızca kodun biçimi değişince görünür oldu.

**Doğrulama ve bedel** (Windows referansıyla, kare 1300–8900, port `NFSRES=1280x960 VCLOCK=fixed`):
bayrakla temel/opt4/opt5 aynı görüntüyü verir; temel sürümde bu UB'nin görünür bir etkisi
ölçülmedi. Çıplak bedel (2 koşu ort.):

| | komut/kare | çevrim/kare |
|---|---|---|
| temel | 42.61 M | 14.34 M |
| temel + `-fno-strict-aliasing` | 45.24 M (+%6.2) | 15.14 M (+%5.6) |
| opt4 | 38.54 M | 12.81 M |
| opt4 + `-fno-strict-aliasing` | 39.61 M (+%2.8) | 12.95 M (+%1.1) |

Bedelin kaynağı (disasm): tip bilgisi yokken her misafir yazmasından sonra küresel `g_mem`'in
değeri yeniden okunuyor (`sub_005f0a40`: 2 → 10 `ldr x, [&g_mem]`).

## 5. MEMARG — misafir bellek tabanı parametre olarak

**Gözlem.** Bayraklı da bayraksız da, misafir belleğine her yazmadan sonra `c->eax` gibi CPU
alanları yeniden okunuyor: `c` `restrict` olsa da `g_mem` bellekten okunan bir işaretçi ve `c`
çağrılara geçirildiği için "kaçmış"; LLVM misafir yazmasının `*c`'ye dokunmadığını kanıtlayamıyor.

**Değişiklik** (`tools/genall.py`, `MEMARG=1`, üretim sonrası dönüşüm): her fonksiyon
`void sub_X_m(CPU *restrict c, uint8_t *restrict _mb)` olur; shard'da `#define g_mem _mb`; lift
kodu birbirini `sub_Y_m(c, _mb)` ile çağırır. Dışarıya açık `void sub_X(CPU*)` ince sarmalayıcı
olarak kalır (fntab, dolaylı çağrı, runtime → lift çağrıları, override'lar değişmez; override ve
eksik fonksiyonlar için `_m` köprüsü `stubs.c`'de). İki ayrı `noalias` parametre LLVM'de kesin
olarak çakışmaz — kaçış analizi gerekmez. ARM64'te `_mb` `x1`'de gelir.

Tek fonksiyonda (`sub_005f0a40`, `-fno-strict-aliasing`): CPU alanı okuma 25 → 4, misafir okuma
18 → 8; int yazıp float okuma artık doğrudan iletiliyor (`str w12` → `fmov s0, w12`).

**Sonuç** (opt4 + `-fno-strict-aliasing` ↔ opt6, dönüşümlü; komut 2+2, çevrim gürültülü p4
koşusu (IPC 2.72) dışarıda): komut 39.33 → **35.50 M (−%9.7)**, çevrim 12.94 → **11.51 M (−%11)**,
ikili 38.1 → 34.8 MB. Görüntü: Windows koşullarındaki 16 karenin hepsi opt4+bayrak ile aynı.
MEMARG doğruluk düzeltmesinin bedelini geri almakla kalmıyor, üstüne çıkıyor.

## Toplam (doğru derleme ile)

| sürüm | komut/kare | çevrim/kare | ikili |
|---|---|---|---|
| temel (`gen/all`, katı takma ad UB'si açık) | 42.61 M | 14.34 M | 46.1 MB |
| opt6 (FLAGABI + X87OPT + RETEIP + MEMARG, `-fno-strict-aliasing`) | **35.50 M (−%16.7)** | **11.51 M (−%19.7)** | **34.8 MB (−%25)** |

## Sıradaki adaylar

- **Çağrı modeli:** `c->esp` ve genel yazmaçlar her çağrıda belleğe yazılıp geri
  okunuyor (sıcak koddaki bellek erişimlerinin ~%20'si).
- **`fcom → fnstsw → test → jcc` kaynaşması:** tek bir C karşılaştırmasına indirmek.
- **x87 bölgesini bloklar arası genişletmek** (derinliği tutarlı birleşme noktaları).
