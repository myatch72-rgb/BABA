@echo off
chcp 65001 >nul
title CS2 Karakter Modelleri Yukleyici
cls

echo ========================================================
echo        CS2 KARAKTER MODELLERI OTO-YUKLEYICI
echo ========================================================
echo.

:: 1. Kaynak Klasoru Bul (MODELLER\characters)
set "SRC="
if exist "%~dp0MODELLER\characters" set "SRC=%~dp0MODELLER\characters"
if not defined SRC if exist "%~dp0..\MODELLER\characters" set "SRC=%~dp0..\MODELLER\characters"
if not defined SRC if exist "C:\Users\HuzErhan\Desktop\BABA\MODELLER\characters" set "SRC=C:\Users\HuzErhan\Desktop\BABA\MODELLER\characters"
if not defined SRC if exist "C:\Users\HuzErhan\Desktop\Execution-main-main\MODELLER\characters" set "SRC=C:\Users\HuzErhan\Desktop\Execution-main-main\MODELLER\characters"

if not defined SRC (
    echo [HATA] MODELLER klasoru bulunamadi!
    echo Lutfen MODELLER klasorunun bu dosya yaninda oldugundan emin olun.
    echo.
    pause
    exit /b 1
)

:: 2. CS2 Hedef Klasorunu Bul
set "CSGO_DIR="
if exist "C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo" (
    set "CSGO_DIR=C:\Program Files (x86)\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo"
)
if not defined CSGO_DIR if exist "C:\Program Files\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo" (
    set "CSGO_DIR=C:\Program Files\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo"
)
if not defined CSGO_DIR if exist "D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo" (
    set "CSGO_DIR=D:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo"
)
if not defined CSGO_DIR if exist "D:\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo" (
    set "CSGO_DIR=D:\Steam\steamapps\common\Counter-Strike Global Offensive\game\csgo"
)
if not defined CSGO_DIR if exist "E:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo" (
    set "CSGO_DIR=E:\SteamLibrary\steamapps\common\Counter-Strike Global Offensive\game\csgo"
)

if not defined CSGO_DIR (
    echo [HATA] CS2 klasoru bulunamadi!
    echo Lutfen Counter-Strike 2'nin yuklu oldugu yolu kontrol edin.
    echo.
    pause
    exit /b 1
)

set "DST=%CSGO_DIR%\characters"

echo [+] Kaynak: %SRC%
echo [+] Hedef:  %DST%
echo.

:: 3. Eski kirik baglanti / Junction varsa temizle
if exist "%DST%" (
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$item = Get-Item '%DST%' -Force -ErrorAction SilentlyContinue; if ($item -and $item.LinkType -eq 'Junction') { [System.IO.Directory]::Delete('%DST%') }" >nul 2>&1
)

:: 4. Hedef klasoru olustur (yoksa)
if not exist "%DST%" mkdir "%DST%" >nul 2>&1

:: 5. Dosyalari Robocopy ile yuksek hizda ve guvenli kopyala
echo [*] Modeller kopyalaniyor, lutfen bekleyin...
robocopy "%SRC%" "%DST%" /E /R:1 /W:1 /NP /NFL /NDL /NJH /NJS >nul 2>&1
set RC_ERR=%ERRORLEVEL%

if %RC_ERR% LEQ 7 (
    echo.
    echo ========================================================
    echo   [BASARILI] Karakter Modelleri CS2'ye Eklendi!
    echo ========================================================
    echo.
    echo [+] Eklenen Modeller:
    echo     - 2B Nier Automata
    echo     - Adult Neptune
    echo     - Adult Jailer
    echo     - Lego Batman
    echo     - Muscular Gura
    echo     - Spider-Man ^(No Way Home^)
    echo     - T-Rex Noun
    echo     - Yoshino
    echo.
    echo [+] Modelleri kullanmak icin:
    echo     1. CS2'yi acin ve HELLFIRE_INJECTOR.bat ile raven.dll'i enjekte edin.
    echo     2. Menude: Misc -^> Custom Models kismindan istediginiz modeli secin!
    echo.
) else (
    echo.
    echo [HATA] Kopyalama basarisiz oldu. Hata Kodu: %RC_ERR%
    echo Eger erisim engellendi ise, bu dosyaya SAG TIKLAYIP
    echo 'Yonetici Olarak Calistir' demeyi deneyin.
    echo.
)

pause