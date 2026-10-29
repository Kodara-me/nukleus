<#
.SYNOPSIS
Removes the Nukleus Windows services and program files.

.DESCRIPTION
Run from an elevated PowerShell. Stops and deletes the nukleus and nukleus-web services, removes
their firewall rules, the Start menu shortcut and %ProgramFiles%\Nukleus. The data under
%ProgramData%\Nukleus and any directory given to the installer by -ModelsDir or -ConfigDir are
kept, and their paths printed, unless -Purge is confirmed.

.PARAMETER Purge
Also remove %ProgramData%\Nukleus (never a directory outside it).
.PARAMETER Yes
Confirm -Purge without prompting.
.PARAMETER DryRun
Print each action without changing anything.
.PARAMETER StateFile
For tests: read the installed state from this JSON file instead of the machine.
#>
[CmdletBinding()]
param(
    [switch]$Purge,
    [switch]$Yes,
    [switch]$DryRun,
    [string]$StateFile
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$ServiceNames = @('nukleus', 'nukleus-web')
$FirewallNames = @('Nukleus', 'Nukleus web interface')
$ServicesKey = 'HKLM:\SYSTEM\CurrentControlSet\Services'

function Stop-Uninstall([string]$Message) {
    [Console]::Error.WriteLine("Error: $Message")
    exit 1
}

function Invoke-Step([string]$Text, [scriptblock]$Action) {
    Write-Host "+ $Text"
    if (-not $DryRun) { & $Action }
}

$programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$installDir = Join-Path $programFiles 'Nukleus'
$dataDir = Join-Path $env:ProgramData 'Nukleus'
$shortcutPath = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Nukleus Manager.lnk'

if (-not $DryRun) {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Stop-Uninstall 'Run this script from an elevated PowerShell (Run as administrator).'
    }
}

# What is installed: the services with their command lines, and the firewall rules.
$state = $null
if ($StateFile) { $state = Get-Content -Raw -LiteralPath $StateFile | ConvertFrom-Json }
$imagePaths = @{}
foreach ($name in $ServiceNames) {
    if ($state) {
        $entry = $state.services.PSObject.Properties[$name]
        if ($entry) { $imagePaths[$name] = [string]$entry.Value.imagePath }
    } elseif (Test-Path "$ServicesKey\$name") {
        $imagePaths[$name] = [string](Get-ItemProperty "$ServicesKey\$name").ImagePath
    }
}
if ($state) {
    $rules = @($state.firewall)
} else {
    $rules = @()
    if (Get-Command Get-NetFirewallRule -ErrorAction SilentlyContinue) {
        $rules = @(Get-NetFirewallRule -ErrorAction SilentlyContinue |
            Where-Object { $_.DisplayName -in $FirewallNames } | ForEach-Object { $_.DisplayName })
    }
}

function Get-CommandLineOption([string]$ImagePath, [string]$Option) {
    if ($ImagePath -match ('--' + [regex]::Escape($Option) + '\s+(?:"([^"]+)"|(\S+))')) {
        if ($Matches[1]) { return $Matches[1] } else { return $Matches[2] }
    }
    return $null
}

# Directories given by -ModelsDir or -ConfigDir are kept in every case.
$keptDirectories = @()
if ($imagePaths.ContainsKey('nukleus')) {
    foreach ($option in @('models-dir', 'config-dir')) {
        $value = Get-CommandLineOption $imagePaths['nukleus'] $option
        if ($value -and -not $value.StartsWith($dataDir + '\', [System.StringComparison]::OrdinalIgnoreCase) -and $value -ne $dataDir) {
            $keptDirectories += $value
        }
    }
}

if ($DryRun) { Write-Host 'Dry run: nothing will be changed.' }

$confirmedPurge = $false
if ($Purge) {
    if ($Yes) {
        $confirmedPurge = $true
    } elseif ($DryRun) {
        Write-Host "Note: a prompt would ask to confirm the removal of $dataDir."
    } elseif ([Environment]::UserInteractive -and -not [Console]::IsInputRedirected) {
        $answer = Read-Host "Remove the configuration, tokens, logs and downloaded models under $dataDir? [y/N]"
        $confirmedPurge = $answer -match '^[Yy]'
    } else {
        Write-Host "-Purge needs confirmation: pass -Yes when the session is not interactive. $dataDir is kept."
    }
}

foreach ($name in $ServiceNames) {
    if (-not $imagePaths.ContainsKey($name)) { continue }
    Invoke-Step "Stop-Service $name" {
        $service = Get-Service -Name $name -ErrorAction SilentlyContinue
        if ($service -and $service.Status -ne 'Stopped') {
            Stop-Service -Name $name -Force
            $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
        }
    }
    Invoke-Step "sc.exe delete $name" {
        & (Join-Path $env:SystemRoot 'System32\sc.exe') delete $name | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "sc.exe delete $name failed with exit code $LASTEXITCODE." }
    }
}
foreach ($display in $FirewallNames) {
    if ($rules -contains $display) {
        Invoke-Step "Remove-NetFirewallRule -DisplayName `"$display`"" { Remove-NetFirewallRule -DisplayName $display }
    }
}
if ($DryRun -or (Test-Path -LiteralPath $shortcutPath)) {
    Invoke-Step "Remove-Item $shortcutPath" { Remove-Item -LiteralPath $shortcutPath -Force -ErrorAction SilentlyContinue }
}
if ($DryRun -or (Test-Path -LiteralPath $installDir)) {
    Invoke-Step "Remove-Item -Recurse $installDir" { Remove-Item -LiteralPath $installDir -Recurse -Force -ErrorAction SilentlyContinue }
}
if ($confirmedPurge) {
    # The only directory a purge removes; it never follows a path outside it.
    Invoke-Step "Remove-Item -Recurse $dataDir" { Remove-Item -LiteralPath $dataDir -Recurse -Force }
}

Write-Host 'Nukleus services and program files removed.'
if (-not $confirmedPurge) {
    Write-Host "Kept: $dataDir"
    foreach ($directory in $keptDirectories) { Write-Host "Kept: $directory" }
    Write-Host 'Run again with -Purge to remove the data under the first path; the others are never removed.'
} else {
    foreach ($directory in $keptDirectories) { Write-Host "Kept: $directory" }
}
