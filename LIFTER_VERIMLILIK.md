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

## Sıradaki adaylar

- **Çağrı modeli:** `c->esp` ve genel yazmaçlar her çağrıda belleğe yazılıp geri
  okunuyor (sıcak koddaki bellek erişimlerinin ~%20'si).
- **`fcom → fnstsw → test → jcc` kaynaşması:** tek bir C karşılaştırmasına indirmek.
- **x87 bölgesini bloklar arası genişletmek** (derinliği tutarlı birleşme noktaları).
