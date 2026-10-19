# Deploy the framework INI: the template's sections, order and comments, with the user's values.
#
# - A key the user has set keeps its value (the first occurrence, as GetPrivateProfileString reads it).
# - A key the user lacks appears with the template's value (the same default the DLL uses).
# - A key the template no longer has is dropped and reported.
# The file is written only when the result differs. No backup is left beside it (user decision
# 2026-09-25): every value survives the merge, and the plugin folder keeps only its own files.
# Output is UTF-8 without BOM, CRLF (GetPrivateProfileString reads it as ANSI; keys are ASCII).
param(
    [Parameter(Mandatory = $true)][string]$Template,
    [Parameter(Mandatory = $true)][string]$Target
)
$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Read-Lines([string]$path) {
    # ReadAllText detects and strips a BOM.
    $text = [System.IO.File]::ReadAllText($path, $utf8)
    $lines = @($text -split "`r?`n")
    if ($lines[-1] -eq '') { $lines = if ($lines.Count -gt 1) { $lines[0..($lines.Count - 2)] } else { @() } }
    return , @($lines)
}

# "section|key" (lower case) of a key line, or $null for sections, comments and blank lines.
function Get-Entry([string]$line, [string]$section) {
    $t = $line.Trim()
    if ($t -eq '' -or $t.StartsWith(';') -or $t.StartsWith('#') -or $t.StartsWith('[')) { return $null }
    $eq = $t.IndexOf('=')
    if ($eq -lt 1) { return $null }
    $key = $t.Substring(0, $eq).Trim()
    return [pscustomobject]@{
        Id    = ($section + '|' + $key).ToLowerInvariant()
        Key   = $key
        Value = $t.Substring($eq + 1).Trim()
        Name  = "[$section] $key"
    }
}

function Get-Section([string]$line, [string]$current) {
    $t = $line.Trim()
    if ($t -match '^\[(.+)\]$') { return $Matches[1].Trim() }
    return $current
}

$templateLines = Read-Lines $Template
$exists = Test-Path -LiteralPath $Target
$user = @{}
$userOrder = New-Object System.Collections.Generic.List[string]
if ($exists) {
    $section = ''
    foreach ($line in (Read-Lines $Target)) {
        $section = Get-Section $line $section
        $entry = Get-Entry $line $section
        if ($entry -and -not $user.ContainsKey($entry.Id)) {
            $user[$entry.Id] = $entry
            $userOrder.Add($entry.Id)
        }
    }
}

$kept = @{}
$added = New-Object System.Collections.Generic.List[string]
$section = ''
$out = New-Object System.Collections.Generic.List[string]
foreach ($line in $templateLines) {
    $section = Get-Section $line $section
    $entry = Get-Entry $line $section
    if (-not $entry) { $out.Add($line); continue }
    if ($user.ContainsKey($entry.Id)) {
        $kept[$entry.Id] = $true
        $out.Add($entry.Key + ' = ' + $user[$entry.Id].Value)
    } else {
        if ($exists) { $added.Add($entry.Name + ' = ' + $entry.Value) }
        $out.Add($line)
    }
}
$dropped = @($userOrder | Where-Object { -not $kept.ContainsKey($_) } | ForEach-Object { $user[$_].Name + ' = ' + $user[$_].Value })

$text = ($out -join "`r`n") + "`r`n"
$bytes = $utf8.GetBytes($text)
if ($exists) {
    $current = [System.IO.File]::ReadAllBytes($Target)
    if ([System.Convert]::ToBase64String($current) -eq [System.Convert]::ToBase64String($bytes)) {
        Write-Output "INI up to date: $Target"
        exit 0
    }
}
[System.IO.File]::WriteAllBytes($Target, $bytes)
if (-not $exists) {
    Write-Output "INI deployed (was absent): $Target"
} else {
    Write-Output ("INI merged: {0} (user values kept: {1}; added: {2}; dropped: {3})" -f $Target, $kept.Count, $added.Count, $dropped.Count)
    foreach ($a in $added) { Write-Output "  added   $a" }
    foreach ($d in $dropped) { Write-Output "  dropped $d" }
}
