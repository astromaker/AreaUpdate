$repositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ErrorActionPreference = 'Stop'
$toolRoot = Join-Path $repositoryRoot 'tools'
$scripts = @(Get-ChildItem -LiteralPath $toolRoot -Recurse -File -Filter '*.ps1')
$references = 0

foreach ($script in $scripts) {
    $tokens = $null
    $errors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($script.FullName, [ref]$tokens, [ref]$errors)
    if ($errors.Count) { throw "PowerShell 구문 오류: $($script.FullName): $errors" }

    $nodes = $ast.FindAll({ param($node)
        $node -is [Management.Automation.Language.StringConstantExpressionAst] -or
        $node -is [Management.Automation.Language.ExpandableStringExpressionAst]
    }, $true)
    foreach ($node in $nodes) {
        $value = $node.Value
        if ($value.StartsWith('$repositoryRoot/')) {
            $value = $value.Substring('$repositoryRoot/'.Length)
        }
        if ($value -notmatch '^tools/.+\.(ps1|py|wprp)(!.*)?$') { continue }

        $relative = ($value -split '!', 2)[0]
        if ((Test-Path -LiteralPath (Join-Path $repositoryRoot $relative)) -eq $false) {
            throw "도구 참조 없음: $($script.Name): $relative"
        }
        ++$references
    }
}

foreach ($profile in (Get-ChildItem -LiteralPath $toolRoot -Recurse -File -Filter '*.wprp')) {
    [void][xml][IO.File]::ReadAllText($profile.FullName)
}

$documents = @(Get-ChildItem -LiteralPath (Join-Path $repositoryRoot 'docs') -Recurse -File -Filter '*.md') +
    @(Get-Item -LiteralPath (Join-Path $repositoryRoot 'README.md'),
        (Join-Path $repositoryRoot 'AGENTS.md'), (Join-Path $repositoryRoot 'CODING_STYLE.md'),
        (Join-Path $toolRoot 'README.md'))
foreach ($document in $documents) {
    $text = [IO.File]::ReadAllText($document.FullName).Replace('\', '/')
    foreach ($match in [regex]::Matches($text, 'tools/[A-Za-z0-9_./-]+\.(ps1|py|wprp)')) {
        if ((Test-Path -LiteralPath (Join-Path $repositoryRoot $match.Value)) -eq $false) {
            throw "문서 도구 참조 없음: $($document.Name): $($match.Value)"
        }
        ++$references
    }
}

Write-Output "도구 검사 통과: PowerShell $($scripts.Count)개, 정적 경로 참조 $($references)개, WPR XML"
