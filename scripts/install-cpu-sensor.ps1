$ErrorActionPreference='Stop'
$projectRoot=Split-Path $PSScriptRoot
$sourceDirectory=if(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'GSW.exe')){$PSScriptRoot}else{Join-Path $projectRoot 'artifacts/GSW'}
$sourceExecutable=Join-Path $sourceDirectory 'GSW.exe'
$installationDirectory=Join-Path $env:ProgramFiles 'GSO STATUS/CpuSensor'
$executable=Join-Path $installationDirectory 'GSW.exe'
$taskName='GSO STATUS CPU Sensor'
if(!(Test-Path -LiteralPath $sourceExecutable)){throw 'Build GSW before installing the CPU sensor task'}
if(!(Get-CimInstance Win32_SystemDriver | Where-Object {$_.Name -eq 'PawnIO' -and $_.State -eq 'Running'})){
    $installer=if(Test-Path -LiteralPath (Join-Path $sourceDirectory 'PawnIO_setup.exe')){Join-Path $sourceDirectory 'PawnIO_setup.exe'}else{Join-Path $projectRoot 'tools/sensors/PawnIO_setup.exe'}
    $signature=Get-AuthenticodeSignature -LiteralPath $installer
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'namazso'){throw 'Official PawnIO installer signature verification failed'}
    $process=Start-Process -FilePath $installer -ArgumentList '-install','-silent' -WindowStyle Hidden -Wait -PassThru
    if($process.ExitCode -ne 0){throw 'PawnIO installation failed'}
}
$old=Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if($old -and ($old.Description -notlike 'CPU temperature and package power only*' -or @($old.Actions | Where-Object {$_.Arguments -ne '--cpu-sensor-worker'}).Count -gt 0)){throw 'An unrelated task already uses the sensor task name'}
if($old){
    Start-Process -FilePath $sourceExecutable -ArgumentList '--cpu-sensor-stop' -WindowStyle Hidden -Wait
    Stop-ScheduledTask -TaskName $taskName
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    do{
        $workers=@(Get-Process -Name GSW -ErrorAction SilentlyContinue | Where-Object {$_.Path -eq $executable})
        if($workers.Count -eq 0){break}
        Start-Sleep -Milliseconds 200
    }while([DateTime]::UtcNow -lt $deadline)
    if($workers.Count -gt 0){throw 'The installed sensor worker did not stop; no files were overwritten'}
}
New-Item -ItemType Directory -Path $installationDirectory -Force | Out-Null
$security=[Security.AccessControl.DirectorySecurity]::new()
$security.SetAccessRuleProtection($true,$false)
$administrators=[Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
$security.SetOwner($administrators)
foreach($permission in @(@('S-1-5-18','FullControl'),@('S-1-5-32-544','FullControl'),@('S-1-5-32-545','ReadAndExecute'))){
    $rule=[Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($permission[0]),[Security.AccessControl.FileSystemRights]$permission[1],([Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit'),[Security.AccessControl.PropagationFlags]::None,[Security.AccessControl.AccessControlType]::Allow)
    $security.AddAccessRule($rule)
}
Set-Acl -LiteralPath $installationDirectory -AclObject $security
Copy-Item -LiteralPath $sourceExecutable -Destination $executable -Force
Get-ChildItem -LiteralPath $sourceDirectory -Filter '*.dll' -File | ForEach-Object {Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $installationDirectory $_.Name) -Force}
if(Test-Path -LiteralPath (Join-Path $sourceDirectory 'licenses')){Copy-Item -LiteralPath (Join-Path $sourceDirectory 'licenses') -Destination $installationDirectory -Recurse -Force}
if((Get-FileHash -LiteralPath $sourceExecutable).Hash -ne (Get-FileHash -LiteralPath $executable).Hash){throw 'Installed sensor worker digest mismatch'}
$action=New-ScheduledTaskAction -Execute $executable -Argument '--cpu-sensor-worker' -WorkingDirectory $installationDirectory
$principal=New-ScheduledTaskPrincipal -UserId ([Security.Principal.WindowsIdentity]::GetCurrent().Name) -LogonType Interactive -RunLevel Highest
$settings=New-ScheduledTaskSettingsSet -Hidden -MultipleInstances IgnoreNew -ExecutionTimeLimit ([TimeSpan]::Zero) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $taskName -Action $action -Principal $principal -Settings $settings -Description 'CPU temperature and package power only; starts on demand, exits after 60 seconds without GSW.' -Force | Out-Null
$scheduler=New-Object -ComObject 'Schedule.Service';$scheduler.Connect()
$userSid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$scheduler.GetFolder('\').GetTask($taskName).SetSecurityDescriptor(('D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;'+$userSid+')'),0)
Start-ScheduledTask -TaskName $taskName
Write-Output 'CPU sensor task registered for on-demand elevated sampling; normal GSW stays asInvoker.'
