param(
    [string]$OutputDirectory = 'artifacts/object-profiles-admin',
    [ValidateRange(3,30)][int]$CpuSeconds = 10,
    [ValidateRange(1,2)][int]$Repetitions = 2,
    [switch]$ValidateOnly,
    [ValidateSet(70000,280000)][int[]]$ActorCounts = @(70000,280000),
    [switch]$LightScheduling
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw '기존 결과를 보존하도록 새로운 출력 경로를 지정하세요.' }
if ($ValidateOnly -eq $false) {
    $principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) -eq $false) {
        throw '관리자 PowerShell이 필요합니다. 기록은 시작하지 않았습니다.'
    }
}

$wpr = (Get-Command wpr.exe -ErrorAction Stop).Source
$readobj = (Get-Command llvm-readobj.exe -ErrorAction Stop).Source
$pdbutil = (Get-Command llvm-pdbutil.exe -ErrorAction Stop).Source
$profileFile = Join-Path $repositoryRoot $(if ($LightScheduling) { 'tools/profiling/ObjectCpuLight.wprp' } else { 'tools/profiling/RadiusCpu.wprp' })
$profileName = if ($LightScheduling) { 'ObjectCpuLight' } else { 'RadiusCpu' }
$null = [xml](Get-Content $profileFile -Raw)
$reference = Import-Csv "$repositoryRoot/artifacts/object-next/direct-comparison/results.csv"
$manifest = @()
foreach ($version in @('baseline','direct')) {
    $directory = Join-Path $repositoryRoot "artifacts/object-next/$version"
    $exe = Join-Path $directory 'AreaUpdateExample.exe'
    $pdb = Join-Path $directory 'AreaUpdateExample.pdb'
    $hash = (Get-FileHash $exe).Hash
    $label = if ($version -eq 'direct') { 'candidate' } else { 'baseline' }
    $expected = @($reference | Where-Object { $_.version -eq $label } | Select-Object -ExpandProperty executableSha256 -Unique)
    if ($expected.Count -ne 1 -or $hash -ne $expected[0]) { throw "비교한 EXE와 다릅니다: $version" }
    $peText = (& $readobj --coff-debug-directory $exe) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "EXE 디버그 정보 검사 실패: $version" }
    $pdbText = (& $pdbutil dump -summary $pdb) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw "PDB 검사 실패: $version" }
    $peGuid = [regex]::Match($peText,'PDBGUID:\s*(\{[^}]+\})').Groups[1].Value
    $pdbGuid = [regex]::Match($pdbText,'GUID:\s*(\{[^}]+\})').Groups[1].Value
    $peAge = [regex]::Match($peText,'PDBAge:\s*(\d+)').Groups[1].Value
    $pdbAge = [regex]::Match($pdbText,'(?m)^\s*Age:\s*(\d+)').Groups[1].Value
    if ($peGuid -eq '' -or $peAge -eq '' -or $peGuid -ne $pdbGuid -or $peAge -ne $pdbAge) {
        throw "EXE/PDB GUID 또는 Age 불일치: $version"
    }
    $manifest += [pscustomobject]@{version=$version;exe=$exe;pdb=$pdb;sha256=$hash;pdbGuid=$pdbGuid;pdbAge=$pdbAge}
}

New-Item -ItemType Directory $root | Out-Null
$manifest | Export-Csv "$root/binaries.csv" -NoTypeInformation
Copy-Item $profileFile "$root/profile-definition.wprp"
Copy-Item "$repositoryRoot/artifacts/object-next/direct/TaskExecuter.h","$repositoryRoot/artifacts/object-next/direct/TaskExecuter.cpp" $root
$keys = @('AREAUPDATE_WORKER_DIAGNOSTICS','AREAUPDATE_OBJECT_TIMER_BATCH')
$saved = @{}
$cases = @()
$count = if ($ValidateOnly) { 1 } else { $Repetitions }
for ($round = 0; $round -lt $count; ++$round) {
    foreach ($actors in ($ActorCounts | Sort-Object -Unique)) {
        $order = if (($round + [int]($actors -eq 280000)) % 2 -eq 0) { @('baseline','direct') } else { @('direct','baseline') }
        foreach ($version in $order) {
            $cases += [pscustomobject]@{actors=$actors;version=$version;seed=42+$round;round=$round}
        }
    }
}
$cases | Export-Csv "$root/plan.csv" -NoTypeInformation

function Invoke-Recorder([string[]]$Options, [string]$LogPath) {
    $result = & $wpr @Options 2>&1
    $code = $LASTEXITCODE
    $result | Add-Content $LogPath
    if ($code -ne 0) { throw "WPR 실패: $Options`n$result" }
}

