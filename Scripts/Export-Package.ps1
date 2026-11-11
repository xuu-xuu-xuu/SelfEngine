# 旧入口保留，所有阶段与失败语义由同一 GameExporter 编排。
param([string]$Project='Resources/Templates/EmptyGame/project.json',[string]$Name='',[string]$ScriptProject='',[string]$ScriptType='',[string]$Bindings='',[string]$BuildSettings='',[string]$EntryUi='',[ValidateSet('zh-CN','en-US')][string]$Locale='zh-CN')
& (Join-Path $PSScriptRoot 'Export-Game.ps1') @PSBoundParameters
exit $LASTEXITCODE