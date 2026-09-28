# End-to-end update test: a local mock of GitHub's "releases/latest" API (tests\updater\mock_server.py) serves the
# real portable package from dist\ (tools\package.ps1 output) plus variants (raw exe asset, bad SHA-256, wrong
# version, missing asset, broken JSON, no Range support), then `updater_test --e2e` checks, downloads, verifies and
# swaps copies of an exe in %TEMP%. The running ShadeTube and its build output are never touched.
#   powershell -ExecutionPolicy Bypass -File tests\updater\run_e2e.ps1 [-BuildDir build\Debug] [-Port 8765]
# The mock folder stays in <BuildDir>\updater-mock (the app can be pointed at it with SHADETUBE_UPDATE_URL).
param([string]$BuildDir = 'build\Debug', [int]$Port = 8765, [switch]$ServeOnly)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$build = Join-Path $root $BuildDir
$test = Join-Path $build 'tests\updater\updater_test.exe'
if (-not $ServeOnly -and -not (Test-Path $test)) { throw "missing $test (build the updater_test target first)" }

$zip = Get-ChildItem (Join-Path $root 'dist') -Filter 'ShadeTube-*-win64.zip' |
    Sort-Object { [version]($_.Name -replace '^ShadeTube-(.+)-win64\.zip$', '$1') } | Select-Object -Last 1
if (-not $zip) { throw 'no dist\ShadeTube-<ver>-win64.zip (run tools\package.ps1 first)' }
$version = $zip.Name -replace '^ShadeTube-(.+)-win64\.zip$', '$1'

# ---- Mock folder -------------------------------------------------------------------------------------------
$mock = Join-Path $build 'updater-mock'
if (Test-Path $mock) { Remove-Item $mock -Recurse -Force }
$files = Join-Path $mock 'files'
New-Item -ItemType Directory -Force -Path $files | Out-Null
Copy-Item $zip.FullName $files
$unpacked = Join-Path $mock 'unpacked'
Expand-Archive -Path $zip.FullName -DestinationPath $unpacked
Copy-Item (Join-Path $unpacked 'ShadeTube.exe') (Join-Path $files 'ShadeTube.exe')
Remove-Item $unpacked -Recurse -Force

$base = "http://127.0.0.1:$Port"
$zipName = $zip.Name
$zipSha = (Get-FileHash $zip.FullName -Algorithm SHA256).Hash.ToLower()
$exeFile = Get-Item (Join-Path $files 'ShadeTube.exe')
$exeSha = (Get-FileHash $exeFile.FullName -Algorithm SHA256).Hash.ToLower()
$wrongSha = 'ab' * 32
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Asset($name, $url, $size, $sha) {
    [ordered]@{ name = $name; size = $size; digest = $(if ($sha) { "sha256:$sha" } else { $null }); browser_download_url = $url }
}
function Release($dir, $tag, $assets, $notes) {
    $release = [ordered]@{
        tag_name = $tag; name = "ShadeTube $tag"; draft = $false; prerelease = $false
        html_url = "https://github.com/shadesofdeath/ShadeTube/releases/tag/$tag"
        published_at = '2026-10-05T09:20:01Z'; assets = @($assets); body = $notes
    }
    $folder = Join-Path $mock "$dir\releases"
    New-Item -ItemType Directory -Force -Path $folder | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $folder 'latest'), ($release | ConvertTo-Json -Depth 6), $utf8)
}

$zipAsset = Asset $zipName "$base/files/$zipName" $zip.Length $zipSha
Release '.' "v$version" $zipAsset "## Yenilikler`r`n- Deneme`r`n`r`nsha256: $zipSha"
Release 'norange' "v$version" (Asset $zipName "$base/norange/files/$zipName" $zip.Length $zipSha) "sha256: $zipSha"
Release 'exe' "v$version" (Asset 'ShadeTube.exe' "$base/files/ShadeTube.exe" $exeFile.Length $null) "sha256: $exeSha"
Release 'badsha' "v$version" (Asset $zipName "$base/files/$zipName" $zip.Length $null) "sha256: $wrongSha"
Release 'baddigest' "v$version" (Asset $zipName "$base/files/$zipName" $zip.Length $wrongSha) 'Notlar'
Release 'wrongversion' 'v99.0.0' (Asset 'ShadeTube-99.0.0-win64.zip' "$base/files/$zipName" $zip.Length $zipSha) ''
Release 'missingasset' "v$version" (Asset $zipName "$base/files/nope.zip" 1 $null) ''
New-Item -ItemType Directory -Force -Path (Join-Path $mock 'broken\releases') | Out-Null
[System.IO.File]::WriteAllText((Join-Path $mock 'broken\releases\latest'), '{"tag_name": "v1.0', $utf8)

# ---- Server + test -----------------------------------------------------------------------------------------
$python = (Get-Command python -ErrorAction Stop).Source
$log = Join-Path $mock 'server.log'
$server = Start-Process -FilePath $python -ArgumentList @("`"$PSScriptRoot\mock_server.py`"", "`"$mock`"", $Port) `
    -PassThru -WindowStyle Hidden -RedirectStandardError $log
$code = 1
try {
    $up = $false
    for ($i = 0; $i -lt 50 -and -not $up; $i++) {
        try { Invoke-WebRequest -UseBasicParsing "$base/releases/latest" | Out-Null; $up = $true } catch { Start-Sleep -Milliseconds 200 }
    }
    if (-not $up) { throw "mock server did not start (see $log)" }
    Write-Output "mock server $base (pid $($server.Id)) serving ShadeTube $version"
    if ($ServeOnly) {
        Write-Output 'Serving until this script is stopped (Ctrl+C).'
        Wait-Process -Id $server.Id
        $code = 0
    } else {
        & $test --e2e $base
        $code = $LASTEXITCODE
    }
} finally {
    Stop-Process -Id $server.Id -Force -ErrorAction SilentlyContinue
}
exit $code
