param([string]$Port='COM4')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot
. (Join-Path $PSScriptRoot 'esp-idf-env.ps1')
$backup=Join-Path $projectRoot 'esp32p4_aibox_r3_backup.bin'
if((Get-Item -LiteralPath $backup).Length -ne 16777216 -or (Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ine '1203ee2cd0248549783611c0265a42774ba6555b7edcf2417fa9f9460b6a23a0'){throw 'Original image validation failed'}
# Never writes eFuses.
& $GsoIdfPython -m esptool --chip esp32p4 --port $Port write-flash 0 $backup
if($LASTEXITCODE -ne 0){throw 'Restore failed'}
& $GsoIdfPython -m esptool --chip esp32p4 --port $Port verify-flash 0 $backup
if($LASTEXITCODE -ne 0){throw 'Restore verification failed'}