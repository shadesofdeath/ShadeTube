# Builds a ShadeTube release, both editions:
#   dist\ShadeTube-<version>-win64.zip    portable: the single self-contained exe (static CRT, static WebView2 loader,
#                                         all assets embedded) plus a short Turkish BENIOKU.txt and the licenses
#   dist\ShadeTube-<version>-setup.exe    the setup (src/setup, ShadeTubeSetup.exe): its own window, per-user install,
#                                         the same exe inside as a sealed LZMS payload; /S for package managers
# each with a .sha256 next to it. The printed "sha256: <hex>" lines go into the GitHub release notes: the in-app
# updater (app/Updater) refuses a downloaded package whose SHA-256 is not among them. It updates installed and portable
# copies alike from the zip (the setup is never picked as the app).
#
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1                      # build Release + package
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 -SkipBuild           # package the existing Release build
#   powershell -ExecutionPolicy Bypass -File tools\package.ps1 -BuildDir build\X    # another (Release) build folder
#
# Keep this file UTF-8 *with BOM*: Windows PowerShell 5.1 reads a BOM-less script as ANSI and the Turkish
# BENIOKU.txt would come out garbled.
param([switch]$SkipBuild, [string]$BuildDir = 'build\Release')

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$cm = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cm -notmatch 'project\(ShadeTube VERSION ([0-9]+\.[0-9]+\.[0-9]+)') { throw 'version not found in CMakeLists.txt' }
$version = $Matches[1]

if (-not $SkipBuild) {
    # The setup target builds the app first and packs exactly that exe.
    & (Join-Path $root 'build.bat') Release ShadeTubeSetup $BuildDir
    if ($LASTEXITCODE -ne 0) { throw "Release build failed ($LASTEXITCODE)" }
}

$bin = Join-Path $root "$BuildDir\bin"
$exe = Join-Path $bin 'ShadeTube.exe'
$setupExe = Join-Path $bin 'ShadeTubeSetup.exe'
foreach ($f in @($exe, $setupExe)) { if (-not (Test-Path $f)) { throw "missing $f (build first)" } }
# The setup carries the app it was built with: both must be this version (a stale setup would ship an old app).
$setupInfo = (Get-Item $setupExe).VersionInfo
if ($setupInfo.ProductName -ne 'ShadeTube Setup' -or $setupInfo.ProductVersion -ne $version) {
    throw "ShadeTubeSetup.exe is '$($setupInfo.ProductName)' $($setupInfo.ProductVersion), expected 'ShadeTube Setup' $version"
}
if ((Get-Item $setupExe).LastWriteTime -lt (Get-Item $exe).LastWriteTime) { throw 'ShadeTubeSetup.exe is older than ShadeTube.exe (rebuild it)' }

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
- Kurulu sürüm isteyenler için ayrı bir kurulum dosyası var: ShadeTube-$version-setup.exe.
  %LOCALAPPDATA%\Programs\ShadeTube klasörüne kurar, Başlat menüsüne ekler; Windows
  Ayarlar > Uygulamalar > Yüklü uygulamalar'dan kaldırılabilir. Yönetici izni gerekmez;
  taşınabilir kopyayla aynı ayarları ve kitaplığı kullanır.

Güncellemeler
- ShadeTube günde en fazla bir kez GitHub'daki yeni sürümlere bakar (Ayarlar > Hakkında).
  "İndir ve kur" yeni sürümü indirir, doğrular ve kendini değiştirip yeniden açılır.

Kaldırma
- Kurduysan: Windows Ayarlar > Uygulamalar > Yüklü uygulamalar > ShadeTube > Kaldır.
- Taşınabilir kullanımda: ShadeTube.exe'yi ve %LOCALAPPDATA%\ShadeTube klasörünü sil.
"@
# UTF-8 with BOM so Notepad shows Turkish characters correctly.
[System.IO.File]::WriteAllText((Join-Path $stage 'BENIOKU.txt'), $readme, (New-Object System.Text.UTF8Encoding($true)))
# Licenses travel with the binary (BSD clause 2, OFL for the embedded fonts). The setup carries the same files.
Copy-Item (Join-Path $root 'LICENSE') (Join-Path $stage 'LICENSE.txt')
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') (Join-Path $stage 'THIRD_PARTY_NOTICES.md')
Copy-Item (Join-Path $root 'assets\fonts\OFL.txt') (Join-Path $stage 'OFL.txt')

$zip = Join-Path $dist "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip

$setupName = "ShadeTube-$version-setup.exe"
$setup = Join-Path $dist $setupName
Copy-Item $setupExe $setup -Force

# sha256sum format ("<hex>  <name>"), ASCII, next to each file; upload all four to the release.
$lines = @()
foreach ($f in @($zip, $setup)) {
    $hash = (Get-FileHash $f -Algorithm SHA256).Hash.ToLowerInvariant()
    $leaf = Split-Path $f -Leaf
    [System.IO.File]::WriteAllText("$f.sha256", "$hash  $leaf`n", (New-Object System.Text.ASCIIEncoding))
    Write-Output ("{0}  ({1:N0} bytes)" -f $f, (Get-Item $f).Length)
    Write-Output "$f.sha256"
    $lines += "sha256: $hash  $leaf"
}
Write-Output ''
Write-Output 'Release notes lines (the in-app updater verifies the zip against them):'
$lines | ForEach-Object { Write-Output $_ }
