$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot
Push-Location $projectRoot
$savedGoOS=$env:GOOS;$savedGoArch=$env:GOARCH;$savedGoARM=$env:GOARM;$savedCGO=$env:CGO_ENABLED
try {
    $env:GOOS='linux';$env:CGO_ENABLED='0'
    $env:GOARCH='arm64'
    & ./tools/go/bin/go.exe build -trimpath -ldflags '-s -w' -o artifacts/GSN/GSN-linux-arm64 ./gsn
    if($LASTEXITCODE -ne 0){throw 'arm64 build failed'}
    $env:GOARCH='arm';$env:GOARM='7'
    & ./tools/go/bin/go.exe build -trimpath -ldflags '-s -w' -o artifacts/GSN/GSN-linux-armv7 ./gsn
    if($LASTEXITCODE -ne 0){throw 'armv7 build failed'}
    python scripts/package-gsn.py
    if($LASTEXITCODE -ne 0){throw 'Offline image packaging failed'}
} finally {$env:GOOS=$savedGoOS;$env:GOARCH=$savedGoArch;$env:GOARM=$savedGoARM;$env:CGO_ENABLED=$savedCGO;Pop-Location}
