param([ValidateRange(15,120)][int]$Seconds = 20)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot 'artifacts/capacity'
New-Item -ItemType Directory -Path $root -Force | Out-Null
$archive = Join-Path $root 'AreaUpdateExample-measured.exe'
if (Test-Path $archive) { throw 'Capacity campaign already exists; preserve its binary and results' }
Copy-Item (Join-Path $repositoryRoot 'Source/x64/Release/AreaUpdateExample.exe') $archive
$results = @()
foreach ($work in @(0,1000,5000,20000,100000,500000)) {
    $duration = if ($work -ge 100000) { [math]::Max($Seconds,30) } else { $Seconds }
    $relative = "artifacts/capacity/work-$work"
    & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $duration -Actors 70000 -ActorWork $work -AllowStarvation -ExecutablePath 'artifacts/capacity/AreaUpdateExample-measured.exe' -OutputDirectory $relative
    $result = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
    $results += $result
    $results | Export-Csv -LiteralPath (Join-Path $root 'results.csv') -NoTypeInformation
    if ($result.steadyCpuPercent -ge 90) {
        $relative = "artifacts/capacity/work-$work-confirm"
        & (Join-Path $repositoryRoot 'tools/benchmark/Measure-RuntimeCpu.ps1') -Seconds $duration -Actors 70000 -ActorWork $work -AllowStarvation -ExecutablePath 'artifacts/capacity/AreaUpdateExample-measured.exe' -OutputDirectory $relative
        $confirmation = Get-Content (Join-Path $repositoryRoot "$relative/summary.json") -Raw | ConvertFrom-Json
        $results += $confirmation
        $results | Export-Csv -LiteralPath (Join-Path $root 'results.csv') -NoTypeInformation
        if ($confirmation.steadyCpuPercent -ge 90) { break }
    }
}
$results | Select-Object actorWork,steadyCpuPercent,actorFps,sleepReadySamples,sleepBlockedSamples,sleepPastTimerCount | Format-Table -AutoSize
