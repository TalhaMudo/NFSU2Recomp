#!/bin/bash
# out/NFSU2.app (ya da --hd ile out/NFSU2 HD.app) paketini kur.
#
#   ./pack.sh                 sadece port (~46 MB) -- oyun verisi kullanicinin
#   ./pack.sh --with-data     oyun verisini de yanina kopyala (~1.5 GB)
#   ./pack.sh --zip           dagitim icin NFSU2.zip da uret
#   ./pack.sh --embed         KISISEL tek parca uygulama: oyun verisi paketin
#                             ICINE gomulur (out/kisisel/, ~1.4 GB).  Kayitlar
#                             ~/Library/Application Support/NFSU2 Kayitlar'a
#                             yazilir.  Oyunun kendisini icerdigi icin
#                             DAGITILAMAZ; dagitim klasoru / zip uretilmez.
#   ./pack.sh --hd            HD build'den "NFSU2 HD.app" (grafik iyilestirmeleri;
#                             ayri paket kimligi, ayri log, orijinalle yan yana durur)
#
# Oyun verisi VARSAYILAN OLARAK pakete girmez: uygulama ilk acilista
# klasoru sorar ve secimi hatirlar.  Boylece paket kucuk kalir ve herkes
# kendi kurulumuyla calisir.
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"

WITH_DATA=0; MAKE_ZIP=0; HD=0; EMBED=0; SIGN_ID="${SIGN_ID:-}"
for a in "$@"; do
  case "$a" in
    --with-data) WITH_DATA=1 ;;
    --zip)       MAKE_ZIP=1 ;;
    --hd)        HD=1 ;;
    --embed)     EMBED=1 ;;
    *) echo "bilinmeyen secenek: $a"; exit 1 ;;
  esac
done

if [ "$HD" = 1 ]; then
  BIN="$ROOT/out/nfsu2_hd_boot"; NAME="NFSU2 HD"; DISP="Need for Speed Underground 2 HD"
  BID="org.nfsu2.nativeport.hd"; TAG="-hd"
else
  BIN="$ROOT/out/nfsu2_boot";    NAME="NFSU2";    DISP="Need for Speed Underground 2"
  BID="org.nfsu2.nativeport";    TAG=""
fi
[ -x "$BIN" ] || { echo "once derleyin: ./build.sh && ./link.sh  ($BIN yok)"; exit 1; }

if [ "$EMBED" = 1 ] && { [ "$WITH_DATA" = 1 ] || [ "$MAKE_ZIP" = 1 ]; }; then
  echo "--embed kisisel kullanim icindir; --with-data / --zip ile birlikte kullanilamaz"; exit 1
fi
OUTDIR="$ROOT/out"; [ "$EMBED" = 1 ] && OUTDIR="$ROOT/out/kisisel"
mkdir -p "$OUTDIR"
APP="$OUTDIR/$NAME.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"

cp "$BIN" "$APP/Contents/MacOS/NFSU2"
chmod +x "$APP/Contents/MacOS/NFSU2"
printf 'APPL????' > "$APP/Contents/PkgInfo"

# --- simge ---
if [ -x "$ROOT/.venv/bin/python" ]; then
  "$ROOT/.venv/bin/python" "$ROOT/tools/mkicon.py" "$APP/Contents/Resources/AppIcon.icns" >/dev/null
else
  echo "uyari: .venv yok, simge uretilmedi"
fi

# --- gomulu oyun verisi (yalnizca --embed) ---
# cp -c: APFS klonu.  Ayni diskte ek yer KAPLAMAZ ve aninda biter; klon
# desteklenmiyorsa cp kendiliginden normal kopyaya duser.
if [ "$EMBED" = 1 ]; then
  SRC="$ROOT/Need for Speed Underground 2 - Clean Edition/Need for Speed Underground 2"
  [ -f "$SRC/SPEED2.EXE" ] || { echo "oyun verisi bulunamadi: $SRC"; exit 1; }
  echo "oyun verisi pakete gomuluyor..."
  cp -Rc "$SRC" "$APP/Contents/Resources/Game"
  # MOVIES: VP6 kod cozucusu yok, oyun onlari zaten atliyor (~249 MB).
  # dinput8.dll / SCRIPTS / server.*: Windows eklentileri, yerel portta yuklenmez.
  rm -rf "$APP/Contents/Resources/Game/MOVIES" "$APP/Contents/Resources/Game/SCRIPTS" \
         "$APP/Contents/Resources/Game/dinput8.dll" "$APP/Contents/Resources/Game/server.dll" \
         "$APP/Contents/Resources/Game/server.cfg" "$APP/Contents/Resources/Game/SafeMode.bat"
  find "$APP/Contents/Resources/Game" -name .DS_Store -delete
