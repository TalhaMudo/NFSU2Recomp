# D3DX9 efekt kütüphanesi — HLE çalışması

NFSU2'ye statik bağlı D3DX9 efekt çerçevesini (ID3DXEffect) adım adım yerel
koda taşıma / kısa devre etme çalışması. Kod: `runtime/d3dx_hle.c`.
Genel performans bağlamı: `PROJECT.md` → "Performans".

---

## Neden

2026-09-23 ölçümü (M1 Pro, en yüksek ayar, AUTOSEQ Quick Race, yarış ortası,
`sample` 8204 örnek, konuk thread öz-zaman):

| kalem | pay |
|---|---|
| **D3DX9 efekt kütüphanesi** (`mov edi,edi` prologlu 33 fonksiyon, çoğu 0x0066xxxx) | **%47** |
| oyunun kendi kodu | %32 |
| bizim D3D9 katmanımız (`com_call`, `memmove`, `dispatch_indirect`) | ~%15 |

Alt çağrılarıyla birlikte **tek bir API girişi, `ID3DXEffect::BeginPass`,
konuk thread'in %48'i**. İçindeki gerçek D3D çağrıları yalnızca %3.4; geri
kalanı D3DX'in her geçişte bütün durum ifadelerini (preshader) CPU'da,
bizde de çevrilmiş x86 üzerinden yeniden yorumlaması.

---

## Harita

### ID3DXEffect vtable — `0x7b6fe8`

Eşleme `ret N` imzalarıyla doğrulandı (her `Set*` 0xc/0x10, `Begin` 0xc,
`BeginPass` 8, `CommitChanges`/`EndPass`/`End` 4 ...). Düzen, pixel/vertex
shader için hem Set hem Get taşıyan geçiş dönemi (2004) başlığı:

| yuva | metot | adres | | yuva | metot | adres |
|---|---|---|---|---|---|---|
| 3 | GetDesc | 0065cb66 | | 58 | SetArrayRange | 0066c9cd |
| 20 | SetValue | 0066c905 | | 59 | GetPool | 0065cb08 |
| 26 | SetInt | 0066dd7f | | 60 | SetTechnique | 00661e8c |
| 28 | SetIntArray | 0066e8a9 | | 62 | ValidateTechnique | 0066074e |
| 30 | SetFloat | 0066f501 | | 65 | **Begin** | 00664792 |
| 32 | SetFloatArray | 0066eec8 | | 66 | **BeginPass** | **0066ace2** |
| 34 | SetVector | 0066d303 | | 67 | CommitChanges | 0066ac27 |
| 36 | SetVectorArray | 0066ca38 | | 68 | EndPass | 0065addb |
| 38 | SetMatrix | 0066d13a | | 69 | End | 0065acf5 |
| 52 | SetTexture | 0066d7e9 | | 70 | GetDevice | 0065cb37 |
| 54–57 | Set/Get Pixel/VertexShader | | | 71/72 | OnLostDevice / OnResetDevice | 0065c89d / 0065e593 |
| | | | | 78 | CloneEffect | 0067065b |

### Sıcak çağrı ağacı (profil payı, alt çağrılar dahil)

```
48.1%  BeginPass            0066ace2   (oyun: sub_005cf430 çizim döngüsü, %38.6)
47.1%   geçişi uygula        006691b1
33.3%    durum ata           00669296   tip>>24 anahtarı, 100 baytlık durum kaydı
26.0%     durum değeri al    00664a2f   ← Aşama 1 burayı not alıyor
17.2%      ifade değerlendir 00663a42   FPU CW ayarı + girdi topla + VM
 5.3%       preshader VM     00688e89   x87, double yazmaç dosyası
 4.8%       girdi topla      00662edc   (tür 1/2)   / 00663275 (tür 0)
 6.5%      sonuç işle        00660a52
 8.2%    (başka yol)         00669f40 → 00668e0d → 00668cd0 → 00664a2f
```

### Çağrı sayıları (yarış, kare başına, `D3DXSTAT`)

| | çağrı/kare |
|---|---|
| Begin / End | 80 / 66 |
| BeginPass / CommitChanges / EndPass | 1398 |
| durum ata (`669296`) | 28 338 (geçiş başına ~20) |
| durum değeri al / ifade değerlendir / VM | 11 838 (geçiş başına ~8.5) |
| Set* toplamı (Matrix 1731, Vector 1512, Texture 576, IntArray 501, Float 233, VectorArray 130) | ~4 700 (geçiş başına ~3.4) |

