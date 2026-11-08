param(
    [Parameter(Mandatory=$true)][string]$InputDirectory,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [string]$SymbolPath = ''
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference='Stop'
$inputRoot=Join-Path $repositoryRoot $InputDirectory
$outputRoot=Join-Path $repositoryRoot $OutputDirectory
if(Test-Path $outputRoot){throw 'Choose a new export directory'}
$metadata=Get-Content "$inputRoot/CPU.json" -Raw|ConvertFrom-Json
if(-not(Test-Path "$inputRoot/CPU.etl")){throw 'CPU ETL is missing'}
$xperf='C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe'
if(-not(Test-Path $xperf)){throw 'Windows Performance Toolkit is required'}
New-Item -ItemType Directory "$outputRoot/symcache" -Force|Out-Null
$symbols=$env:_NT_SYMBOL_PATH
$cache=$env:_NT_SYMCACHE_PATH
try{
    $env:_NT_SYMBOL_PATH="$inputRoot/bin"
    if ($SymbolPath -ne '') { $env:_NT_SYMBOL_PATH += ";$SymbolPath" }
    $env:_NT_SYMCACHE_PATH="$outputRoot/symcache"
    & $xperf -i "$inputRoot/CPU.etl" -o "$outputRoot/trace-statistics.txt" -a tracestats
    if($LASTEXITCODE -ne 0){throw 'Trace statistics export failed'}
    & $xperf -i "$inputRoot/CPU.etl" -symbols -o "$outputRoot/cpu-functions.txt" -a profile -detail
    if($LASTEXITCODE -ne 0){throw 'Function export failed'}
    & $xperf -i "$inputRoot/CPU.etl" -symbols -o "$outputRoot/cpu-stacks.html" -a stack -pid $metadata.pid -event '.*Profile.*' -butterfly 10
    if($LASTEXITCODE -ne 0){throw 'CPU stack export failed'}
    & $xperf -i "$inputRoot/CPU.etl" -o "$outputRoot/thread-cpu.txt" -a cswitch -process -thread
    if($LASTEXITCODE -ne 0){throw 'Context switch export failed'}
    & $xperf -i "$inputRoot/CPU.etl" -o "$outputRoot/ready-thread.txt" -a readythread -list
    if($LASTEXITCODE -ne 0){throw 'ReadyThread export failed'}
    if ($null -eq $metadata.PSObject.Properties['schedulingStacks'] -or $metadata.schedulingStacks) {
        & $xperf -i "$inputRoot/CPU.etl" -symbols -o "$outputRoot/wait-stacks.html" -a stack -pid $metadata.pid -event '.*CSwitch.*' -butterfly 10
        if($LASTEXITCODE -ne 0){throw 'Context switch stack export failed'}
    }
    Copy-Item "$inputRoot/CPU.json" "$outputRoot/target.json"
}finally{
    $env:_NT_SYMBOL_PATH=$symbols
    $env:_NT_SYMCACHE_PATH=$cache
}
