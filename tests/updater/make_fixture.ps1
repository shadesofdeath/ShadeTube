# Regenerates tests/updater/fixtures/sample.zip with Windows PowerShell's Compress-Archive (the same tool
# tools/package.ps1 uses): deflate entries, a nested folder (PowerShell 5.1 writes '\' separators) and
# incompressible data (deflate stored blocks). updater_test rebuilds the same bytes to check the extraction.
#   powershell -ExecutionPolicy Bypass -File tests\updater\make_fixture.ps1
$ErrorActionPreference = 'Stop'
$fixtures = Join-Path $PSScriptRoot 'fixtures'
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("shadetube-zip-fixture-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force -Path (Join-Path $stage 'data') | Out-Null
try {
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $sb = New-Object System.Text.StringBuilder
    $u = [char]0x00FC   # "ü" (Windows PowerShell reads this BOM-less script as ANSI: keep it ASCII)
    for ($i = 0; $i -lt 500; $i++) { [void]$sb.Append("ShadeTube g${u}ncelleme testi $i`n") }
    [System.IO.File]::WriteAllText((Join-Path $stage 'hello.txt'), $sb.ToString(), $utf8)
    [System.IO.File]::WriteAllText((Join-Path $stage 'small.txt'), 'abc', $utf8)
    # 64 KB from the LCG x = x * 1103515245 + 12345 (mod 2^32), byte = (x >> 16) & 0xFF, x0 = 20260928.
    $bytes = New-Object byte[] 65536
    [uint64]$x = 20260928
    for ($i = 0; $i -lt $bytes.Length; $i++) {
        $x = ($x * 1103515245 + 12345) % 4294967296
        $bytes[$i] = [byte](($x -shr 16) -band 0xFF)
    }
    [System.IO.File]::WriteAllBytes((Join-Path $stage 'data\random.bin'), $bytes)
    [System.IO.File]::WriteAllText((Join-Path $stage 'data\ShadeTube.exe'), 'not really an exe', $utf8)
    New-Item -ItemType Directory -Force -Path $fixtures | Out-Null
    $zip = Join-Path $fixtures 'sample.zip'
    if (Test-Path $zip) { Remove-Item $zip -Force }
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip
    Write-Output ("{0} ({1:N0} bytes)" -f $zip, (Get-Item $zip).Length)
} finally {
    Remove-Item $stage -Recurse -Force
}
