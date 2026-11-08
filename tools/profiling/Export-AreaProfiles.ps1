param([string]$InputDirectory='artifacts/motion-profiles-admin')
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$ErrorActionPreference='Stop'
$root=Join-Path $repositoryRoot $InputDirectory
$xperf='C:\Program Files (x86)\Windows Kits\10\Windows Performance Toolkit\xperf.exe'
if(-not(Test-Path "$root/CPU.etl") -or -not(Test-Path "$root/Heap.etl")){throw 'CPU/Heap ETL collection must succeed before analysis'}
$oldSymbols=$env:_NT_SYMBOL_PATH
$oldCache=$env:_NT_SYMCACHE_PATH
try{
 New-Item -ItemType Directory "$root/symcache" -Force|Out-Null
 $env:_NT_SYMBOL_PATH="$root/bin;$(Join-Path $repositoryRoot 'Source/x64/Release')"
 $env:_NT_SYMCACHE_PATH="$root/symcache"
 $cpu=Get-Content "$root/CPU.json" -Raw|ConvertFrom-Json
 $heap=Get-Content "$root/Heap.json" -Raw|ConvertFrom-Json
 & $xperf -i "$root/CPU.etl" -symbols -o "$root/cpu-functions.txt" -a profile -detail
 if($LASTEXITCODE -ne 0){throw 'CPU function export failed'}
 & $xperf -i "$root/CPU.etl" -symbols -o "$root/cpu-sampled-stacks.html" -a stack -pid $cpu.pid -event '.*Profile.*' -butterfly 10
 if($LASTEXITCODE -ne 0){throw 'CPU stack export failed'}
 & $xperf -i "$root/Heap.etl" -symbols -o "$root/heap-stacks.txt" -a heap -pid $heap.pid -stacks so -top 50
 if($LASTEXITCODE -ne 0){throw 'Heap stack export failed'}
 & $xperf -i "$root/CPU.etl" -o "$root/cpu-trace-statistics.txt" -a tracestats
 if($LASTEXITCODE -ne 0){throw 'CPU trace statistics export failed'}
 & $xperf -i "$root/Heap.etl" -o "$root/heap-trace-statistics.txt" -a tracestats
 if($LASTEXITCODE -ne 0){throw 'Heap trace statistics export failed'}
}finally{
 $env:_NT_SYMBOL_PATH=$oldSymbols
 $env:_NT_SYMCACHE_PATH=$oldCache
}