fi

VER="$(date +%Y.%m.%d)"
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key>                <string>${NAME}</string>
  <key>CFBundleDisplayName</key>         <string>${DISP}</string>
  <key>CFBundleExecutable</key>          <string>NFSU2</string>
  <key>CFBundleIdentifier</key>          <string>${BID}</string>
  <key>CFBundleIconFile</key>            <string>AppIcon</string>
  <key>CFBundlePackageType</key>         <string>APPL</string>
  <key>CFBundleShortVersionString</key>  <string>${VER}</string>
  <key>CFBundleVersion</key>             <string>${VER}</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>LSMinimumSystemVersion</key>      <string>11.0</string>
  <key>LSApplicationCategoryType</key>   <string>public.app-category.racing-games</string>
  <key>LSArchitecturePriority</key>      <array><string>arm64</string></array>
  <!-- Retina: bu anahtar olmadan pencere 1x cizilip WindowServer
       tarafindan buyutulur (bulanik goruntu). -->
  <key>NSHighResolutionCapable</key>     <true/>
  <key>NSSupportsAutomaticGraphicsSwitching</key> <true/>
  <!-- Oyun klasoru bu dizinlerde olabilir; izin sorusunun metni. -->
  <key>NSDesktopFolderUsageDescription</key>
    <string>Oyun dosyalarini (SPEED2.EXE) okumak icin.</string>
  <key>NSDocumentsFolderUsageDescription</key>
    <string>Oyun dosyalarini (SPEED2.EXE) okumak icin.</string>
  <key>NSDownloadsFolderUsageDescription</key>
    <string>Oyun dosyalarini (SPEED2.EXE) okumak icin.</string>
  <key>NSRemovableVolumesUsageDescription</key>
    <string>Oyun dosyalarini (SPEED2.EXE) okumak icin.</string>
  <key>NSHumanReadableCopyright</key>
    <string>x86 -> ARM64 statik rekompilasyon portu. Oyunun kendisi bu pakete dahil degildir.</string>
</dict>
</plist>
PLIST

# --- imza ---
# SIGN_ID verilirse gercek sertifikayla imzala (Apple Developer hesabi
# olanlar icin), yoksa ad-hoc.  Ad-hoc imza Gatekeeper'i GECMEZ: indirilen
# paket "spctl: rejected" olur, kullanicinin karantinayi kaldirmasi gerekir.
if [ -n "$SIGN_ID" ]; then
  codesign --force --options runtime --sign "$SIGN_ID" --timestamp "$APP" \
    && echo "imza: $SIGN_ID" || { echo "codesign BASARISIZ"; exit 1; }
else
  codesign --force --sign - --timestamp=none "$APP" 2>/dev/null \
    && echo "imza: ad-hoc (notarize edilmedi)" || echo "uyari: codesign basarisiz"
fi

# genisletilmis oznitelikleri temizle (kaynak agactan gelen quarantine vb.)
xattr -cr "$APP" 2>/dev/null || true

SZ="$(du -sh "$APP" | cut -f1)"
echo "hazir: $APP  ($SZ)"

if [ "$EMBED" = 1 ]; then
  echo "kisisel tek parca uygulama hazir (DAGITMAYIN: oyunun kendisini icerir)."
  echo "kayitlar: ~/Library/Application Support/NFSU2 Kayitlar"
  exit 0
fi

