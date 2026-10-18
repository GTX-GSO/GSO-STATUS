$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot
$configDirectory='\\192.168.124.14\hdd0\GSN\config'
$imageDirectory='\\192.168.124.14\hdd0\我的镜像'
if(!(Test-Path -LiteralPath $configDirectory) -or !(Test-Path -LiteralPath $imageDirectory)){throw 'NAS shared directories are unavailable'}
$manifest=Get-Content -LiteralPath (Join-Path $projectRoot 'artifacts/release-manifest.json') -Raw | ConvertFrom-Json
$entry=@($manifest | Where-Object {$_.file -match '^NAS/GSN-[0-9.]+-arm64\.docker\.tar$'})
if($entry.Count -ne 1){throw 'Expected exactly one release ARM64 image'}
$sourceImage=Join-Path $projectRoot ('artifacts/'+$entry[0].file)
$imageName=Split-Path $sourceImage -Leaf
$sourceConfig=Join-Path $projectRoot 'artifacts/NAS/config/gsn.json'
$config=Get-Content -LiteralPath $sourceConfig -Raw | ConvertFrom-Json
if($config.listen -ne '0.0.0.0:2780'){throw 'Host configuration must listen on 2780'}
$imageHash=(Get-FileHash -LiteralPath $sourceImage -Algorithm SHA256).Hash
if($imageHash -ine $entry[0].sha256){throw 'Release image digest mismatch'}
$targetImage=Join-Path $imageDirectory $imageName
if(!(Test-Path -LiteralPath $targetImage) -or (Get-FileHash -LiteralPath $targetImage -Algorithm SHA256).Hash -ine $imageHash){
    $stagedImage=Join-Path $imageDirectory ($imageName+'.'+[guid]::NewGuid().ToString('N')+'.uploading')
    Copy-Item -LiteralPath $sourceImage -Destination $stagedImage
    if((Get-FileHash -LiteralPath $stagedImage -Algorithm SHA256).Hash -ine $imageHash){throw 'Uploaded image digest mismatch'}
    Move-Item -LiteralPath $stagedImage -Destination $targetImage -Force
}
$targetConfig=Join-Path $configDirectory 'gsn.json'
$configHash=(Get-FileHash -LiteralPath $sourceConfig -Algorithm SHA256).Hash
if(!(Test-Path -LiteralPath $targetConfig) -or (Get-FileHash -LiteralPath $targetConfig -Algorithm SHA256).Hash -ine $configHash){
    if(Test-Path -LiteralPath $targetConfig){
        $backup=Join-Path $projectRoot ('work/nas-config-backups/'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
        New-Item -ItemType Directory -Path $backup -Force | Out-Null
        Copy-Item -LiteralPath $targetConfig -Destination (Join-Path $backup 'gsn.json')
    }
    $stagedConfig=Join-Path $configDirectory ('gsn.'+[guid]::NewGuid().ToString('N')+'.uploading')
    Copy-Item -LiteralPath $sourceConfig -Destination $stagedConfig
    if((Get-FileHash -LiteralPath $stagedConfig -Algorithm SHA256).Hash -ine $configHash){throw 'Uploaded configuration digest mismatch'}
    Move-Item -LiteralPath $stagedConfig -Destination $targetConfig -Force
}
if((Get-FileHash -LiteralPath $targetConfig -Algorithm SHA256).Hash -ine $configHash -or (Get-FileHash -LiteralPath $targetImage -Algorithm SHA256).Hash -ine $imageHash){throw 'NAS final verification failed'}
[pscustomobject]@{configuration=$targetConfig;listen=$config.listen;image=$targetImage;sha256=$imageHash;verified=$true;containers_changed=$false}|ConvertTo-Json