Oyun her çizimde BeginPass çağırıyor. Geçiş başına ~3 parametre değişirken
D3DX ~20 durumun hepsini yeniden hesaplayıp uyguluyor.

### Bilinen veri yapıları

Efekt nesnesi (`this`, `ecx`):

| ofset | anlam |
|---|---|
| +0x2c | parametre/geçiş kayıtları dizisi, 0x64 bayt/kayıt |
| +0x3c | Begin durumu (0 = Begin dışında → `D3DERR_INVALIDCALL` 0x8876086c) |
| +0x40 / +0x42 | teknik indeksleri |
| +0x44 | etkin geçiş (0xffff = yok) |
| +0xa4 | teknik dizisi, 0xb4 bayt/teknik; geçişler `[teknik+0x38]`, 0x38 bayt, geçiş param indeksi `+0x34` |
| +0xb0 | nesne tablosu |
| +0x114 | kaydedilmiş FPU kontrol sözcüğü |
| +0x118 | bayraklar: bit0 = CW kaydedildi, 0x100 = geçiş içinde, 8 = … |

Parametre/geçiş kaydı `P = idx*0x64 + [this+0x2c]`: `+0` veri bloğu işaretçisi,
`+4` alt kayıt (durum) dizisi, `+8` bayraklar, `+0x36` durum sayısı.

Durum kaydı `R = b*0x64 + [P+4]`:

