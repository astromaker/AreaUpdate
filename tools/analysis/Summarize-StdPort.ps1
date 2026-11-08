param(
    [switch]$Paired,
    [ValidateRange(1,1000000)][int]$RuntimeActors = 10000
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot 'artifacts/std-port'
$rows = @()
$actorPrefix = if ($RuntimeActors -eq 10000) { '' } else { "actors$RuntimeActors-" }
foreach ($stage in @('windows','queue','std','sleep')) {
    if ((Test-Path (Join-Path $root $stage)) -eq $false) { continue }
    $pattern = if ($Paired) { "paired*-${actorPrefix}runs.csv" } else { "${actorPrefix}runs.csv" }
    foreach ($file in Get-ChildItem (Join-Path $root $stage) -Filter $pattern) {
        $rows += Import-Csv -LiteralPath $file.FullName
    }
}

function Get-Median($values) {
    $sorted = @($values | Where-Object { $null -ne $_ -and [string]$_ -ne '' } | ForEach-Object { [double]$_ } | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    return ($sorted[[int][math]::Floor(($sorted.Count-1)/2)] + $sorted[[int][math]::Floor($sorted.Count/2)]) / 2
}

$result = @()
foreach ($group in $rows | Group-Object stage,scenario) {
    $first = $group.Group[0]
    $row = [ordered]@{ stage=$first.stage; scenario=$first.scenario; runs=$group.Count }
    foreach ($key in @('seconds','updates_per_sec','p99_tick_ms','cpu_seconds','process_cpu_seconds','actor_fps','max_gap_ms',
            'p99_gap_bucket_ms','max_queue_delay_ms','max_timer_delay_ms','busy_ms','idle_ms')) {
        $row[$key] = Get-Median @($group.Group | ForEach-Object { $_.$key })
    }
    $row['min_seconds'] = ($group.Group.seconds | ForEach-Object { [double]$_ } | Measure-Object -Minimum).Minimum
    $row['max_seconds'] = ($group.Group.seconds | ForEach-Object { [double]$_ } | Measure-Object -Maximum).Maximum
    $result += [pscustomobject]$row
}
$name = if ($Paired) { "paired-${actorPrefix}summary.csv" } else { "${actorPrefix}summary.csv" }
$result | Export-Csv -LiteralPath (Join-Path $root $name) -NoTypeInformation
$result | ConvertTo-Json -Depth 3
