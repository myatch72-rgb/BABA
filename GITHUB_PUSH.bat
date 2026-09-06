@echo off
chcp 65001 >nul
title GitHub Push - myatch72-rgb

echo ========================================================
echo        GITHUB'A GONDURME SIHIRBAZI (myatch72-rgb)
echo ========================================================
echo.
echo [1] GitHub'da yeni (bos) bir repo olusturdugunuzdan emin olun:
echo     https://github.com/new
echo.
set REPO_NAME=BABA
set /p USER_REPO=Repository adi [Varsayilan: BABA]: 
if defined USER_REPO set REPO_NAME=%USER_REPO%

git remote set-url origin https://github.com/myatch72-rgb/%REPO_NAME%.git

echo.
echo [*] GitHub'a yukleniyor (git push -u origin main)...
echo.
git push -u origin main

if %ERRORLEVEL% equ 0 (
    echo.
    echo ========================================================
    echo  [BASARILI] Proje eksiksiz sekilde GitHub'a yuklendi!
    echo  Link: https://github.com/myatch72-rgb/%REPO_NAME%
    echo ========================================================
) else (
    echo.
    echo [!] Hata olustu. Eger repo henuz olusturulmadiysa,
    echo     https://github.com/new adresinden olusturup tekrar deneyin.
)

pause