# --- test edenler icin okuma notu ---
if [ "$WITH_DATA" = 1 ]; then
  VERI="Oyun dosyalari yanindaki klasorde geliyor (ayrica bir sey gerekmiyor)."
  ADIM3="3. Uygulamayi ac.  ONEMLI: $NAME.app ile yanindaki
   \"Need for Speed Underground 2\" klasorunu AYNI yerde tut; uygulama
   oyunu kendi yaninda arar.  Ikisini birlikte tasiyabilirsin."
else
  VERI="Kendi NFSU2 kurulumun.  Oyun dosyalari bu pakete DAHIL DEGIL."
  ADIM3="3. Uygulamayi ac.  Ilk acilista oyunun kurulu oldugu klasoru sorar
   (icinde SPEED2.EXE olan klasor).  Secim hatirlanir."
fi
cat > "$ROOT/out/OKUBENI.txt" <<TXT
$DISP -- macOS (Apple Silicon) yerel port
================================================================
x86 kodu ARM64'e statik olarak yeniden derlendi.  Wine yok, emulator yok.

GEREKENLER
  - Apple Silicon Mac (M1 ve uzeri).  Intel Mac calistiramaz.
  - macOS 11 Big Sur ve uzeri.
  - $VERI

KURULUM
1. $NAME.app'i Uygulamalar klasorune surukle.
2. Terminal'i acip su satiri calistir:

     xattr -dr com.apple.quarantine "/Applications/$NAME.app"

   Bu adim ZORUNLU.  Uygulama Apple tarafindan notarize edilmedigi icin
   indirildikten sonra macOS onu acmayi reddeder ("zarar gormus" ya da
   "gelistirici dogrulanamiyor" der).  Komut sadece indirme etiketini siler.
$ADIM3

KULLANIM
  Enter        baslik ekranini gec
  Cmd-F        tam ekran
  Cmd-Q        cikis
  Cozunurluk   oyunun kendi menusunden: Options > Display.
               Listedeki 1920x1080 satiri genis ekran icindir.

BIR SEY TERS GIDERSE
  Log:  ~/Library/Logs/NFSU2/nfsu2${TAG}.log        (bir onceki: nfsu2${TAG}-onceki.log)
  Bu dosyayi gonder, ne oldugu icinde yaziyor.

  Oyun klasorunu yeniden sectirmek icin:
     defaults delete $BID

  Kayitli oyunlar oyun klasorundeki memcard/ icinde,
  ayarlar ~/.nfsu2_registry dosyasinda tutulur.
TXT
echo "not: $ROOT/out/OKUBENI.txt"

# --- dagitim klasoru ---
# Uygulama, .app'in bulundugu dizini (ve bir ustunu) tarayarak oyunu
# kendiliginden buluyor.  Bu yuzden veri paketin ICINE degil YANINA
# konur: imza bozulmaz ve memcard/ yazilabilir kalir -- imzali bir
# bundle'in icine kayit yazmak calismaz.
DIST="$ROOT/out/NFSU2${TAG}-dagitim"
rm -rf "$DIST"; mkdir -p "$DIST"
ditto "$APP" "$DIST/$NAME.app"
cp "$ROOT/out/OKUBENI.txt" "$DIST/"

if [ "$WITH_DATA" = 1 ]; then
  SRC="$ROOT/Need for Speed Underground 2 - Clean Edition/Need for Speed Underground 2"
  [ -f "$SRC/SPEED2.EXE" ] || { echo "oyun verisi bulunamadi: $SRC"; exit 1; }
  echo "oyun verisi kopyalaniyor (buyuk, birkac dakika)..."
  # MOVIES: VP6 kod cozucusu yok, oyun onlari zaten atliyor (~249 MB)
  rsync -a --exclude='MOVIES/' "$SRC/" "$DIST/Need for Speed Underground 2/"
fi
echo "dagitim klasoru: $DIST  ($(du -sh "$DIST" | cut -f1))"

if [ "$MAKE_ZIP" = 1 ]; then
  ZIP="$ROOT/out/NFSU2${TAG}.zip"; rm -f "$ZIP"
  # ditto: imzayi ve paket yapisini bozmaz (zip -r bozar)
  ditto -c -k --sequesterRsrc --keepParent "$DIST" "$ZIP"
  echo "dagitim arsivi: $ZIP  ($(du -sh "$ZIP" | cut -f1))"
fi
