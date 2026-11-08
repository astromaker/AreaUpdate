param(
    [string]$OutputDirectory = 'artifacts/active-wait/matrix',
    [ValidateSet(0,250,500,1000,2000)][int[]]$ThresholdsUs = @(0,250,500,1000,2000),
    [ValidateRange(1,1000000)][int[]]$ActorCounts = @(70000),
    [ValidateSet('baseline','direct')][string[]]$Versions = @('baseline','direct'),
    [ValidateSet('job','area','legacy')][string]$Mode = 'job'
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $OutputDirectory
if (Test-Path $root) { throw '이전 측정을 보존하도록 새 출력 경로를 지정하세요.' }
New-Item -ItemType Directory $root | Out-Null
$keys = @('AREAUPDATE_ACTIVE_WAIT_US','AREAUPDATE_WAIT_RETURN_DIAGNOSTICS','AREAUPDATE_WORKER_DIAGNOSTICS')
$saved = @{}
$rows = @()
try {
    foreach ($key in $keys) { $saved[$key] = [Environment]::GetEnvironmentVariable($key,'Process') }
    $env:AREAUPDATE_WAIT_RETURN_DIAGNOSTICS = '1'
    $env:AREAUPDATE_WORKER_DIAGNOSTICS = '1'
    $hashes = @()
    foreach ($version in $Versions) {
        $hashes += Get-FileHash "$repositoryRoot/artifacts/active-wait/$version/AreaUpdateExample.exe"
    }
    $hashes | Export-Csv "$root/binaries.csv" -NoTypeInformation
    foreach ($actors in ($ActorCounts | Sort-Object -Unique)) {
        $side = [int][math]::Ceiling(320 * [math]::Sqrt($actors/70000.0))
        $moves = [int][math]::Ceiling($actors * 2000.0/70000)
        foreach ($seed in @(42,43)) {
            $thresholds = @($ThresholdsUs | Sort-Object -Unique)
            $order = @($Versions)
            if ($seed -eq 43) { [array]::Reverse($thresholds); [array]::Reverse($order) }
            foreach ($threshold in $thresholds) {
                $env:AREAUPDATE_ACTIVE_WAIT_US = [string]$threshold
                foreach ($version in $order) {
                    $name = "$actors-$seed-$version-$threshold"
                    $stride = if ($Mode -eq 'job') { 256 } else { 0 }
                    & "$repositoryRoot/tools/benchmark/Measure-RuntimeCpu.ps1" -Seconds 20 -Actors $actors -MapSide $side `
                        -MovesPerSecond $moves -Seed $seed -JoinDurationMs 1000 -JoinIntervalMs 1 -Mode $Mode `
                        -WaitMode signal -ObjectSampleStride $stride -MaxPrivateMemoryMB 1500 -AllowStarvation `
                        -ExecutablePath "artifacts/active-wait/$version/AreaUpdateExample.exe" -OutputDirectory "$OutputDirectory/$name" | Out-Null
                    $r = Get-Content "$root/$name/summary.json" -Raw | ConvertFrom-Json
                    if ($r.processExitCode -ne 0 -or $r.neverUpdated -ne 0 -or $r.tasksLive -ne 0 -or
                        $r.activeWaitUs -ne $threshold -or $r.waitReturnDiagnosticsEnabled -ne $true) {
                        throw "실행 설정/불변식 실패: $name"
                    }
                    $b = @(Import-Csv "$root/$name/wait-returns.csv" | Where-Object scope -eq total)
                    $active = @($b | Where-Object kind -eq active)
                    if ($b.Count -ne 5 -or $active.Count -ne 1 -or [double]$active[0].returns -ne $r.activeWaits -or
                        ($b | Measure-Object returns -Sum).Sum -ne $r.sleepCount -or
                        $r.signalWaits + $r.activeWaits -ne $r.sleepCount -or
                        ($b | Measure-Object late_timer_returns -Sum).Sum -ne $r.sleepPastTimerCount) {
                        throw "능동 대기/복귀 합계 불일치: $name"
                    }
                    $r | Add-Member version $version
                    $rows += $r
                    $rows | Export-Csv "$root/results.csv" -NoTypeInformation
                    "$name FPS=$($r.steadyActorFps) CPU=$($r.steadyCpuPercent) timerP99=$($r.objectTimerP99Ms) activeMs=$($r.activeWaitMs)"
                }
            }
        }
    }
} finally {
    foreach ($key in $saved.Keys) { [Environment]::SetEnvironmentVariable($key,$saved[$key],'Process') }
}
