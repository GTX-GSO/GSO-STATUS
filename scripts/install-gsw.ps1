# Installs GSW for the current user under %LOCALAPPDATA%\Programs\GSO STATUS WIN, turns on start at sign-in and starts it.
# No administrator rights: GSW runs asInvoker. The CPU temperature/power worker is separate (install-cpu-sensor.ps1, elevated).
#   -Config <file>   configuration to install as gsw.json (replaces the installed one); without it an installed gsw.json is kept,
#                    and a first install takes config\gsw-nas.json from the project, if there is one.
#   -NoAutostart     install without start at sign-in (an existing entry is removed).
#   -NoStart         do not start GSW after installing.
param([string]$Config='',[switch]$NoAutostart,[switch]$NoStart)
$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot
$sourceDirectory=if(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'GSW.exe')){$PSScriptRoot}else{Join-Path $projectRoot 'artifacts/GSW'}
$sourceExecutable=Join-Path $sourceDirectory 'GSW.exe'
if(!(Test-Path -LiteralPath $sourceExecutable)){throw '没有找到 GSW.exe，请先构建（scripts/build-services.ps1）或在发布包目录里运行'}
$installDirectory=Join-Path $env:LOCALAPPDATA 'Programs\GSO STATUS WIN'
$executable=Join-Path $installDirectory 'GSW.exe'
$installedConfig=Join-Path $installDirectory 'gsw.json'

# Which configuration ends up installed.
$configSource=$null
if($Config){$configSource=(Resolve-Path -LiteralPath $Config).Path}
elseif(!(Test-Path -LiteralPath $installedConfig)){
    $projectConfig=Join-Path $projectRoot 'config/gsw-nas.json'
    if(Test-Path -LiteralPath $projectConfig){$configSource=$projectConfig}
}
$haveConfig=$configSource -or (Test-Path -LiteralPath $installedConfig)

# Stop every running GSW (not the elevated CPU sensor worker): its stop event is per AgentId, so ask through the config,
# then wait for the processes to leave.
function Get-Collectors{@(Get-Process -Name GSW -ErrorAction SilentlyContinue | Where-Object {$_.Path -and $_.Path -notlike (Join-Path $env:ProgramFiles 'GSO STATUS\CpuSensor\*')})}
if((Get-Collectors).Count -gt 0){
    foreach($candidate in @($configSource,$installedConfig,(Join-Path $projectRoot 'config/gsw-nas.json'),(Join-Path $projectRoot 'config/gsw.json'))){
        if($candidate -and (Test-Path -LiteralPath $candidate)){Start-Process -FilePath $sourceExecutable -ArgumentList @('--config',('"'+$candidate+'"'),'--stop') -WindowStyle Hidden -Wait}
    }
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    while((Get-Collectors).Count -gt 0 -and [DateTime]::UtcNow -lt $deadline){Start-Sleep -Milliseconds 200}
    if((Get-Collectors | Where-Object {$_.Path -eq $executable}).Count -gt 0){throw '已安装的 GSW 没有退出，未覆盖任何文件；请从托盘退出后重试'}
}

New-Item -ItemType Directory -Force -Path $installDirectory | Out-Null
Get-ChildItem -LiteralPath $sourceDirectory -File | Where-Object {$_.Name -ne 'gsw.json' -and $_.Name -notlike 'install-*.ps1' -and $_.Name -ne 'PawnIO_setup.exe'} | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $installDirectory $_.Name) -Force
}
if(Test-Path -LiteralPath (Join-Path $sourceDirectory 'licenses')){Copy-Item -LiteralPath (Join-Path $sourceDirectory 'licenses') -Destination $installDirectory -Recurse -Force}
if((Get-FileHash -LiteralPath $sourceExecutable).Hash -ne (Get-FileHash -LiteralPath $executable).Hash){throw '安装后的 GSW.exe 校验不一致'}
if($configSource){Copy-Item -LiteralPath $configSource -Destination $installedConfig -Force}
$version=((Get-Item -LiteralPath $executable).VersionInfo.ProductVersion -split '\+')[0]

if(!$haveConfig){
    $example=Join-Path $sourceDirectory 'gsw.example.json'
    if(Test-Path -LiteralPath $example){Copy-Item -LiteralPath $example -Destination $installedConfig}
    Write-Output "GSW $version 已安装到 $installDirectory，但还没有配置：请编辑 gsw.json（GSN 地址与写令牌）后重新运行本脚本。"
    exit 0
}

# --enable-autostart only succeeds for a configuration that loads, so a broken config is caught here.
$switch=if($NoAutostart){'--disable-autostart'}else{'--enable-autostart'}
$process=Start-Process -FilePath $executable -ArgumentList @('--config',('"'+$installedConfig+'"'),$switch) -WindowStyle Hidden -Wait -PassThru
if($process.ExitCode -ne 0){throw "配置无法加载（$installedConfig），未设置开机自启；日志在 %LOCALAPPDATA%\GSO STATUS WIN\logs"}
if(!$NoStart){Start-Process -FilePath $executable -ArgumentList @('--config',('"'+$installedConfig+'"')) -WorkingDirectory $installDirectory}
$autostart=if($NoAutostart){'未开启开机自启'}else{'已开启开机自启（托盘菜单或任务管理器“启动应用”可关闭）'}
Write-Output "GSW $version 已安装到 $installDirectory，$autostart。"
