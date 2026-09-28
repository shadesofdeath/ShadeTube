# Builds a portable ShadeTube release: dist\ShadeTube-<version>-win64.zip containing the single self-contained
# exe (static CRT, static WebView2 loader, all assets embedded) plus a short Turkish BENIOKU.txt, and
# dist\ShadeTube-<version>-win64.zip.sha256. The printed "sha256: <hex>" line goes into the GitHub release notes:
# the in-app updater (app/Updater) refuses a downloaded package whose SHA-256 differs from it.
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1            # build Release + package
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 -SkipBuild # package the existing Release exe
#
# Keep this file UTF-8 *with BOM*: Windows PowerShell 5.1 reads a BOM-less script as ANSI and the Turkish
# BENIOKU.txt would come out garbled.
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$cm = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cm -notmatch 'project\(ShadeTube VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { throw 'version not found in CMakeLists.txt' }
$version = $Matches[1]

if (-not $SkipBuild) {
    & (Join-Path $root 'build.bat') Release ShadeTube build\Release
    if ($LASTEXITCODE -ne 0) { throw "Release build failed ($LASTEXITCODE)" }
}

$exe = Join-Path $root 'build\Release\bin\ShadeTube.exe'
if (-not (Test-Path $exe)) { throw "missing $exe (build first)" }

$name = "ShadeTube-$version-win64"
$dist = Join-Path $root 'dist'
$stage = Join-Path $dist $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Copy-Item $exe $stage

$readme = @"
ShadeTube $version (Windows x64, taşınabilir)
=============================================

Kurulum gerekmez: ShadeTube.exe'yi istediğin klasöre koy ve çalıştır.

Gereksinimler
- Windows 10 (1809+) veya Windows 11, 64 bit.
- Spotify girişi için Microsoft Edge WebView2 Runtime (Windows 11'de hazır gelir;
  Windows 10'da yoksa: https://developer.microsoft.com/microsoft-edge/webview2/).

İlk çalıştırma
- "Spotify ile bağlan" -> Spotify hesabınla giriş yap (Apple/Google/e-posta).
  İstersen "Spotify olmadan keşfet" ile açık katalogla (MusicBrainz) da kullanabilirsin.
- Sesler eşleşen YouTube kaydından reklamsız çalar.

Verilerin nerede?
- Ayarlar, kitaplık, oturum, önbellek: %LOCALAPPDATA%\ShadeTube
  (Spotify çerezi ve Last.fm/ListenBrainz bilgileri Windows DPAPI ile şifreli saklanır.)
- İndirilen MP3'ler: Müzik\ShadeTube\Sanatçı\Albüm\

Kurulum (isteğe bağlı)
- Ayarlar > Hakkında > "Bilgisayara kur": ShadeTube'u %LOCALAPPDATA%\Programs\ShadeTube
  klasörüne kopyalar, Başlat menüsüne ekler; Windows Ayarlar > Uygulamalar > Yüklü
  uygulamalar'dan kaldırılabilir. Yönetici izni gerekmez.

Güncellemeler
- ShadeTube günde en fazla bir kez GitHub'daki yeni sürümlere bakar (Ayarlar > Hakkında).
  "İndir ve kur" yeni sürümü indirir, doğrular ve kendini değiştirip yeniden açılır.

Kaldırma
- Kurduysan: Windows Ayarlar > Uygulamalar > Yüklü uygulamalar > ShadeTube > Kaldır.
- Taşınabilir kullanımda: ShadeTube.exe'yi ve %LOCALAPPDATA%\ShadeTube klasörünü sil.
"@
# UTF-8 with BOM so Notepad shows Turkish characters correctly.
[System.IO.File]::WriteAllText((Join-Path $stage 'BENIOKU.txt'), $readme, (New-Object System.Text.UTF8Encoding($true)))
# Licenses travel with the binary (BSD clause 2, OFL for the embedded fonts).
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $stage 'THIRD_PARTY_NOTICES.md')
Copy-Item (Join-Path $root 'assets\fonts\OFL.txt') (Join-Path $stage 'OFL.txt')

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
$hash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLowerInvariant()
# sha256sum format ("<hex>  <name>"), ASCII, next to the zip; upload both to the release.
[System.IO.File]::WriteAllText("$zip.sha256", "$hash  $name.zip`n", (New-Object System.Text.ASCIIEncoding))
Write-Output ("{0}  ({1:N0} bytes)" -f $zip, (Get-Item $zip).Length)
Write-Output "$zip.sha256"
Write-Output ''
Write-Output 'Release notes line (the in-app updater verifies the download against it):'
Write-Output "sha256: $hash"