function Wait-Target([Diagnostics.Process]$Process, [int]$Seconds) {
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.Elapsed.TotalSeconds -lt $Seconds) {
        if ($Process.WaitForExit(250)) { throw '기록 구간 전에 대상 프로세스가 종료됐습니다.' }
        $Process.Refresh()
        if ($Process.PrivateMemorySize64 -gt 1500MB) { throw '대상 프로세스가 메모리 안전 상한을 초과했습니다.' }
    }
}

try {
    foreach ($key in $keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
    $env:AREAUPDATE_WORKER_DIAGNOSTICS = '1'
    $env:AREAUPDATE_OBJECT_TIMER_BATCH = '0'
    foreach ($case in $cases) {
        $name = "$($case.actors)-$($case.seed)-$($case.version)"
        $folder = Join-Path $root $name
        New-Item -ItemType Directory "$folder/bin","$folder/temp" -Force | Out-Null
        $binary = $manifest | Where-Object { $_.version -eq $case.version }
        Copy-Item $binary.exe "$folder/bin/AreaCpuProbe.exe"
        Copy-Item $binary.pdb "$folder/bin/AreaUpdateExample.pdb"
        $side = if ($case.actors -eq 70000) { 320 } else { 640 }
        $moves = if ($case.actors -eq 70000) { 2000 } else { 8000 }
        $targetSeconds = if ($ValidateOnly) { 10 } else { [math]::Max(30,$CpuSeconds+18) }
        $instance = 'ObjectProbe'+[guid]::NewGuid().ToString('N')
        $recording = $false
        $process = $null
        try {
            $process = Start-Process -FilePath "$folder/bin/AreaCpuProbe.exe" -WindowStyle Hidden -PassThru `
                -ArgumentList @($targetSeconds,$case.actors,$moves,$case.seed,1000,1,0,$side,'job',0,'signal',0,0,0,'unlimited',1,10,16,'uniform') `
                -RedirectStandardOutput "$folder/CPU-runtime.log" -RedirectStandardError "$folder/CPU-runtime.stderr"
            $null = $process.Handle
            if ($ValidateOnly -eq $false) {
                Wait-Target $process 6
                $startUtc = [DateTime]::UtcNow.ToString('o')
                Invoke-Recorder @('-start',"$profileFile!$profileName",'-filemode','-recordtempto',"$folder/temp",'-instancename',$instance) "$folder/wpr.log"
                $recording = $true
                Wait-Target $process $CpuSeconds
                Invoke-Recorder @('-stop',"$folder/CPU.etl",'-instancename',$instance) "$folder/wpr.log"
                $recording = $false
                $stopUtc = [DateTime]::UtcNow.ToString('o')
            }
            if ($process.WaitForExit(60000) -eq $false) { throw '대상 종료 대기 시간 초과' }
            $process.WaitForExit()
            if ($process.ExitCode -ne 0) { throw "대상 검증 실패: $name, exit=$($process.ExitCode)" }
            $log = Get-Content "$folder/CPU-runtime.log" -Raw
            foreach ($pattern in @('(?m)^SUMMARY .*\boverlaps=0(?: |$)','(?m)^SUMMARY .*\bnever_updated=0(?: |$)',
                '(?m)^RUNTIME .*\btasks_live=0(?: |$)','\btask_statistics=1(?: |\r?$)',
                '\bobject_schedule=fixed(?: |$)','\bwait_mode=signal(?: |$)')) {
                if ($log -notmatch $pattern) { throw "대상 설정/불변식 검사 실패: $name, $pattern" }
            }
            $metadata = [ordered]@{
                kind='CPU';pid=$process.Id;image='AreaCpuProbe.exe';mode='job';version=$case.version
                actors=$case.actors;mapSide=$side;movesPerSecond=$moves;seed=$case.seed;round=$case.round
                targetSeconds=$targetSeconds;durationSeconds=$CpuSeconds;validateOnly=[bool]$ValidateOnly
                executableSha256=$binary.sha256;pdbGuid=$binary.pdbGuid;pdbAge=$binary.pdbAge
                cpuProfile="$profileFile!$profileName";taskStatistics=$true;workerDiagnostics=$true
                schedulingStacks=($LightScheduling -eq $false)
                objectSampleStride=0;dispatcherProfileStride=0;timerTraceMs=0;waitMode='signal'
            }
            if ($ValidateOnly -eq $false) {
                $metadata.recordStartUtc=$startUtc
                $metadata.recordStopUtc=$stopUtc
                $metadata.etlBytes=(Get-Item "$folder/CPU.etl").Length
            }
            $metadata | ConvertTo-Json -Depth 5 | Set-Content "$folder/CPU.json" -Encoding utf8
            "$name 완료 (검사만=$ValidateOnly)"
        } finally {
            try {
                if ($recording) { Invoke-Recorder @('-cancel','-instancename',$instance) "$folder/wpr.log" }
            } finally {
                if ($null -ne $process) {
                    if ($process.HasExited -eq $false) { $process.Kill(); $process.WaitForExit() }
                    $process.Dispose()
                }
            }
        }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