| ofset | anlam |
|---|---|
| +0x08 | değer tamponu işaretçisi |
| +0x30 | bayrak; &4 = sabit değer (`[R+0x5c]`'den `[R+0x60]` bayt kopyalanır) |
| +0x34 / +0x4e | girdi kayıtları (32 bayt) / sayısı; `[R+0x3c]` != 0 ise geçerli |
| +0x38 | program kabı (vtable 0x7b7124, yuva 3 = `[+0xc]` döndüren getter) |
| +0x40 / +0x44 | VM program işaretçileri |
| +0x48 | hedefin `P.data` içindeki ofseti |
| +0x52 | durum türü → 0x7b5834 tablosu (0x1c bayt/giriş) |

Girdi kaydı: `+0` kirli bayrağı (hep açık, `[+0] = [+4]` ile yeniden kuruluyor,
yani işe yaramıyor), `+0xc` → {veri işaretçisi, tanım}, `+0x10` → yazmaç
tanımı (`+4` küme 0/1/2, `+6` indeks, `+8` adet). Toplayıcılar float/int/bool
veriyi VM'in **double** yazmaçlarına (float4 başına 32 bayt) çeviriyor.

---

## Aşamalar

### Aşama 0 — ölçüm altyapısı ✅ (2026-09-23)

* `runtime/d3dx_hle.c`: 20 sıcak D3DX fonksiyonuna sayaç sarmalayıcısı.
* `tools/override_patch.py`: `OVERRIDE`'ı tam yeniden üretim yapmadan mevcut
  shard'lara uygular; yalnızca etkilenen shard'lar yeniden derlenir.
* `CPUWIN`: konuk thread CPU süresi / kare (GPU'dan bağımsız ölçü).

### Aşama 1 — durum değeri notu (memo) ✅ (2026-09-23)

`664a2f` (durum değeri al), ifade durumlarında yalnızca tek bir girdinin
verisine bağlı. `D3DXANALYZE` ölçümü: yarışta 111 farklı (efekt, geçiş,
durum) anahtarı, değerlendirmelerin **%95.8'inde girdi verisi aynı durumun
bir önceki değerlendirmesiyle bayt bayt aynı**.

Not anahtarı `(this, a, b)`. Kayıtta girdinin **tam kopyası** tutuluyor
(`this`, `R`, `[R+0x38]`, `[P]`, `[R+0x48]`, `[R+8]` ile her girdinin kirli
bayrağı, veri işaretçisi ve veri baytları; en fazla 256 bayt) ve `memcmp`
ile karşılaştırılıyor. Önceki kopyayla aynıysa, hedef dword ve değer
tamponunun ilk dword'u da son gerçek hesaptan beri değişmemişse çağrı
atlanıyor. (İlk sürüm bayt bayt FNV özeti kullanıyordu; profilde kendisi
%8 tutuyordu. Tam kopyaya geçince hem hızlandı hem çakışma riski kalktı.)

**Doğrulama (`D3DXVERIFY=n`):** tahmini isabetlerin 1/n'inde orijinal yine
çalıştırılıyor; efekt nesnesinin ±256 KB'ı (haritalı sayfalar) ve `.data`
önce/sonra karşılaştırılıyor.
* Tek fark `this+0x114/0x118`: FPU CW kaydı. Zararsız, çünkü atlanan
  değerlendirme CW'ye hiç dokunmuyor, BeginPass sonu da geri yükleme yapmıyor.
  Karşılaştırmadan çıkarıldı.
* Not kapalıyken **965 299**, not açıkken **1 003 104** (özetli sürüm) ve
  **501 888** (tam kopya sürümü) kontrol: **0 fark**, 0 koruma iptali.
* Görsel: araç seçimi (3050. kare) birebir aynı; yarış karelerinde yalnızca
  zamanlamaya bağlı sayaçlar farklı.

**Sonuç** (dönüşümlü dört koşu, 2448x1836, yarış 5900–8990. kareler):

| | konuk CPU ms/kare | yarış ort | medyan | p95 |
|---|---|---|---|---|
| not kapalı | 11.98 / 10.89 | 78.6 / 90.0 fps | 12.13 / 11.05 ms | 15.30 / 12.33 ms |
| not açık | 8.10 / 8.11 | 111.2 / **118.9 fps** | 8.36 / **8.33 ms** | 16.58 / 8.72 ms |

Konuk CPU **−%29**. GPU boş olan çiftte yarış ekranın 120 Hz sınırına
dayandı (medyan 8.33 ms).

`D3DXMEMO=0` notu kapatır (varsayılan açık).

### Aşama 2 — durum atama notu ✅ (2026-09-24)

**Ölçüm (`COMSTAT`, yarış, kare başına):** BeginPass/CommitChanges içinden
~36 000 cihaz çağrısı geliyor, **%96'sı gereksiz** (cihazdaki değeri yeniden
yazıyor). Dışarıdan gelen yalnızca ~7 000.

| metot | BeginPass içi | gereksiz | dışarıda | gereksiz |
|---|---|---|---|---|
| SetRenderState | 12 598 | %97.9 | 1 578 | %85.0 |
| SetSamplerState | 11 745 | %97.4 | 1 212 | %75.2 |
| SetTextureStageState | 5 325 | %99.3 | 486 | %93.4 |
| SetTexture | 2 189 | %74.1 | 194 | %63.4 |
| SetVertexShader / SetPixelShader | 1 340 | %95.9 | 66 | %25.8 |
| SetVertexShaderConstantF | 1 317 | %50.4 | 57 | %10.5 |
| SetPixelShaderConstantF | 458 | %76.2 | 16 | %0 |

**Uygulama:** `sub_00669296` (durum ata) sarmalayıcısı + `runtime/dxrec.h`.
* Gerçek çalışmada fonksiyonun yaptığı cihaz çağrıları (argümanlar ve sabit
  verisiyle) `com.c`'de kaydediliyor (`dxrec_begin/end`).
* "Bu çağrı şimdi yapılsa etkisiz mi" sorusu `com.c`'de, işleyicinin
  yazacağı değer mevcut durumla karşılaştırılarak yanıtlanıyor
  (`dxrec_redundant`). Desteklenen 8 metot: RenderState, SamplerState,
  TextureStageState, Texture (türetilmiş Metal kimliği dahil), Vertex/Pixel
  Shader (türetilmiş kimlik dahil), VS/PS ConstantF (veri baytları). Başka
  her cihaz çağrısı, iç içe `669296`, durum bloğu kaydı ya da taşma kaydı
  geçersiz kılıyor.
* Anlık görüntü: 11 sabit alan (`this` bayrakları, P), durum kaydının tamamı
  (0x64 bayt), hedef değer (16), değer tamponu (16), nesne tablosu girişi,
  ifade durumlarında Aşama 1 girdileri. Kayıt yuvasının tamponuyla **yerinde**
  karşılaştırılıyor, yalnızca ilk farktan sonra kopyalanıyor (8 baytlık satır
  içi sözcük karşılaştırması).
* İsabet = görüntü aynı **ve** kaydedilen her çağrı şu an etkisiz.

**Doğrulama (`D3DXVERIFY2=n`):** örneklenen isabetlerde orijinal kayıtla
çalıştırılıyor. Bellek penceresi aynı olmalı ve yaptığı her çağrı o anda
etkisiz olmalı.
* İlk koşuda iki durum türü farklı çıktı. **Tür 9** doku tablosundan
  dolaylı okuyor; **tür 14/15** ifadeyle diziden nesne seçip iç içe
  uyguluyor. Anlık görüntü bu girdileri kapsamıyor. **Yalnızca tür 1–8
  notlanıyor** (çağrıların %86'sı).
* Sonrasında 3 286 996 + 3 304 796 kontrol, **0 bellek / 0 cihaz / 0 eax
  farkı**. Aşama 1 doğrulaması birlikte açıkken de temiz.
* Görsel: araç seçimi %0.07 piksel farkı (imleç), yarışta yalnızca sayaçlar.

**Sonuç:** isabet %82 (tür 1–8'de %92–99).

| ölçüm | Aşama 2 kapalı | açık |
|---|---|---|
| konuk CPU, `PACE60=1` (60 fps sabit) | 7.94 / 7.80 ms | 7.50 / 7.45 ms (**−%5**) |
| yarış fps (bu makine, 120 Hz) | 119.8 | 119.9 (zaten sınırda) |

60 fps profilinde BeginPass meşgul sürenin ~%26'sı. Sarmalayıcının kendi
kontrolü ~%12 (durum başına ~34 ns, orijinal ~100 ns). Kazancın küçük
kalmasının sebebi bu: her durum için ~300 bayt girdi okunup karşılaştırılıyor.

`D3DXMEMO2=0` kapatır (varsayılan açık).

**Ölçüm dersi:** Aşama 1'den sonra yarış 120 Hz sınırında. Orada konuk CPU/kare
iki yapılandırmada da 8.15 ms çıkıyor: oyunun işi kare hızıyla biraz esniyor
(60 fps'te 7.9 ms). CPU değişikliklerini **`PACE60=1` + `CPUWIN`** ile ölçün.
`NOVSYNC=1` bu sınırı kaldırmıyor (pencere kipinde birleştirici 120 Hz).

### Aşama 2b — kalanlar (SIRADAKİ)

1. **Kontrolü ucuzlatmak:** durum kaydının 100 baytı D3DX'in statik yapısı.
   Yalnızca okunan alanlar (+0, +4, +8, +0x1c, +0x30, +0x34, +0x38, +0x3c,
   +0x48, +0x4e, +0x52..+0x60) ile yetinmek görüntüyü ~yarıya indirir.
   Önce `D3DXVERIFY2` ile doğrulanmalı. Yuva tablosu da (8192 × ~1.1 KB)
   önbelleğe sığmıyor olabilir.
2. **Tür 9 ve 14/15:** dolaylı girdilerini görüntüye eklemek (tür 9: `[this+0x18]`
   doku tablosu girişi; tür 14: seçilen nesnenin kendi durumları). Tür 14 kendi
   ifadesini `663a42`'yle doğrudan değerlendiriyor, yani Aşama 1 notu onu
   kapsamıyor.
3. **`669f40 → 668e0d → 668cd0` yolu** (60 fps'te meşgulün ~%7'si): sampler
   nesnesi uygulaması gibi görünüyor, alt durum başına aynı kayıt/denetim
   yapılabilir.

### Aşama 3 — parametre sürümleri + geçiş düzeyinde atlama ❌ (2026-09-24, olumsuz sonuç)

**Fikir:** parametre başına sürüm sayacı ile "girdiler değişti mi"yi O(1)
yapmak ve BeginPass başına tek kontrolle bütün geçişi atlamak.

**Kurulan altyapı (kodda duruyor, varsayılan KAPALI):**
* Bütün `Set*` yuvaları (20–56 ve CloneEffect, 20 çağrı yeri) `0x66056c`'yi,
  o da (efekt havuzundaki her efekt için) **`0x65ab24(efekt, yuva)`**'yı
  çağırıyor. Parametre bloğu yolu (`0x67054b`) doğrudan `65ab24`'e geliyor.
  `65ab24` kancası değişen verinin 64 baytlık satırlarına global bir sayaç
  yazıyor. Efekt havuzu veriyi efektler arasında paylaştığı için anahtar
  veri **adresi** (aynı veri adresi farklı efektlerde görüldü).
* Parametre yuvası: `+0` veri, `+4` tanım, `+0x14` parametre indeksi.
  Tanım: `+0` tür (2 int, 3 float, 5 doku), `+4` sınıf (0 skaler, 1 vektör,
  2 matris, 4 nesne), `+0x10` eleman, `+0x14` satır, `+0x18` sütun.
  İfade girdi kaydının `+0xc` alanı aynı yuva yapısını gösteriyor.
* `6691b1` geçiş sarmalayıcısı: parmak izi (efekt alanları + geçiş kaydı +
  teknik `+0x8c..+0xab`), bağımlılık satırları, kayıtlı çağrıların
  etkisizliği. `dxrec` artık kayıt türü (geçiş/durum) ve `dxrec_append`
  destekliyor. Aşama 2'nin atladığı durumların çağrıları açık geçiş kaydına
  ekleniyor.
* Bayrak 8 yolu (`66404c`/`6641a7`): geçişin shader sabit yazmaç sayısını
  tekniğin en büyüğüyle karşılaştırıyor, yalnızca büyümede iş yapıyor
  (ısınmadan sonra çağrısız). Parmak izine teknik alanları eklenerek uygun
  sayıldı.

**Sonuç:** tahmini isabet %86, ama `D3DXVERIFY3` isabetlerin **%78'inde**
geçişin gerçekte farklı çağrı ürettiğini gösterdi (919 133 kontrolde 713 519
cihaz, 523 606 bellek farkı). Sebepler:
1. **Dokular parametre verisinden gelmiyor.** Sampler/doku durumu parametreye
   bir nesne indeksi üzerinden bakıyor; `SetTexture` yeni işaretçiyi efektin
   nesne tablosuna (`[this+0x18] + indeks*24 + 4`) yazıyor, parametre verisi
   (indeks) değişmiyor.
2. Oyun neredeyse **her çizimde doku değiştiriyor** (kare başına 576
   SetTexture). Doku bağımlılığı doğru eklense bile bir geçişin bütünüyle aynı
   kaldığı durumlar ~%19.

Yani bu oyunda doğru ayrıntı düzeyi geçiş değil, **durum** (Aşama 2).
`D3DXMEMO3` varsayılan kapalı, sürüm tablosu yalnızca açıkken tutuluyor.
Aşama 1–2 bu değişikliklerden sonra yeniden doğrulandı: 3 541 437 + 81 950
kontrol, 0 fark.

**Yan bulgu, `668e0d`'nin kimliği:** shader tabanlı geçiş uygulaması.
VS/PS'yi bağlıyor (`+0x170`/`+0x1ac`), parametrelerden sabit bloklarını
dolduruyor (`6623ea`/`6627a5`/`662bab`) ve `SetVertex/PixelShaderConstantF/I/B`
ile yüklüyor (`+0x178/+0x1b4`, `+0x180/+0x1bc`, `+0x188/+0x1c4`). Sonunda
`668cd0` ile sampler durumlarını (tür 3 → `SetSamplerState`, tür 9 →
`SetTexture`, doku nesne tablosundan) uyguluyor. Sabit yüklemeleri çizim
başına gerçekten değişiyor. Tasarruf ancak shader/sampler kısmında var.

### Aşama 3b — sıradaki adaylar

1. **`668cd0` (sampler alt durumları) için durum düzeyinde not:** Aşama 2'nin
   yöntemi, doku için nesne tablosu girişi (`[this+0x18]`) anlık görüntüye
   eklenerek. 60 fps'te `668e0d` yolu meşgul sürenin ~%7.5'i.
2. **Aşama 2 kontrolünü ucuzlatmak** (Aşama 2b'deki madde).
3. Parametre sürümlerini **durum düzeyinde** kullanmak (ifade girdilerinin
   bayt karşılaştırması yerine). Beklenen kazanç küçük: görüntünün büyük
   kısmı statik durum kaydı.

### Aşama 4 — tam yerel ID3DXEffect (uzun vade)

`.efx` ayrıştırma (`tools/efxparse.py` başlangıç noktası), teknik/geçiş/durum
modeli, preshader VM'in yerel C karşılığı. Bu aşamada D3DX'in iç veri
yapılarına bağımlılık tamamen kalkar.

---

## Ortam değişkenleri

| değişken | etki |
|---|---|
| `D3DXMEMO=0` | Aşama 1 notunu kapatır (varsayılan açık) |
| `D3DXVERIFY=<n>` | tahmini isabetlerin 1/n'inde orijinali çalıştırıp belleği karşılaştırır (yavaş) |
| `D3DXSTAT=<kare>` | o kareden itibaren sayaç/süre raporu (zamanlayıcılar kareye ~5 ms ekler; süreleri profilden alın) |
| `D3DXANALYZE=<kare>` | ifade değerlendirme girdilerinin değişim oranı |
| `CPUWIN=<baş>,<son>` | konuk thread CPU ms/kare (120 Hz sınırında `PACE60=1` ile birlikte) |
| `D3DXMEMO2=0` | Aşama 2 notunu kapatır (varsayılan açık) |
| `D3DXVERIFY2=<n>` | Aşama 2 isabetlerinin 1/n'inde orijinali kayıtla çalıştırıp bellek + cihaz karşılaştırır |
| `D3DXMEMO3=1` | Aşama 3 geçiş notu (araştırma; varsayılan KAPALI, doğrulamada yanlış çıktı) |
| `D3DXVERIFY3=<n>` | Aşama 3 isabetlerinin 1/n'inde orijinali çalıştırıp karşılaştırır, etkili çağrıları yazar |
| `COMSTAT=<kare>` | cihaz metot sayıları, BeginPass içi/dışı, gereksiz oranı (`D3DXSTAT` ile birlikte) |

Hepsi ölçüm/tanı amaçlı; `run.sh` bunları temizlemiyor. Ölçümde tarif:
`AUTOSEQ` Quick Race + `QUITFRAMES=9000`, pencere 5900–8990. Bkz.
`PROJECT.md` "Ölçüm yöntemi".

**Ölçüm tuzağı:** kare süresi max(CPU, GPU). GPU başka bir uygulamayla
paylaşıldığında (2026-09-23 gecesi GPU oyun kapalıyken %20 doluydu, oyun
içinde kare başına 5 → 18 ms) eski ve yeni koşuların hepsi yavaşlar ve
CPU kazancı kaybolur. CPU değişikliklerini `CPUWIN` ile ölçün, kare süresini
`NFSNATIVE=0` ile.

## Yeniden üretim

Sarmalanan her fonksiyon `sub_XXXX_orig` olarak üretilmeli. Tam OVERRIDE
listesi (overrides.c + d3dx_hle.c; 29 adres, `PROJECT.md` → "Komutlar" ile
aynı olmalı). İlk üçü `overrides.c`'deki yağmur / günün-saati sarmalayıcıları;
biri eksik olursa `sub_XXXX_orig` üretilmez ve link kırılır:

```
60fab0,61e390,61f130,5c7ed0,50d510,65ae35,6623ea,66aa3f,66ace2,6691b1,669296,664a2f,663a42,688e89,663275,662edc,660a52,668e0d,664792,66ac27,65addb,65acf5,66d13a,66d303,66d7e9,66e8a9,66f501,66ca38,65ab24,67193e,66056c,65cf44,640c12,668cd0,65ccd8
```

Tam üretim yerine mevcut shard'lara uygulamak için:

```bash
.venv/bin/python tools/override_patch.py 66ace2,6691b1   # etkilenen shard adlarını basar
# yalnızca o shard'ları derle (build.sh ile aynı bayraklar), sonra ./link.sh
```

---

## İlerleme günlüğü

| tarih | aşama | değişiklik | konuk CPU ms/kare (yarış) | not |
|---|---|---|---|---|
| 2026-09-23 | 0 | ölçüm altyapısı, vtable haritası | 10.9–12.3 | referans (not kapalı) |
| 2026-09-23 | 1 | durum değeri notu (`664a2f`), özetli | 8.40–9.22 | −%22–25, 0 doğrulama farkı |
| 2026-09-24 | 1 | not tam kopya + `memcmp` | 8.10–8.11 | −%29 (aynı gece referans 10.9–12.0), yarış 118.9 fps |
| 2026-09-24 | 2 | durum atama notu (`669296`), çağrı kaydı + etkisizlik denetimi | 7.45–7.50 (`PACE60`) | −%5 (`PACE60` referansı 7.80–7.94); 120 Hz'de fark yok, zaten sınırda |
| 2026-09-24 | 3 | parametre sürümleri + geçiş notu | — | olumsuz: doğrulamada isabetlerin %78'i yanlış (doku nesne tablosu, çizim başına doku değişimi); varsayılan kapalı |
| | | | | |
