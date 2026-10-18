$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot
Push-Location $projectRoot
try {
    New-Item -ItemType Directory -Force artifacts/GSN,artifacts/GSW | Out-Null
    & ./tools/go/bin/go.exe test ./contracts ./gsn
    if($LASTEXITCODE -ne 0){throw 'GSN tests failed'}
    & ./tools/go/bin/go.exe build -trimpath -ldflags '-s -w' -o artifacts/GSN/GSN.exe ./gsn
    if($LASTEXITCODE -ne 0){throw 'GSN build failed'}
    & ./tools/dotnet/dotnet.exe publish gsw/GSW.csproj -c Release -o artifacts/GSW -p:PublishSingleFile=true -p:SelfContained=true
    if($LASTEXITCODE -ne 0){throw 'GSW publish failed'}
} finally {Pop-Location}
