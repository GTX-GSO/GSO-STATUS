# 从本机 Espressif Installation Manager 登记的安装中加载环境。
$ErrorActionPreference='Stop'
$registration='C:/Espressif/tools/eim_idf.json'
if(!(Test-Path -LiteralPath $registration)){throw "ESP-IDF 安装登记不存在：$registration"}
$registered=Get-Content -LiteralPath $registration -Raw | ConvertFrom-Json
$selected=@($registered.idfInstalled | Where-Object {$_.id -eq $registered.idfSelectedId})
if($selected.Count -ne 1){throw '无法确定本机选中的 ESP-IDF 安装'}
$installation=$selected[0]
foreach($file in @($installation.activationScript,$installation.python,(Join-Path $installation.path 'tools/idf.py'))){if(!(Test-Path -LiteralPath $file)){throw "ESP-IDF 安装文件不存在：$file"}}
# -e 仅输出该安装自己的环境变量，不修改用户的配置或全局命令别名。
$exports=& $installation.activationScript -e 6>&1
foreach($line in $exports){if($line -match '^([A-Za-z_][A-Za-z_0-9]*)=(.*)$'){[Environment]::SetEnvironmentVariable($matches[1],$matches[2],'Process')}}
$env:IDF_PATH=$installation.path
$env:IDF_TOOLS_PATH=$installation.idfToolsPath
$env:IDF_PYTHON_ENV_PATH=Split-Path (Split-Path $installation.python)
$env:PYTHONUTF8='1'
if(Test-Path -LiteralPath $registered.gitPath){$env:PATH=(Split-Path $registered.gitPath)+';'+$env:PATH}
Remove-Item Env:IDF_PYTHON_CHECK_CONSTRAINTS -ErrorAction SilentlyContinue
$GsoIdfPython=$installation.python
$GsoIdfScript=Join-Path $installation.path 'tools/idf.py'
