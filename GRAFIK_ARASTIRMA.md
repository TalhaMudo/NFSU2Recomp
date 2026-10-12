# NFSU2 grafik geliştirme araştırması (2026-10-04)

Soru: Bu oyuna yeni yansıma/efekt/motor iyileştirmesi ekleyen projeler nasıl
yapılıyor, topluluk ne yapmış, biz native port olarak ne yapabiliriz?

**Kısa cevap.** PC topluluğu dört yol kullanıyor: (1) exe'ye bellek/kod yaması
(ASI eklentileri), (2) `d3d9.dll` sarmalayıcısı (ReShade, dgVoodoo, Widescreen
Fix'in PostFX'i), (3) RTX Remix ile yol izleme, (4) veri dosyası modları
(doku/model). Bizim portta (1) ve (2) **DLL olarak çalışmaz** ama ikisini de
daha güçlü biçimde yapabiliyoruz: D3D9 katmanı zaten bizim, lift edilmiş kod
zaten bizim. (4) ise olduğu gibi çalışır, çünkü aynı oyun dosyalarını okuyoruz.

---

## 1. Topluluk envanteri

| Proje | Ne yapıyor | Teknik | Lisans | Bize uygulanabilirlik |
|---|---|---|---|---|
| **NFSU2 HD Reflections** (Aero_, sonra Zintexiss çatalı) | Yol yansıması, araç küp haritası ve dikiz aynası çözünürlüğünü yükseltir; yansımalara gökyüzü, farlar, ışık izleri ekler; yansıma LOD'unu ve mesafesini artırır; "Xbox 360 tarzı" hareket bulanıklığı; krom malzemesi; bellek havuzlarını büyütür | ASI + `injector` ile sabit adres yaması ve kod mağaraları | **GPL-3** | Bilgi (adresler, sabitler) kullanılabilir, kod kopyalanamaz. En değerli harita bu. |
| **Widescreen Fix** (ThirteenAG) | Hor+ FOV, HUD, ayna/yansıma en-boy oranı, "Console Gamma", SMAA, MSAA kenar hatası gizleme, pencereli mod | ASI + bayt imzası (`hook::pattern`) + `IDirect3DDevice9` EndScene kancası | MIT | Hor+ ve HUD bizde zaten var (`NFSRES`, `HUDFIX`). Console Gamma ve SMAA'yı Metal'de yeniden yazmak kolay. |
| **Xbox Rain Droplets** (ThirteenAG) | Kameraya yapışan yağmur damlaları (Xbox sürümündeki efekt) | Oyunun yağmur nesnesinden yoğunluk ve "tünelde miyim" bilgisi okunuyor; damlalar D3D9 ile çiziliyor | MIT | Doğrudan taşınabilir: guest belleğini okuyup damlaları Metal'de çizeriz. |
| **NFSU2 Extra Options** (ExOpts Team) | Sürekli yağmur, yağmur boyutu/yoğunluk/yerçekimi, yol yansıması miktarı, far parlaklığı, neon parlaklığı, gizli kameralar, debug kamera | ASI, `.ini` | — | Oyun değişkenlerine yazma; bizde ortam değişkeni ya da ayar menüsü olarak kolay. |
| **Underground 2 RTX Remix** (UncleBurrito; `adamplayer/Underground2RTX`) | Yol izleme: tüm ışık kaynakları yeniden konmuş, neonlar ışık yayıyor, parlak araç boyası, yeni trafik araçları, gökyüzü, NRC/froxel sis, DLSS/XeSS | RTX Remix D3D9 çağrılarını yakalayıp sahneyi yeniden kuruyor; **kamera bazlı ayıklamayı kapatmak için `speed2.exe`'ye IPS yaması** gerekiyor | — | Fikir olarak önemli. Sorunlar: ışık ayıklaması titremesi, 100+ ışık, RTX 4080'de 30–50 fps. |
| **Redux 3.0, "UG2 SKIN", 8K Remaster** | HD dokular, yeni aydınlatma atmosferi, ReShade ayarları | Oyun dosyalarının (TPK/BIN) değiştirilmesi + ReShade | Değişken | Doku/model kısmı bizde **olduğu gibi çalışır**. ReShade kısmı çalışmaz (Metal'de ReShade yok). |
| **ReShade ön ayarları** | SSAO, bloom, SMAA, keskinleştirme, LUT | d3d9 sarmalayıcısında ekran uzayı son işlemi, derinlik tamponunu okuyarak | BSD (ReShade) | Kendi Metal son işlem geçişimizle eşdeğeri yapılabilir. |
| **dgVoodoo2 / DXVK** | Zorla MSAA/AF, API çevirisi | D3D9 → D3D11/Vulkan | — | Bizim D3D9→Metal katmanımızın karşılığı. Zorla MSAA'yı biz de yapabiliriz. |

## 2. Oyunun yansıma hattı (bizim exe'de doğrulandı)

HD Reflections kodundan ve kendi ayrıştırmamızdan:

| Yansıma | Nasıl üretiliyor | Varsayılan | Bizim exe'de konum |
|---|---|---|---|
| **Araç ortam haritası** | 6 küp yüzü ayrı ayrı çiziliyor (bunu zaten çözdük, bkz. küp doku notu) | 128 px/yüz | `.data` `0x7FEE6C` = **128** ✅ |
| **Dikiz aynası** | ayrı eView (`ecx==3`) | 256×256 | `.data` `0x7FEE80/84` = **256/256** ✅ |
| **Yol yansıması** | ayrı eView (`ecx==4`), aynalanmış sahne render hedefine çiziliyor, sonra bulanıklaştırılıyor | **320×240** | `CreateTexture(320,240,…,usage=RT)` üç kez: `0x5BA099`, `0x5BA0DB`, `0x5BA10F` ✅ |

eView kimlikleri (modlardan): `1` ana görüntü, `3` dikiz aynası, `4` yol yansıması.
Oyunun ayarları zaten en yüksekte: `g_RoadReflectionEnable=3`,
`g_CarEnvironmentMapEnable=3`, `g_FSAALevel=-1` (MSAA bizde hiç yok).

Yol yansıması 1920×1080 ekranda 320×240 → **yatayda 6×, dikeyde 4,5× büyütülüyor**.
Islak zemindeki bulanık, bloklu yansımanın asıl nedeni bu. Topluluğun en
çok indirilen grafik modunun var olma sebebi de bu.

### ⚠️ Kritik tuzak: bizim exe, modların hedeflediği exe değil

HD Reflections "v1.2 HOODLUM speed2.exe, **4.800.512 bayt**" istiyor. Bizimki
**4.788.224 bayt** (SHA `8f6a9ae2…`). İkisinin giriş noktası da `0x75BCC7`, bu
yüzden modun kendi sürüm kontrolü bile ikisini ayırt edemiyor. Ölçülen:

* **`.data` adresleri tutuyor:** `0x7FEE6C`=128, `0x7FEE80`=256, `0x7FEE84`=256.
* **Kod adresleri kaymış:** yol yansımasının anlık değerleri modda
  `0x5BA08A/8F`, bizde `0x5BA09A/9F` (**+0x10**). Diğer kanca noktaları başka
  miktarlarda kaymış (ör. `0x5CAEC3` bizde bir `call`'ın 5 bayt öncesi).

**Sonuç:** modlardaki kod adreslerini **asla doğrudan kullanmayın**. Her
birini bayt imzasıyla yeniden bulun (Widescreen Fix ve Rain Droplets'ın
yaptığı gibi `hook::pattern`). `.data` adresleri değerleri okunarak
doğrulandıktan sonra kullanılabilir.

## 3. Bu teknikler bizim portta nasıl karşılık bulur

| PC'de | Bizde | Not |
|---|---|---|
| `.data`/`.rdata`'ya yazma | Oyun başlamadan önce `MW32(adres)=değer` | En ucuz yol. Küp ve ayna çözünürlüğü bu sınıfta. |
| Koddaki anlık değeri değiştirme (`push 0x140`) | **Çalışmaz**: lift edilmiş C'de anlık değer derlenip sabite dönüşmüş, guest belleğine yazmak etkisiz. Seçenekler: (a) D3D katmanında yakalamak, (b) lift sırasında bayt yaması uygulamak, (c) `override_patch.py` ile fonksiyonu C'de yeniden yazmak | Bayt yaması mekanizması henüz yok (`lift.py`/`genall.py`'de PATCH yok). |
| Kod mağarası (`MakeJMP`) | `override_patch.py` + `overrides.c` (`sub_X` → `sub_X_orig` + bizim sarmalayıcımız) | Bu mekanizma zaten var ve kullanılıyor. |
| d3d9 proxy / ReShade | `com.c` + `metal.m`'nin kendisi | Derinlik, tüm render hedefleri, gölgelendirici bytecode'u ve VS sabitleri zaten elimizde. |
| RTX Remix sahne yakalama | Çizim kuyruğumuz + guest belleğinden doğrudan kamera matrisi | Remix'in tahmin etmek zorunda kaldığı bilgiyi biz oyunun veri yapılarından okuyabiliriz. |
| Doku paketi (TPK değiştirme) | Değişiklik gerekmez | Aynı dosyaları okuyoruz. |

## 4. Yapabileceklerimiz: önerilen sıra

Bağlam: GPU karede 3,3 ms ile **%20 dolu**, darboğaz lift edilmiş x86 kodu
(bkz. kare bütçesi notu). Yani GPU tarafında **~13 ms boş bütçe** var. Aşağıdaki
efektlerin çoğu fiilen bedava.

### Aşama A: hızlı kazanımlar (gün mertebesi, nesnel doğrulanabilir)

1. **Yol yansıması çözünürlüğü (en büyük görsel kazanç).** `CreateTexture`
   içinde `usage&RT && 320×240` → cihaz boyutu (ya da `REFLRES` çarpanı).
   `NFSRES` için tam ekran hedeflerde yaptığımızın aynısı. Ek olarak bu hedefe
   bağlıyken `SetViewport`/`Clear` dikdörtgenleri de aynı oranla ölçeklenmeli.
   Örnekleme ekran uzayı koordinatıyla yapılıyorsa başka bir şey gerekmez.
   **Doğrulama:** `DUMPFRAME` ile ıslak zemin; RT boyutu loglanır.
2. **Küp haritası 128 → 512, ayna 256 → ekran/2.** `.data` yazımı, iki satır.
   Küp yüzü sayısı 6 olduğu için maliyet 6× yüz boyutu; GPU bütçesi var.
3. **MSAA (4×).** Oyunun `g_FSAALevel` ayarı var ama bizde örnekleme sayısı
   hiç yok. Ana hedef ve Z 4× olur, oyun ana hedefi örneklemeden önce
   (`StretchRect`, son işlem) `resolve` edilir. Widescreen Fix'in "HideAABug"
   notu (MSAA'da 1 px kenar hatası) burada da beklenebilir.
4. **16× anizotropik zorlama.** Kod var (`maxAnisotropy`), varsayılanı 16
   yapıp yol dokularında doğrulamak yeterli.
5. **Console Gamma / renk eşleme.** PC sürümü konsollardan daha soluk; WFP
   bunu son işlem gölgelendiricisiyle düzeltiyor (MIT). Sunumdan önce tek tam
   ekran geçiş.

### Aşama B: oyunun kendi efektlerini geliştirme (hafta mertebesi)

6. **Yansımalara eksik içeriği geri getirme** (HD Reflections'ın
   RestoreSkybox / RestoreHeadlights / LOD özellikleri). Kod yaması gerektiriyor:
   adresler imzayla bulunur, `overrides.c` ile uygulanır. GPL kodu kopyalanmaz,
   davranış yeniden yazılır.
7. **Daha iyi yol yansıması bulanıklığı.** Oyunun bulanıklığı düşük
   çözünürlüğe göre ayarlı ("yüksek çözünürlükte iyi ölçeklenmiyor"). Yüksek
   çözünürlükte mesafeye bağlı (pürüzlülük) Kawase bulanıklığı.
8. **Xbox yağmur damlaları.** Rain Droplets (MIT) mantığını taşı: yağmur
   nesnesinin yoğunluğunu (`+0x4D48`) ve tünel bayrağını (`+0x4D04`) oku,
   damlaları son işlemde çiz. Ofsetler struct içi olduğu için muhtemelen tutar,
   fonksiyon adresleri imzayla bulunmalı.
9. **Ekran uzayı efektleri** (ReShade'in yaptığı): SSAO (Z dokusu elimizde;
   projeksiyon `vs_pos_creg` sabitlerinden), SMAA/FXAA, CAS keskinleştirme,
   bloom. Hepsi tek kare içinde açılıp kapatılabilir (A/B ölçümü mümkün).
10. **HD doku değiştirme.** `LockRect/UnlockRect`'te XXH3 özeti → diskte
    `textures/<hash>.dds` varsa onu yükle (Dolphin/Remix yöntemi). Oyun
    dosyalarını değiştirmeden doku paketi ve AI ile büyütülmüş dokular.

### Aşama C: motor düzeyi, yeni yetenekler (ay mertebesi, araştırma)

11. **MetalFX.** Uzamsal ölçekleyici kolay (Retina 5K'ye bedava netlik).
    Zamansal ölçekleyici ve **macOS 26'daki kare ara değerlemesi** hareket
    vektörü ister. Bizim darboğazımız CPU olduğu için ara değerleme algılanan
    akıcılığı ikiye katlayabilir. Hareket vektörü için çizim başına önceki
    karenin WVP'sini saklamak gerekir (çizimleri kareler arasında eşlemek
    zor ama imkânsız değil).
12. **Hibrit ışın izlemeli yansımalar (Metal RT).** Remix'in yaptığını seçici
    yaparız: araç boyası ve ıslak yol için gerçek yansıma. Gereken: dünya uzayı
    geometrisi. Oyun birleşik WVP gönderiyor, ama kamera ViewProj'u guest
    belleğindeki eView yapısından okunursa World = WVP·(VP)⁻¹ çıkarılabilir.
    Remix'in bu bilgiyi elde etmekte zorlandığı yerde bizim avantajımız var.
    **Kısıt:** M1 Pro'da donanım RT yok (M3+ var); M1'de yazılım RT yavaş olur.
    Remix'in de yaşadığı ayıklama sorunu bizde de var (ekran dışı nesneler hiç
    gönderilmiyor; yansımada eksik görünür). Ayıklamayı override ile gevşetmek
    gerekir.
13. **Dinamik ışıklar** (neonlar, sokak lambaları, farlar gerçek ışık olarak).
    Remix modunun en etkileyici kısmı bu, ama elle ışık yerleştirme isteyen
    bir içerik işi. Önce 12. madde.

## 5. Yapmamamız gerekenler / riskler

* **GPL kodu kopyalamak.** HD Reflections GPL-3. Adres, sabit ve davranış
  bilgisi kullanılabilir, kod yeniden yazılmalı. WFP ve Rain Droplets MIT
  (atıf yeterli).
* **Mod adreslerini doğrudan kullanmak.** Exe farklı (bkz. §2).
* **Öznel hedefle başlamak.** Grafik doğrulama sınırı notu geçerli. Her madde
  için nesnel sinyal tanımlı: RT boyutu logu, `DUMPFRAME` karşılaştırması,
  tek koşu içinde açılıp kapatılabilir anahtar, `metal_budget_report` maliyeti.
* **Bellek havuzu yamaları (ExpandMemoryPools).** Modun `0x8F5790` adresi
  bizim exe'de isimsiz 5. bölüme (reloc verisi) düşüyor. Bu yama bizde
  doğrulanmadan uygulanamaz.

## 6. Uygulandı: HD build ve yansıma çözünürlüğü (2026-10-04)

**Yapı.** `link.sh` artık iki ikili üretiyor: `out/nfsu2_boot` (orijinal)
ve `out/nfsu2_hd_boot` (HD). Shard'lar ortak, fark yalnızca runtime'da:
`runtime/enhance.{h,c}` ve `NFS_ENHANCED` ile korunan kancalar. Orijinal
ikilide HD kodu yok (`strings | grep '[HD]'` → 0). `./run.sh hd`,
`./pack.sh --hd` → `NFSU2 HD.app` (ayrı paket kimliği, ayrı log
`nfsu2-hd.log`).

**Yöntem.** Oyunun mantığına dokunulmuyor: hedefler `CreateTexture` /
`CreateCubeTexture`'ın **dönüş adresinden** tanınıyor, yalnızca Metal dokusu
büyütülüyor, oyun eski boyutu görmeye devam ediyor. Viewport'u zaten hedefin
tamamına kurduğumuz için sahne büyük dokuyu dolduruyor.

| Hedef | Çağrı | Orijinal | HD varsayılan | Ortam |
|---|---|---|---|---|
| yol yansıması (+ bulanıklık hedefi) | `0x5BA0A7`, `0x5BA11D` | 320×240 | geri tampon (1080p'de 1920×1080) | `HD_ROAD=1.0` |
| dikiz aynası | `0x5BA1DD` | 256×256 | 1024×1024 | `HD_MIRROR` |
| araç küp haritası | `0x5BA559` | 128/yüz | 512/yüz | `HD_CUBE` |

`HDREFL=0` HD build içinde hepsini kapatır (A/B için).

**Bulanıklık tuzağı (ölçüldü).** Oyun yol yansımasını ps 19 + vs 5 ile iki
geçiş bulanıklaştırıyor (13→14→13): 4 örnek, ofsetler 320 piksele göre UV'de
sabit. Doku büyüyünce örnekler 6 piksel aralıklı düştü ve **dikey çizgili
desen** çıktı. Aynı ps/vs çifti parlama zincirinde de (640×480 hedefler)
kullanıldığı için düzeltme yalnızca "yol hedefinden yol hedefine" çizimlere
uygulanıyor:
yol hedefleri tam mip zinciriyle yaratılıyor, bulanıklık çiziminden önce
kaynağın mip'leri üretiliyor (`generateMipmapsForTexture`), gölgelendirici
L seviyesinden okuyor ve örnek ofsetlerini merkeze göre `s = 2^L/ölçek` ile
ölçekliyor (P.pad0/pad1; diğer her çizimde 0). Örnek aralığı her zaman ~1
teksel kalıyor. `HD_BLUR`: 1 = oyunun bulanıklık genişliği (daha pürüzsüz),
0 = keskin, **varsayılan 0,5**.

**Doğrulama** (Quick Race, 1080p, kare 6200, `DUMPTEX=13`): yansıma dokusu
320×240 → 1920×1080, çizgili desen yok, üç `HD_BLUR` değeri beklendiği gibi
sıralanıyor. Z boyut uyumsuzluğu 0. GPU (koşu ortalaması): 2,84 → 3,50 ms/kare.

**Açık kalanlar:**
* Resort Loop kuru pist; son karede yol yansıması çok az karışıyor, fark
  yansıma dokusunda net ama tam karede küçük. **Islak zeminde/yağmurda**
  görsel karşılaştırma gerekiyor.
* Dikiz aynası bu kamerada görünmüyor; 1024'lük doku yaratılıyor ama içerik
  doğrulanmadı.
* Küp haritası 512'de mip'siz örnekleniyor; küçük arabalarda kıpırtı
  (aliasing) olabilir. Gerekirse küp için de mip zinciri.

## 7. Uygulandı: 4× MSAA (HD build, 2026-10-06)

`HD_MSAA=4` varsayılan (2/8 da olur, 0/1 kapalı). Kodu `metal.m`
("HD: ekran için MSAA").

* **Yalnızca ekran geçişleri çok örnekli.** Sahne ve arayüz doğrudan arka
  tampona çiziliyor; render hedefleri (yansıma, son işlem, küp) tek örnekli
  kalıyor. Her ekran geçişi `StoreAndMultisampleResolve` ile drawable'a
  çözülüyor, böylece StretchRect'in arka tampondan okuduğu kopya her an
  çözülmüş görüntü. Ekrana yapılan blit'ler (yarışta 0) çok örnekli dokuya
  dörtgen olarak çiziliyor.
* **Boru hattı anahtarında bit 48 = örnek sayısı.**
* **Z tuzağı (ölçüldü).** Her Z yüzeyinin 4× ikizi tutuluyor. İlk sürümde
  ikizler içerik paylaşmıyordu ve oyunun **derinlik testli tam ekran
  dörtgeni** (ps 17 / vs 11, 1920×1080 hedefe, ekranın Z'siyle) karede ~11
  kez sahnenin derinliğini göremiyordu. Çözüm: MSAA geçişi derinliği de tek
  örnekli Z'ye **çözüyor** (`depthResolveFilter = Sample0`;
  `HD_ZRESOLVE=0` kapatır). Bekleyen `Clear(Z)` iki ikiz için ayrı
  tüketiliyor. Ters yön (tek örnekli geçiş Z yazıp ekranın okuması) sayaçla
  izleniyor: 0.
* **Doğrulama.** Metal doğrulama katmanı temiz. MSAA açık/kapalı farkı
  yalnızca geometri kenarlarında; düz alanlarda ortalama fark 0,9/765.
* **Maliyet** (A/B/A/B, `tools/bcmp.py`, yarış, 1080p): kare süresi
  **+%0,2** (gürültü, ~119 fps); GPU meşgul ~+1,0 ms/kare.
* **Sınır.** Alfa testli yapraklar ve çitler (gölgelendiricide `discard`)
  MSAA ile yumuşamaz. Sıradaki adım: bu çizimlerde alpha-to-coverage +
  alfa keskinleştirme.

## 8. Uygulandı: SSR + yansıma ayrıştırma altyapısı (HD build, 2026-10-07)

Amaç iki katlı: bugün ekran uzayı yansıma (SSR), ileride aynı altyapı
üzerinde ışın izlemeli yansıma. `HD_SSR=0` kapatır, `HD_SSR_DEBUG=1`
görselleştirir (yeşil isabet, mor oyunun düz yansımasına düşüş, kırmızı
kenar sönümü). `HD_SSR_STEPS=48`, `HD_SSR_MAXD=120`.

**Kamera (ölçüldü).** Oyun `SetTransform`'a yalnızca birim matris veriyor;
matrisler doğrudan VS sabitlerine yazılıyor. Kaynak `sub_005c7ed0`
(görünüm başına projeksiyon kurulumu; thiscall, ecx = matris yapısı,
`[esp+4]` = eView). `eView+4` kimlik (1 ana), `eView+0x40` Camera*
(`+0xbc` near, `+0xc0` far, `+0xc4` FOV); `ecx+0` görünüm, `+0x40`
projeksiyon, `+0x80` çarpım. `overrides.c` sarmalayıcısı ana görünümün
matrislerini kareye işliyor. Doğrulama: çarpım, dünya geometrisi
çizimlerinin `c0..c3` sabitleriyle aynı (x sütunu ×0,75 = bizim Hor+).
Near 0,5; dünya yukarısı +Z.

**Yansıma nerede (ölçüldü).** Yol: ps 5 / ps 6 yol yansıması dokusunu
1./3. aşamada örnekliyor; araba: ps 265 küp haritayı 0. aşamada. Üçü de
yansıma örneğinde **doğrusal**, arada `saturate` yok:
`çıkış = A + B · yansıma` (yol için B = doku alfası × köşe alfası).

**Ayrıştırma.** HD'de bu çizimler yansıma varyantına geçiyor (ps +
512·(1+aşama), ihtiyaç anında üretiliyor). Gövde üç kez çalışıyor
(gerçek / yansıma=0 / yansıma=1) ve G-tamponuna `B = o₂ − o₁` (sis
dahil, RGBA16F) ve orijinal `R0` (RGBA8) yazılıyor. Diğer opak çizimler
sıfır yazıyor (önde kalan yüzey yolun verisini örter), karışımlılar
yazmıyor. Ana sahne bitince (ekrandan ilk StretchRect) birleştirme geçişi
derinlikte ışın yürütüyor; isabette `renk + B·(R_yeni − R0)`, değilse
oyunun düz yansıması kalıyor (ekran dışı içerik için doğru yedek).
**Işın izleme geldiğinde yalnızca `R_yeni`'nin kaynağı değişecek.**

**Doğrulama.** Metal doğrulama katmanı temiz; karede 1 birleştirme,
~26 yansıma çizimi. Görselleştirmede yol yüzeyinin çoğu isabet; ışın
arabanın altına girdiğinde ve ekran kenarında düşüş.

### 8b. Araba boyası (2026-10-11)

`HD_SSR_CAR=0` kapatır, `HD_SSR_CARLOD=1.0` yansıma yumuşaklığı (sahne
kopyasının mip seviyesi).

* **Varyant:** ps 9'un küp varyantı (265) → 777. Üçüncü G-tamponu hedefi
  (RGBA16F) küp örnekleme vektörünü taşıyor; bu, oyunun köşe
  gölgelendiricisinde hesapladığı yansıma yönü.
* **Yön uzayı (ölçüldü, tuzak).** Küp haritanın altı yüz kamerası
  (görünüm 16–21) dünya eksenlerine DEĞİL, kamera/araba çerçevesine
  hizalı. Küp vektörü: +x = sağ, +y = geri, +z = yukarı. Dünyaya çeviri
  yüz kameralarının ileri yönleriyle yapılıyor: +x → görünüm 17,
  +y → görünüm 18, +z → görünüm 21 (her karede `sub_005c7ed0`'dan).
* **Ölçüm tuzağı.** Görselleştirme renkleri oyunun son işleminden
  (ton, parlama) geçiyor; renkten sayı okumak YANILTIYOR (ilk denemede
  +z'yi aşağı sandım). Güvenilir test: `HD_SSR_DEBUG=3` — yansıma
  yönünden türetilen normal `normalize(R − V)` ile derinlikten gelen
  geometrik normalin uyumu, ikili renkle (yeşil uyumlu / kırmızı zıt).
  Sonuç: gövdenin %62'si uyumlu; kırmızı bantlar ince çıkıntılar
  (yumuşatılmış köşe normali ≠ yüzey normali).
* **Kendine çarpma:** ışın, `N = normalize(R − V)` boyunca 4 cm
  ötelenerek başlıyor; arabada adım 8 cm, menzil 60 m.
* **Sonuç.** Arkadan bakışta bagaj ve tampon arkadaki yolu yansıtıyor
  (isabet); tavan ve cam gökyüzüne bakıyor, küp harita kalıyor.
* **Sınır.** SSR yalnızca ekranda görüneni yansıtabilir; araba boyasının
  çoğu yukarı/yana bakar. Burada asıl kazanç ışın izlemeyle gelecek —
  aynı G-tamponu (B, R0, yön) doğrudan kullanılacak.

## 9. Ölçüldü: ışın izleme bu makinede ne kadar sürer (2026-10-11)

`HD_RTPROBE=1` + `DUMPFRAME=<kare>`: seçilen karede bir kez çalışır, rapor
basar, iki PNG yazar (`…99001` birincil ışınlar, `…99002` yansıma
ışınları). Kod `metal.m` "ISIN IZLEME SURE OLCUMU". Apple M1 Pro
(donanım RT yok), 1920×1080, Quick Race kare 6200, iki koşu:

| Adım | Süre |
|---|---|
| Yakalama (CPU): 518 çizim → **18.989 üçgen**, dünya uzayı | **0,20 ms** |
| İvme yapısı, varsayılan kurulum | 2,6 ms |
| İvme yapısı, hızlı kurulum (`PreferFastBuild`) | 1,7 ms |
| İvme yapısı, yeniden uydurma (refit) | **0,13 ms** |
| Birincil ışın, tüm pikseller (2,07 M ışın) | 3,05 ms (~680 M ışın/sn) |
| **Yansıma ışını, yol + araba pikselleri (852 bin ışın)** | **2,0 ms** |

**Sonuç.** En kötü durumda (her karede tam yeniden kurulum + yansıma
ışınları) ~4 ms GPU. Kare 8,35 ms (CPU sınırlı), GPU şu an ~5,8 ms meşgul;
yani ışın izlemeli yansıma 60 fps'in çok üstünde kalır. Sebep sahnenin
küçüklüğü: 2004 oyunu, görüş alanında ~19 bin üçgen.

**Yakalama yöntemi.** Köşe gölgelendiricisi çalıştırılmıyor:
`oPos = p·WVP` olduğundan `dünya = p·(WVP·(VP′)⁻¹)`; WVP çizimin
`c[cx..cx+3]` sabitleri (`vs_pos_creg`), VP′ Hor+ uygulanmış kamera.
Çarpım çift duyarlıkta (near 0,5 / far ~16 km iptal hatası için).
Birincil ışın görüntüsü oyunun karesiyle örtüşüyor → yakalama doğru.

**Bu ölçümün KAPSAMADIĞI (asıl kalan iş):**
* **İsabet noktasının gölgelenmesi.** Ölçüm yalnızca kesişim. Renk için
  üçgen başına doku + UV + köşe rengi gerekir. Oyunun aydınlatması köşe
  rengine pişmiş (ps 5: `2·doku·köşe rengi`), yani ışık hesabı gerekmez;
  maliyet doku dizisi + birkaç örnekleme.
* **Yol yapıda yok.** vs_4 / vs_14 konumu `c[cx..cx+3]` kalıbıyla
  üretmiyor (45 çizim atlandı); bu iki VS için konum yolu ayrıca çözülmeli.
* **Görüş dışı geometri yok.** Oyun kameranın arkasını ayıklıyor;
  arabanın arkasındaki sahne yansımada eksik kalır (Remix'in de sorunu).
  Çözüm: ayıklamayı override ile gevşetmek ya da yedek olarak küp harita.
* **Alfa testli geometri** (ağaç, çit) opak sayıldı; doğrusu kesişimde
  doku alfası okumak (daha yavaş).
* Z yazmayan 116 çizim (ışık hüzmeleri, parçacık) bilerek dışarıda.
* Ölçüm karesinin görüntüsü bozuk çıkar: ölçüm saniyelerce beklediği için
  o karenin çizim tamponunun üzerine yazılıyor (yalnızca tanı yan etkisi).

## 10. Uygulandı: ışın izlemeli yansıma, eksiksiz (HD build, 2026-10-11)

§9'daki ölçüm yalnızca kesişimdi ve dört şeyi eksik bırakıyordu (isabet
gölgeleme, yol, görüş dışı geometri, alfa testi). Hepsi yapıldı; ışın
izleme artık HD build'de **varsayılan** (`HD_RT=0` kapatır, SSR'ye düşer).
Kod: `metal.m` "RT cekirdegi" + "SABIT SAHNE ONBELLEGI", `vsemu.c`
`vs_to_msl` (yakalama çekirdeği), §8'in G-tamponu.

### Boru hattı (kare başına)

1. **Yakalama (GPU).** Her köşe gölgelendiricisinin gövdesi ikinci kez,
   hesaplama çekirdeği `vsk_N` olarak üretiliyor. Çekirdek çizimin ham köşe
   verisini ve sabitlerini okuyup oyunun VS'ini AYNEN çalıştırır; çıktıdan
   dünya konumu (`görüş = (x/P00′, y/P11, w)`, `dünya = görüş·V⁻¹`: z−w
   iptaline girmez) ve öznitelikler (köşe rengi, 3 UV kümesi, sis) yazılır.
   Böylece yol (vs_4/vs_14), kemikli modeller ve UV üreten VS'ler özel durum
   gerektirmiyor: "VS ne üretiyorsa o".
2. **Malzeme.** Çizim başına: 3 doku, PS sınıfı, alfa referansı, PS sabiti
   c0, sis rengi, izdüşüm kipleri. Dokular kare başına 512'lik bağsız
   (argument buffer) tabloya yerleşir.
3. **İvme yapısı.** İki alt yapı + örnekli üst yapı: *anlık* (her kare
   hızlı kurulum; arabalar, kemikliler, yeni görünenler) ve *sabit depo*
   (aşağıda).
4. **İzleme (hesaplama çekirdeği `hd_rt_kern`).** G-tamponunda yol ya da
   araba sınıfı taşıyan her piksel için yansıma ışını: yolda normal +Z,
   arabada oyunun küp arama vektöründen (`N = normalize(R − V)`). İsabette
   oyunun PS'i yeniden kurulur: `2·doku·köşe rengi`, far ışığı alan
   yüzeylerde (ps 4/6) `ışık dokusu × sönüm rampası × c0 + köşe rengi`,
   sonra oyunun sis formülü. **Alfa testli** yüzeyde (ağaç, çit) doku
   alfası referansın altındaysa ışın yüzeyin içinden devam eder (5 kata
   kadar). Doku mip'i ışın uzunluğundan.
5. **Birleştirme.** §8'deki ayrıştırma: `renk + B·(R_yeni − R0)`. Iska
   eden ışında oyunun kendi yansıması kalır.

### Yarım çözünürlük

Çekirdek tek başına 4,55 ms sürüyor ama kare içinde tam çözünürlükte GPU
~15 ms'ye çıkıp oyunu GPU'ya takıyordu (~74 fps). Işınlar yarım
çözünürlükte atılıyor (`HD_RT_SCALE=2`; 1080p'de varsayılan, daha yüksek
çözünürlükte ölçek otomatik büyür, bkz. §11), birleştirmede
**derinliğe ve sınıfa duyarlı** 4 örnekli büyütme: araba kenarı yola
taşmıyor. `HD_RT_SCALE=1` tam çözünürlük.

### Sabit sahne önbelleği: kameranın arkası

Oyun kameranın görmediğini çizmiyor; arabanın arka camı/bagajı tam da
kameranın arkasını yansıtır. Çözüm oyunun ayıklamasına dokunmak değil,
görüleni **hatırlamak**:

* Çizim = (ham köşe verisinin özeti `gh`, dünya matrisi `W`). Aynı çizim
  12 kare **art arda** aynı yerde görülürse sabit sayılır; yakalanan
  köşeleri GPU'da (CPU'ya geri okuma yok) sabit depoya kopyalanır ve anlık
  yakalamadan çıkar. Sabit alt yapı en fazla 30 karede bir yeniden kurulur.
* Arabalar (küp haritalı PS) ve kemikliler hiç girmez.
* Anahtar `gh` + doku; **VS kimliği yok**: aynı parçanın far ışığı alan
  (vs_13/vs_14 + ps 4/6) ve almayan çizimi tek girdidir.
* **Far ışığı dondurulmaz.** Sabit kopya ışıksız taban olarak saklanır.
  Parça o karede ışıklı çiziliyorsa çizimin ışık matrisi (VS sabitleri
  `c[K..K+3]`, `vs_light_creg` ile bulunur; nesne → ışık) `W⁻¹` ile dünya
  uzayına çevrilip malzemeye yazılır; çekirdek far terimini isabet
  noktasında kurar (`oT1 = p·L`, `oT2.x = 0,005·q.w + 0,5`). Çizilmeyen
  parçada far terimi yoktur. Sis rengi, PS sabiti vb. de görüldüğü her
  karede yenilenir.
* **Atma kuralları:** (a) *uzak*: 900 birimden uzak ve 30 karedir
  görülmüyor (800'den uzaktaki hiç terfi etmez); (b) *yakın*: kameranın
  hemen önünde (< 50 birim), görüşün ortasında ve 60 karedir çizilmiyor
  = gerçekten kalkmış; (c) *kardeş*: aynı geometri aynı noktada başka
  dokuyla ya da başka yönelimle çizildi (animasyonlu doku, kameraya dönen
  ağaç panosu, dönen nesne) → eskisi atılır.
* **Oynaklık:** her kardeş değişimi girdinin terfi eşiğini ikiye katlar
  (12, 24, … 768 kare); sürekli değişen şey anlık yakalamada kalır.
* Ölü aralıklar deponun %40'ını geçince canlılar GPU'da öbür tampon
  çiftine sıkıştırılır (ping-pong; depo sıfırlanmaz).
* Sabit isabetlerde sis kayıttaki değil, o anki kameraya göre yeniden
  hesaplanır (`vs_fog_creg` ile bulunan sis sabiti).

**Burada dört hata bulundu, dördü de ölçümle** (`HD_RT_LOG`, `HD_RT_DUMPTAB`,
`HD_SSR_DEBUG=4` ile önbellek açık/kapalı görüntü farkı):

1. *"Görüş alanında ama çizilmiyor → at" kuralı yanlıştı.* Oyun görüş
   alanının içindekileri de ayıklıyor (küçük nesneyi uzakta çizmiyor).
   Kamera gezerken 300 karede 300+ haksız atma/yeniden terfi.
2. *W'nin duyarlığı.* `W = M·(VP′)⁻¹` homojen bileşeni
   `(z − Q·w)/(−Q·near)` iptalinden ~1e-4 hatayla veriyor; kamera konumuyla
   (yüzlerce birim) çarpılınca W'nin ötelemesi kameraya göre 0,03–0,1 birim
   oynuyordu. Aynı nesne her kamera açısında ayrı girdi sayılıp depoda üst
   üste yığıldı: **1771 girdinin 878'i kopya**. Düzeltme: yakalama
   çekirdeğiyle aynı yol, homojen sütun tam (0,0,0,1).
3. *Kameraya dönen panolar.* Ağaçlar kameraya dönüyor; kamera her
   durduğunda yeni yönelim ayrı "sabit" oluyordu (bir ağacın 12 kopyası).
   Alfa testli kopya yığınları ışını defalarca yeniden attırıp GPU'yu
   **8 → 22 ms**'ye çıkardı (oyun ~45 fps'e düştü). Kardeş kuralı
   yönelimi de kapsayacak şekilde genişletildi → 8 kopya (hepsi meşru).
4. *Dondurulmuş far ışığı.* Sabit kopya ışık UV'lerini terfi anındaki
   haliyle taşıyordu: rakipler start çizgisinden kalktıktan sonra yol
   yansımada parlak kalıyordu (araba bölgesinde önbelleksiz referansa göre
   ortalama fark 10/255). İlk düzeltme (ışıklı çizimleri her kare yeniden
   yakala) işe yaramadı: oyun yakın sahnenin neredeyse tamamını ışıklı VS
   ile çiziyor (yarış başında 255 çizim, hepsi oyuncunun farı: doku 204),
   GPU 7,1 → 10,9 ms. Kalıcı çözüm yukarıdaki dünya uzayı far terimi:
   fark 0,8/255, GPU 7,0 ms.

### Ölçüm (M1 Pro, 1920×1080, 4× MSAA, Quick Race, 7500 kare)

| | GPU meşgul (ort) | yarış kare süresi (medyan) | kare başına üçgen | anlık çizim |
|---|---|---|---|---|
| RT kapalı (`HD_RT=0`, SSR) | 5,87 ms | 8,3–8,4 ms | – | – |
| RT, önbellek kapalı (`HD_RT_CACHE=0`) | 8,08 ms | 8,3–8,6 ms | 19 375 | 406 |
| **RT, önbellek açık (varsayılan)** | **7,06 ms** | **8,3–8,4 ms** | 34 716 (8 692 anlık + 26 024 sabit) | 134 (yarışta ~110) |

Kare süresi üçünde de aynı (oyun CPU sınırlı, ~120 fps); ışın izlemenin
bedeli GPU'da ~1,2 ms. Önbellek hem kameranın arkasını getiriyor hem de
her karede yakalanan çizimi 406'dan ~110'a indirdiği için GPU yükünü
düşürüyor (8,08 → 7,06 ms). Yarış başında (kamera sabit) çalkantı sıfır:
300 karede 0–5 terfi, 0–2 atma; koşu boyunca 933 terfi, 66 atma, 33 sabit
kurulum, 0 sıfırlama. Doğrulama katmanı (`MTL_DEBUG_LAYER=1`) temiz;
sıkıştırma ve yeniden özetleme yolları `HD_RT_STRESS=400` ile zorlanarak
(14'er kez) aynı koşuda sınandı.

**Kameranın arkası gerçekten geliyor mu?** `HD_SSR_DEBUG=4` (yalnız
yansıma; ıska = lacivert), kare 7400, arabanın gövdesi (280×290 piksel):
ıska eden piksel önbellek kapalıyken 7 720, açıkken 6 848. Yani ıskaların
%11'i artık kameranın görmediği geometriye isabet ediyor (tamponun alt
kenarı, köşeler); kalan ıskalar gökyüzü (Z yazmadığı için yapıda yok →
oyunun küp haritası). Bu sahnede kazanç küçük, çünkü arka yüzeylerin çoğu
kamera ile araba arasındaki yolu yansıtıyor; asıl fark sürüş sırasında
arkada kalan binalarda beklenir (aşağıda "Sınırlar").
Görüntüler: `docs_img/rt_acik_kapali.png` (RT kapalı/açık),
`docs_img/rt_yalniz_yansima_onbellek.png` (yalnız yansıma; üstte önbellek
açık, altta kapalı), `docs_img/yol_horplus_duzeltme.png`.

### Yol Hor+ hatası (iki build'i de etkiler)

Yakalama sırasında bulundu: `vs_pos_creg` konumu geçici yazmaç üzerinden
kuran VS'leri (`dp4 rN.c, v0, cK` + `mov oPos, rN`: yol, vs_4/vs_14)
tanımıyordu → yol Hor+ düzeltmesini almıyor, 16:9'da %33 yatay geniş
çiziliyordu (start çizgisi damaları ve şerit çizgileri kayık). Geçicileri
izleyecek şekilde yeniden yazıldı; orijinal build de düzeldi.

### Sınırlar (dürüstçe)

* **Tek sekme, keskin yansıma.** Pürüzlülük yok; yolun bulanıklığı oyunun
  kendi B teriminden geliyor. Yansımanın içinde yansıma yok (isabet edilen
  arabanın boyası küp haritasız, düz doku × köşe rengi).
* **Yol normali +Z varsayılıyor** (eğimli rampalarda yansıma hafif kayar).
* **Hiç görülmemiş geometri yok.** Önbellek yalnızca kameranın bir kez
  gördüğünü hatırlar; ilk kez gidilen yolda arkadaki bina, kamera oraya
  hiç bakmadıysa yansımada yoktur (ıskada oyunun küp haritası kalır).
* **Z yazmayan çizimler** (ışık hüzmeleri, parçacık, hâle) yapıda yok.
* **Hareketli test yok.** AUTOSEQ arabayı süremiyor; ölçümler yarış
  başındaki sabit kamera + yarış öncesi kamera gezintisi (hareketli kamera,
  300 karede ~480 terfi) üzerinden. Sürüş sırasındaki davranış elle
  denenmeli.
* macOS 13+ gerekir (ışın izleme API'si); eskide SSR'ye düşer.

## 11. Ölçüldü ve düzeltildi: HD build'de GPU süresi neye gidiyor (2026-10-11)

Soru: "çizim süresi neye harcanıyor, gereksiz iş yükü var gibi". Cevap
tahminle değil iki araçla verildi (ikisi de iki build'de var):

* `GPUWIN=a,b`: GPU/CPU-kodlama ortalaması yalnızca `[a,b)` kareleri için
  (menüler ortalamayı sulandırmasın). Yarış: `GPUWIN=6000,7300`.
* `PASSPROF=N` (+ `PASSPROF_MODE=1`): penceredeki her N. karede **her
  geçişin** GPU süresi, Metal sayaç örnekleyicisiyle; köşe ve parça aşaması
  ayrı. İlk profilli karenin geçiş sırası ve içeriği de dökülür.
* Yardımcılar: `HD_COMP=0` (birleştirmeyi atla), `HD_RT_AB=1` (aynı koşuda
  ışın izleme adımlarını 100'er karelik dilimlerle aç/kapa).

**Ölçüm tuzakları (hepsi yaşandı):**
* Hesaplama/blit/ivme geçişlerini örneklemek (`PASSPROF_MODE=3`) GPU'yu ~6×
  yavaşlatıyor; yalnızca render geçişleri (`MODE=1`) ölçümü bozmuyor.
* Sayaç örnekleri Apple GPU'sunda doğrudan ns; `sampleTimestamps` ise mach
  tiki (41,67 ns). Karıştırınca süreler 41× çıkar.
* GPU hafif yükte frekans düşürüyor: aynı geçiş orijinal build'de HD'dekinden
  uzun "sürüyor". ms farkları özellik bedelini olduğundan KÜÇÜK gösterir.
* 1080p'de GPU boşta kalıyor (kare CPU sınırlı, 8,3 ms) ve her şey "bedava"
  görünüyor. Asıl bedel ekranın gerçek çözünürlüğünde (3024×1964) çıkıyor.
* Hava durumu rastgele: yağmurlu koşu bambaşka bir yük (aşağıda).

### Karenin yapısı (yarış, HD varsayılan, düzeltmeden önce)

| sıra | geçiş | not |
|---|---|---|
| 0 | EKRAN +MSAA +G | **yalnızca Clear** |
| 1–6 | yol yansıması (ekran boyutu) + bulanıklık + 2 mip üretimi | |
| 7–12 | küp harita, 6 yüz 512² | |
| 13 | EKRAN +MSAA +G | sahne: 674 çizim |
| 14 | ekran dışı, ekran boyutu | ışık parlamaları: sahnenin Z'sini test eder, Z yazmaz |
| 15 | EKRAN +MSAA +G | **tek çizim** |
| 16–20 | ışın izleme (yakalama, yapı, çekirdek) + birleştirme | |
| 21–38 | son işlem zinciri (640×480 ve ekran boyutu) | |
| 39–40 | EKRAN +MSAA | son işlem dörtgeni, arayüz |

Beş çok örnekli ekran geçişinin HER biri renk + 3 G-tamponu + Z'yi 4×
olarak saklayıp çözüyordu. 1080p profilinde ekran geçişleri 4,6 ms / 9,7 ms
(%48); ışın izlemenin kendisi bundan küçük.

### Üç düzeltme (`metal.m` "MSAA: gereksiz saklama / cozme")

1. **Yalnız Clear içeren ekran geçişi açılmıyor** (sonraki ekran geçişi ilk
   kullanımda zaten temizler).
2. **Tembel saklama/çözme.** Çözme kararı geçiş açılırken (Metal kuralı:
   çözme hedefi bağlıysa eylem çözme içermeli), saklama kararı kapanırken
   veriliyor: sırada başka çok örnekli ekran geçişi varsa yalnız sakla,
   ekranı okuyan kopya ya da kare sonu varsa yalnız çöz; G-tamponu yalnız
   birleştirmeden hemen önce çözülür. Ekrandan ilk kopyadan sonraki ekran
   geçişleri (son işlem dörtgeni, arayüz) ve birleştirme tek örnekli: orada
   tüm örnekler zaten aynı değeri taşıyor. `HD_MSLAZY=0` eski davranış.
3. **Sahneyi bölen ara geçiş erteleniyor** (`hd_reorder`). Kalıp
   `ekran | R (ekran dışı, Z yazmaz) | S (ekran, Z yazmaz, R'nin hedefini
   örneklemez)` ise R ile S yer değiştirir: sonuç piksel piksel aynı, sahne
   tek geçiş. `HD_REORDER=0` kapatır.

Çok örnekli ekran geçişi kare başına 5 → 1. Görüntü aynı (menü ve yarış
karelerinde eski/yeni ortalama fark ≤ 0,5/255, zamanlama gürültüsü
düzeyinde), kenar yumuşatma korunuyor, doğrulama katmanı temiz, tahmin
tutmayan geçiş sayacı 0.

### Sonuç

1080p, yarış penceresi, GPU ms/kare (kare süresi hepsinde 8,3–8,4 ms):

| | önce | sonra |
|---|---|---|
| orijinal build | 3,6–3,9 | (değişmedi) |
| HD: yansıma çözünürlüğü + MSAA | 6,0–6,3 | 5,3 |
| HD: + SSR | 6,8 | 5,8 |
| HD: + ışın izleme (varsayılan) | 9,3–9,9 | **6,2** |

Ekranın gerçek çözünürlüğü 3024×1964, kuru hava, kare hızı:

| | önce | sonra |
|---|---|---|
| orijinal build | 120 fps | – |
| HD, ışın izleme kapalı | 56 fps | **medyan 120 fps** (ortalama 9,1 ms ≈ 110 fps) |
| HD, ışın izleme açık, ışın ölçeği 2 | 40 fps | 60 fps (120 Hz ekranda 8,3 ms aşılınca 16,7 ms'ye yuvarlanıyor) |
| HD, ışın izleme açık, **otomatik ölçek** | – | **medyan 120 fps** (8,3 ms; ortalama 10,8 ms, p99 17,5 ms: ara ara 60'a düşüyor) |

### Otomatik ışın ölçeği

`HD_RT_AB=1` ile aynı koşuda adım adım (3024×1964, kuru, ölçek 2, kare
süresi medyanı): ışın izlemesiz 8,3 ms; ışınlar atılınca 12,8 ms;
birleştirmeyle 16,7 ms. Işın sayısı çözünürlükle büyüyordu (yarım
çözünürlük = 1512×982 = 1,48 M ışın; 1080p'de 0,52 M).

`HD_RT_SCALE` verilmediyse ölçek artık çözünürlüğe göre seçiliyor
(`(genişlik + 480) / 960`, 2–4 arası): 1080p'de 2, 2560 ve 3024'te 3, 4K'da
4. Işın tamponu hep ~1000 piksel genişliğinde kalıyor. 3024×1964'te ölçek 3
ile tüm adımlar açıkken kare 8,2 ms; yalnız-yansıma görüntüsünde ölçek 2'ye
göre ortalama fark 1,1/255 (derinliğe duyarlı büyütme sayesinde).

**Ölçüm notu:** kare 8,3 ms'nin altındayken iki komut tamponu aynı anda
uçuşta olur ve `GPUEndTime − GPUStartTime` kuyruk beklemesini de sayar
(ölçek 3 koşusunda 21 ms "GPU" ama 8,2 ms kare). GPU sınırına yakınken
doğru ölçü **kare süresi**dir.

### Çözüldü: "yağmurda 17 fps" aslında zehirli ivme yapısıydı (2026-10-11, gece)

Önceki bölümde "yağmurlu koşularda ~17 fps, neden bilinmiyor" yazıyordu.
Yağmur DEĞİLDİ. Bulunma sırası (yanlış izler dahil, tekrar düşülmesin):

* `FORCERAIN=1` ile yağmur zorlandı (aşağıda): yağmurda bile 80 fps. Yağmur
  suçsuz.
* Yavaş koşularda fazladan bir köşe gölgelendiricisi kayıtlıydı (17 yerine
  18–19). Bu neden değil SONUÇTU: koşu yavaş olunca daha uzun sürüyor, oyun
  saati 60. saniyeye varıyor ve gökyüzü geçişi (`skymorph.efx`, vs_18)
  başlıyor. `FORCETOD=1` ile geçiş zorlandı: yavaşlık yok.
* `HD_RT_AB=1` ile gerçek bir yavaş koşu yakalandı (13 kısa koşuda 1):
  ışın izlemesiz 8,3 ms; **düz ışın atma** (gölgeleme yok) açılınca 58 ms;
  yakalama + yapı kurulumu 8,3 ms. Çizim/üçgen sayıları normal → ivme
  yapısının kendisi bozuk.
* **Kök neden:** yakalanan köşelerin bir kısmı NaN / sonsuz ya da akıl
  dışı uzak. Yarış sahnesinde kare başına ~450 NaN'li + ~200 "100 km'den
  uzak" üçgen; çoğu araba gölgelendiricisinden (vs_12), bir kısmı kemikli
  modellerden (vs_15), azı dünya çizimlerinden (vs_3/vs_13). Bunlar ekranda
  görünmeyen çizimler (büyük olasılıkla oyunun bozuk/boş matrisle bıraktığı
  gizli parçalar; rasterleştirici atıyor, yakalama atmıyordu).
* **Nedensellik testi** (`HD_RT_POISON=<değer>`: denetim kapalı, tek bir
  köşe x = değer'e taşınır), 3024×1964, kare medyanı: 1e7 → **71 ms**;
  1e12 → 16,6 ms. Yani tek bir köşenin ~10⁷ birim uzakta olması yapının
  kutularını işe yaramaz hale getiriyor (kök kutu devleşince düğüm
  sınırlarının duyarlığı bitiyor); çok daha büyük değerleri kurucu zaten
  geçersiz sayıyor. Rastgelelik buradan: çöpün büyüklüğüne bağlı.

**Düzeltme** (`metal.m` `hd_rt_fix`, varsayılan açık, `HD_RT_FIX=0`
kapatır): yakalamadan hemen sonra, yapı kurulmadan ve sabit depoya
kopyalanmadan önce bir hesaplama geçişi her üçgene bakar; NaN/sonsuz ya da
herhangi bir bileşeni 100 km'yi aşan köşesi olan üçgeni dejenere eder
(üç köşe = 0). Sayaçlar raporda (`[HD] RT kose denetimi`) ve `HD_RT_LOG`
çıktısında gölgelendirici bazında.

**Doğrulama durumu (dürüstçe):** denetim açıkken iki koşu hızlıydı ve
sayaçlar çalışıyor; nedensellik testi mekanizmayı kanıtlıyor. Ama "artık
hiç yavaş koşu çıkmıyor" iddiası istatistik ister (düzeltmesiz oran ~%8;
%95 güven için ~40 koşu) ve bu YAPILMADI (bilgisayar kapatılacaktı).
Açık kalan: bozuk köşelerin asıl kaynağı (hangi çizimler, oyun mu çöp
veriyor yoksa yakalama çekirdeği mi yanlış okuyor) incelenmedi; denetim
belirtiyi kesiyor.

**Geçersiz sayılan eski ölçümler:** bu bölümdeki 3024×1964 "önce" değerleri
(ışın izleme açık 40 fps, kapalı 56 fps) zehirli/uzun koşulara denk
gelmişti; MSAA düzeltmesinin o çözünürlükteki gerçek kazancı yeniden
ölçülmeli. 1080p karşılaştırması (9,3–9,9 → 6,2 ms) geçerli. Önceki
oturumdaki "önbellekte 878 kopya → 22 ms" bulgusu da kısmen bu etkiyle
karışmış olabilir (kopyalar gerçekti, ama o koşularda da 18 gölgelendirici
vardı).

### Hava durumunu zorlama (iki build'de, `runtime/overrides.c`)

* `FORCERAIN=1` yağmur, `2` hep kuru: `Rain::Update` = `sub_0061f130`
  (`Rain+0x5094` hedef yağış, `+0x1f0` şiddet) ve hedefi hesaplayan
  `sub_0061e390` sarmalandı. (`3`/`4` denemeleri gökyüzünü değiştirmedi.)
* `FORCETOD=1` (+ `FORCETOD_STATE=1..4`): gökyüzü durum makinesi
  `sub_0060fab0`; durum `0x8a1c8c`, dünya sayacı `0x7fb718` (sn),
  `(int)sayaç % 300 == 60` olduğunda durum 2'ye geçer.
* Ayar değişkenleri: `g_RainEnable` = `0x870D10` (yalnızca seçenek),
  tablo `0x870ccc…0x870d24`.

## Kaynaklar

* AeroWidescreen/NFSHDReflections: https://github.com/AeroWidescreen/NFSHDReflections (`SOURCE/NFSU2/dllmain.cpp`, `assembly.h`)
* Zintexiss/NFSU2HDReflections: https://github.com/Zintexiss/NFSU2HDReflections
* ThirteenAG/WidescreenFixesPack (NFSU2): https://github.com/ThirteenAG/WidescreenFixesPack/releases/tag/nfsu2
* ThirteenAG/XboxRainDroplets (NFSU2): https://github.com/ThirteenAG/XboxRainDroplets/releases/tag/nfsu2
* ExOptsTeam/NFSU2ExOpts: https://github.com/ExOptsTeam/NFSU2ExOpts
* adamplayer/Underground2RTX: https://github.com/adamplayer/Underground2RTX
* Underground 2 Remix (ModDB): https://www.moddb.com/mods/need-for-speed-underground-2-remix
* DSOGaming, Remix v0.4: https://www.dsogaming.com/mods/need-for-speed-underground-2-rtx-remix-mod-v0-4-released/
* PCGamesN, Remix ilk sürüm: https://www.pcgamesn.com/nvidia/rtx-remix-need-for-speed-underground-2
* Redux 3.0: https://www.ktmx.pro/2024/06/need-for-speed-underground-2-redux-30.html
* ReShade ön ayarları: https://nfsmods.xyz/mod/5569
* Metal 4 / MetalFX kare ara değerlemesi: https://developer.apple.com/videos/play/wwdc2025/211/
