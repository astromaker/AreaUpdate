param(
    [string]$InputDirectory = 'artifacts/object-profiles-admin',
    [string]$SymbolPath = ''
)
$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


$ErrorActionPreference = 'Stop'
$root = Join-Path $repositoryRoot $InputDirectory
$xperf = (Get-Command xperf.exe -ErrorAction Stop).Source
if ($SymbolPath -eq '') {
    $cache = Join-Path $repositoryRoot 'artifacts/radius-profiles-admin/symbols'
    if (Test-Path $cache) { $SymbolPath = "srv*$cache" }
}
$plan = @(Import-Csv "$root/plan.csv")
$exports = @()
foreach ($case in $plan) {
    $name = "$($case.actors)-$($case.seed)-$($case.version)"
    $relative = "$InputDirectory/$name"
    $metadata = Get-Content "$root/$name/CPU.json" -Raw | ConvertFrom-Json
    if ($metadata.validateOnly -or $metadata.mode -ne 'job' -or $metadata.actors -ne [int]$case.actors -or
        $metadata.version -ne $case.version -or $metadata.seed -ne [int]$case.seed) {
        throw "실제 Object 기록의 메타데이터가 아닙니다: $name"
    }
    if ((Get-FileHash "$root/$name/bin/AreaCpuProbe.exe").Hash -ne $metadata.executableSha256) {
        throw "기록한 실행 파일이 변경됐습니다: $name"
    }
    & "$repositoryRoot/tools/profiling/Export-RadiusProfile.ps1" -InputDirectory $relative -OutputDirectory "$relative/export" -SymbolPath $SymbolPath
    $partial = "$root/$name/export/scheduling.csv.partial"
    & $xperf -i "$root/$name/CPU.etl" -o $partial -a dumper -provider '{3d6fa8d1-fe05-11d0-9dda-00c04fd7ba7c}'
    if ($LASTEXITCODE -ne 0) { throw "스레드 이벤트 추출 실패: $name" }
    Move-Item -LiteralPath $partial -Destination "$root/$name/export/scheduling.csv"
    $exports += [pscustomobject]@{name=$name;actors=$case.actors;version=$case.version;seed=$case.seed;pid=$metadata.pid;sha256=$metadata.executableSha256}
    $exports | Export-Csv "$root/exports.csv" -NoTypeInformation
    "$name 추출 완료"
}
