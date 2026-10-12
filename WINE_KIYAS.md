# Wine kıyas ölçümü (2026-10-03)

Projenin çıkış noktası olan "oyunu Wine ile çalıştırma" yoluna karşı native
portun ölçümü. Aynı makine (M1 Pro, macOS 27.0.1, 120 Hz ekran), aynı ayarlar
(`~/.nfsu2_registry`: yarış çözünürlüğü indeks 3 = 1280x960, tüm efektler açık),
aynı sahne (Quick Race → Circuit → Resort Loop, `TRACKS/STREAML4RA.BUN`), ses
iki tarafta da kapalı. Native ve Wine **dönüşümlü** iki tur koşuldu
(N → W → N → W) ki ısıl sürükleme iki tarafa eşit dağılsın.

## Sonuç

| | Native port | Wine 11.18 (wined3d, Rosetta) | oran |
|---|---|---|---|
| Yarış, +10..60 s | **119.3 / 119.0 fps** (8.34 ms medyan) | **11.3 / 10.0 fps** (~88–100 ms) | ~11x |
| Yarış, +60..140 s | 119.8 / 119.3 fps | 14.6 / 12.8 fps (~68–78 ms) | ~8.7x |
| Kare süresi p99 (yarış) | 9.4–10.6 ms | 10 sn'lik pencerelerde 4–5 fps'e düşüşler | |
| 3D menüler | 119.5 / 118.7 fps | 45.4 / 45.9 fps | ~2.6x |
| Oyun süreci CPU (yarış ortası) | %151 / %148 | %142 / %144 (tüm Wine süreçleri %147–151) | |
| **Kare başına CPU zamanı** | **~12.6 ms** | **~105 ms** | **~8x** |
| Enerji etkisi (`top` POWER) | 123 / 121 | 116 / 117 | kare başına ~8x |
| RSS (oyun süreci) | ~575 MB | ~345 MB (+ wineserver) | |
| Açılış güvenilirliği | 4/4 | 11 açılışın 7'si 15 sn içinde kare üretmedi (otomatik yeniden denendi) | |

**Native yarışta 120 Hz vsync tavanına dayalı** (`NOVSYNC=1` ile de 119–120);
gerçek tavan payı bu tablodan yüksek, yani oran **en az** 8–11x.

Wine tarafında hiçbir thread doymuş değil: oyun thread'i ~%70–77 meşgul,
`wined3d_cs` (komut thread'i) ~%44–48. Toplam CPU ~1.5 çekirdek iken kare
başına 8x daha fazla CPU harcanıyor; geri kalan zaman muhtemelen thread'ler
arası senkronizasyon ve GL→Metal sürücü beklemesi. Bu bir çıkarım: Rosetta
altındaki kod `sample` ile sembollenmedi.

## Wine tarafında karşılaşılanlar (Wine yolunun gerçek maliyeti)

1. Homebrew'ün `wine-stable`/`wine@devel` cask'ları 2026-09-01'de kapatıldı;
   macOS 27 güncellemesi Rosetta 2'yi kaldırmıştı (elle yeniden kuruldu).
2. Tam ekran 1280x960 macdrv'de reddediliyor (`ChangeDisplaySettings -2`) →
   `CreateDevice` başarısız → oyun `0x5D2790`'da null okumasıyla çöküyor.
   Yalnızca Wine sanal masaüstüyle açılıyor.
3. Pencere Mac'te öne geçemezse (ekran kilitliyken ya da kullanıcı başka
   uygulamadayken) tam ekran D3D cihazı "kayıp" durumunda kalıyor, oyun
   donuyor. macOS 14+ kooperatif aktivasyonu ve kilitli ekran yüzünden
   programla öne alınamıyor.
4. Açılışta aralıklı çökme (`7BF21239`, Wine DLL'i) ve sessiz kapanmalar.

## Yöntem ve sapmalar (dürüstlük notu)

- Kilitli ekran yüzünden Wine koşuları **pencereli modda** yapıldı. Bunun için
  oyunla gelen ThirteenAG widescreen fix ASI'si (`dinput8=n,b`) yüklendi ama
  `WindowedMode=2` dışındaki tüm müdahaleler kapatıldı (FixHUD/FOV, FPSLimit,
  SingleCoreAffinity, ConsoleGamma, SkipIntro vb. = 0). Native taraf ASI'siz.
- Wine'da menü girişi zamanlamalı `keybd_event` enjektörüyle yapıldı
  (`~/wine-olcum/tools/mkinject.py`, clang + elle PE32), native'de `AUTOSEQ`.
  Pist her iki tarafta da L4RA; Wine'da araba koşudan koşuya değişti
  (206 / 106), native'de hep Peugeot 206.
- Native fps: `FPSCSV`, yarış başı = çizimi 1000'i aşan ilk sürekli kare,
  `cizim > 400` kareleri. Wine fps: `WINEDEBUG=+timestamp,+fps` (~1.5 sn'lik
  ortalamalar, kare ağırlıklı birleştirildi), yarış başı = menülerden sonraki
  ilk <20 fps raporu.
- CPU/RSS `ps` ile 1 sn'de bir, enerji `top -stats power` ile 5 sn'de bir.
- Ham veriler ve betikler: `~/wine-olcum/` (`ab.sh`, `run_wine.sh`,
  `run_native.sh`, `tools/nstat.py`, `tools/wstat.py`, `tools/summary.py`,
  `n_ab*.csv`, `w_ab*.log`).
