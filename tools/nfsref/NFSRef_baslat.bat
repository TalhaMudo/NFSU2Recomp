@echo off
rem NFSRef: orijinal oyundan referans kareleri al.  Oyun klasorunde calistirin.
cd /d "%~dp0"
if not exist "SPEED2.EXE" (
  echo HATA: Bu dosya oyun klasorunde ^(SPEED2.EXE yaninda^) olmali.
  pause
  exit /b 1
)
if not exist "SCRIPTS\nfsref.asi" (
  echo HATA: SCRIPTS\nfsref.asi yok. Kitin tamamini oyun klasorune cikarin.
  pause
  exit /b 1
)
rem WidescreenFix oyunu degistiriyor (FOV, HUD, kare siniri); port'ta yok -> gecici kapat
if not exist "SCRIPTS_nfsref_kapali" mkdir "SCRIPTS_nfsref_kapali"
move /y "SCRIPTS\NFSUnderground2.WidescreenFix.*" "SCRIPTS_nfsref_kapali\" >nul 2>&1
rem onceki cikti silinmez, NFSRef_cikti_onceki olarak saklanir
if exist "NFSRef_cikti_onceki" rmdir /s /q "NFSRef_cikti_onceki"
if exist "NFSRef_cikti" ren "NFSRef_cikti" "NFSRef_cikti_onceki"
echo.
echo NFSRef calisiyor. Oyun kendi kendine menulerden yarisa girecek ve
echo yaklasik 2.5 dakika sonra (8950. karede) KENDINI KAPATACAK. Klavyeye/fareye dokunmayin,
echo baska pencereye gecmeyin.
echo.
start "" /wait "SPEED2.EXE"
rem WidescreenFix'i geri koy
move /y "SCRIPTS_nfsref_kapali\NFSUnderground2.WidescreenFix.*" "SCRIPTS\" >nul 2>&1
rmdir "SCRIPTS_nfsref_kapali" >nul 2>&1
echo.
echo Bitti. Lutfen "NFSRef_cikti" klasorunu zipleyip gonderin.
findstr /c:"BITTI" "NFSRef_cikti\nfsref_log.txt" >nul 2>&1 || echo UYARI: kosu tamamlanmamis gorunuyor; nfsref_log.txt yine de gonderin.
pause
