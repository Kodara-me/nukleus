<#
.SYNOPSIS
Installs or updates Nukleus as Windows services.

.DESCRIPTION
Run from an elevated PowerShell. Installs the backend (the nukleus service, nukleusd.exe, and the
model manager) and the web service (the nukleus-web service) from a build directory into
%ProgramFiles%\Nukleus, with the service data under %ProgramData%\Nukleus.

Every component already installed is updated. A component that is not installed is added only by
its flag, and a machine with nothing installed needs at least one of -InstallBackend and
-InstallWebService.

.PARAMETER InstallBackend
Install or update nukleusd and the model manager.
.PARAMETER InstallWebService
Install or update the web service (nukleus-web).
.PARAMETER BuildDir
Build directory holding the executables in <BuildDir>\src (default: build next to this script).
.PARAMETER QtDir
Qt installation directory holding bin\windeployqt.exe (default: read from <BuildDir>\CMakeCache.txt).
.PARAMETER Listen
LAN IPv4 address of the backend, added to the always-present 127.0.0.1 on port 9384. Needs a token.
.PARAMETER Token
Shared LAN token; avoid this option in a shared shell history. A masked prompt asks for it in an
interactive session.
.PARAMETER ModelsDir
Model weight directory; requires -ConfigDir.
.PARAMETER ConfigDir
Configuration directory; the catalogue lives in <ConfigDir>\catalog. The token stays in
%ProgramData%\Nukleus\config.
.PARAMETER SdCli
Path of sd-cli.exe (default: found on PATH).
.PARAMETER LlamaServer
Path of llama-server.exe (default: found on PATH).
.PARAMETER LlamaCompletion
Path of llama-completion.exe (default: found on PATH).
.PARAMETER Ffmpeg
Path of ffmpeg.exe (default: found on PATH).
.PARAMETER WebListen
LAN IPv4 address of the web interface, added to 127.0.0.1 on port 9385.
.PARAMETER WebBackendHost
Host of nukleusd, needed when the backend is neither on this machine nor installed by this run.
.PARAMETER WebBackendToken
LAN token of that nukleusd; avoid this option in a shared shell history.
.PARAMETER NoStart
Install without starting the services.
.PARAMETER DryRun
Print each action without changing anything.
.PARAMETER StateFile
For tests: read the installed state from this JSON file instead of the machine.
#>
[CmdletBinding()]
param(
    [switch]$InstallBackend,
    [switch]$InstallWebService,
    [string]$BuildDir,
    [string]$QtDir,
    [string]$Listen,
    [string]$Token,
    [string]$ModelsDir,
    [string]$ConfigDir,
    [string]$SdCli,
    [string]$LlamaServer,
    [string]$LlamaCompletion,
    [string]$Ffmpeg,
    [string]$WebListen,
    [string]$WebBackendHost,
    [string]$WebBackendToken,
    [switch]$NoStart,
    [switch]$DryRun,
    [string]$StateFile
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$ServiceName = 'nukleus'
$WebServiceName = 'nukleus-web'
$ServiceDisplayName = 'Nukleus'
$WebDisplayName = 'Nukleus web interface'
$ServiceAccount = "NT SERVICE\$ServiceName"
$WebAccount = "NT SERVICE\$WebServiceName"
$BackendPort = 9384
$WebPort = 9385
$AdministratorsSid = '*S-1-5-32-544'
$SystemSid = '*S-1-5-18'
$ServicesKey = 'HKLM:\SYSTEM\CurrentControlSet\Services'

function Stop-Install([string]$Message) {
    [Console]::Error.WriteLine("Error: $Message")
    exit 1
}

function Write-Action([string]$Text) { Write-Host "+ $Text" }

# Prints an action and, unless this is a dry run, runs it.
function Invoke-Step([string]$Text, [scriptblock]$Action) {
    Write-Action $Text
    if (-not $DryRun) { & $Action }
}

function Test-Ipv4([string]$Value) {
    $address = $null
    return ($Value -match '^\d{1,3}(\.\d{1,3}){3}$') -and [System.Net.IPAddress]::TryParse($Value, [ref]$address)
}

function Test-LoopbackHost([string]$Value) {
    return ($Value -eq 'localhost') -or ($Value -eq '::1') -or ($Value -match '^127\.')
}

function Test-AbsolutePath([string]$Value) {
    return $Value -match '^[A-Za-z]:[\\/]'
}

function Test-UnderUserProfile([string]$Path) {
    $users = (Join-Path $env:SystemDrive 'Users') + '\'
    return $Path.StartsWith($users, [System.StringComparison]::OrdinalIgnoreCase)
}

function Quote-ScArgument([string]$Value) {
    return '"' + ($Value -replace '"', '\"') + '"'
}

function Invoke-Sc([string[]]$ScArguments) {
    $process = Start-Process -FilePath (Join-Path $env:SystemRoot 'System32\sc.exe') `
        -ArgumentList ($ScArguments -join ' ') -Wait -PassThru -NoNewWindow
    if ($process.ExitCode -ne 0) {
        throw "sc.exe $($ScArguments[0]) $($ScArguments[1]) failed with exit code $($process.ExitCode)."
    }
}

function Invoke-Icacls([string[]]$IcaclsArguments) {
    & (Join-Path $env:SystemRoot 'System32\icacls.exe') @IcaclsArguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "icacls $($IcaclsArguments -join ' ') failed with exit code $LASTEXITCODE." }
}

function Read-MaskedValue([string]$Prompt) {
    $secure = Read-Host -Prompt $Prompt -AsSecureString
    $pointer = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { return [Runtime.InteropServices.Marshal]::PtrToStringBSTR($pointer) }
    finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($pointer) }
}

$interactive = [Environment]::UserInteractive -and -not [Console]::IsInputRedirected

# ---- Installed state -------------------------------------------------------------------------

function Read-EnvironmentList($Lines) {
    $map = [ordered]@{}
    foreach ($line in @($Lines)) {
        if ($line -match '^([^=]+)=(.*)$') { $map[$Matches[1]] = $Matches[2] }
    }
    return $map
}

function Get-InstalledService([string]$Name, $State) {
    if ($State) {
        $entry = $State.services.PSObject.Properties[$Name]
        if (-not $entry) { return $null }
        return @{ ImagePath = [string]$entry.Value.imagePath; Environment = (Read-EnvironmentList $entry.Value.environment) }
    }
    $key = "$ServicesKey\$Name"
    if (-not (Test-Path $key)) { return $null }
    $properties = Get-ItemProperty $key
    $imagePath = if ($properties.PSObject.Properties['ImagePath']) { [string]$properties.ImagePath } else { '' }
    $environment = if ($properties.PSObject.Properties['Environment']) { $properties.Environment } else { @() }
    return @{ ImagePath = $imagePath; Environment = (Read-EnvironmentList $environment) }
}

function Get-CommandLineOption([string]$ImagePath, [string]$Option) {
    if ($ImagePath -match ('--' + [regex]::Escape($Option) + '\s+(?:"([^"]+)"|(\S+))')) {
        if ($Matches[1]) { return $Matches[1] } else { return $Matches[2] }
    }
    return $null
}

function Test-FileHasContent([string]$Path) {
    return (Test-Path -LiteralPath $Path -PathType Leaf) -and ((Get-Item -LiteralPath $Path).Length -gt 0)
}

$programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$installDir = Join-Path $programFiles 'Nukleus'
$dataDir = Join-Path $env:ProgramData 'Nukleus'
$defaultConfigDir = Join-Path $dataDir 'config'
$defaultModelsDir = Join-Path $dataDir 'models'
$webDataDir = Join-Path $dataDir 'web'
$logsDir = Join-Path $dataDir 'logs'
$tmpDir = Join-Path $dataDir 'tmp'
$tokenPath = Join-Path $defaultConfigDir 'token'
$webTokenPath = Join-Path $webDataDir 'backend-token'
$shortcutPath = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Nukleus Manager.lnk'

$state = $null
if ($StateFile) { $state = Get-Content -Raw -LiteralPath $StateFile | ConvertFrom-Json }
$existingBackend = Get-InstalledService $ServiceName $state
$existingWeb = Get-InstalledService $WebServiceName $state
if ($state) {
    $tokenIsSet = [bool]$state.tokenSet
    $webTokenIsSet = [bool]$state.webBackendTokenSet
    $existingRules = @($state.firewall)
} else {
    $tokenIsSet = Test-FileHasContent $tokenPath
    $webTokenIsSet = Test-FileHasContent $webTokenPath
    $existingRules = @()
    if (Get-Command Get-NetFirewallRule -ErrorAction SilentlyContinue) {
        $existingRules = @(Get-NetFirewallRule -ErrorAction SilentlyContinue |
            Where-Object { $_.DisplayName -in @($ServiceDisplayName, $WebDisplayName) } | ForEach-Object { $_.DisplayName })
    }
}

# ---- Validation, before any change -------------------------------------------------------------

if ([bool]$ModelsDir -ne [bool]$ConfigDir) { Stop-Install '-ModelsDir and -ConfigDir must be given together.' }
foreach ($pair in @(@('-ModelsDir', $ModelsDir), @('-ConfigDir', $ConfigDir))) {
    if ($pair[1] -and -not (Test-AbsolutePath $pair[1])) { Stop-Install "$($pair[0]) must be an absolute path." }
}
foreach ($pair in @(@('-Listen', $Listen), @('-WebListen', $WebListen))) {
    if ($pair[1]) {
        if (-not (Test-Ipv4 $pair[1])) { Stop-Install "$($pair[0]) must be an IPv4 address." }
        if ($pair[1] -eq '0.0.0.0') { Stop-Install "$($pair[0]) must identify one interface address, not all interfaces." }
    }
}

$doBackend = $InstallBackend.IsPresent -or [bool]$existingBackend
$doWeb = $InstallWebService.IsPresent -or [bool]$existingWeb
if (-not ($doBackend -or $doWeb)) {
    Stop-Install 'Nothing is installed on this machine: pass -InstallBackend, -InstallWebService or both.'
}

if (-not $DryRun) {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Stop-Install 'Run this script from an elevated PowerShell (Run as administrator).'
    }
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $BuildDir) { $BuildDir = Join-Path (Split-Path -Parent $scriptDir) 'build' }
$BuildDir = [System.IO.Path]::GetFullPath($BuildDir)
$buildBinDir = Join-Path $BuildDir 'src'
$executables = @{}
if ($doBackend) {
    $executables['nukleusd.exe'] = $true
    $executables['nukleus-manager.exe'] = $true
}
if ($doWeb) { $executables['nukleus-web.exe'] = $true }
foreach ($name in $executables.Keys) {
    if (-not (Test-Path -LiteralPath (Join-Path $buildBinDir $name) -PathType Leaf)) {
        Stop-Install "$name was not found in $buildBinDir. Build the project first, or pass -BuildDir."
    }
}

$windeployqt = $null
if (-not $QtDir) {
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cache) {
        $line = Select-String -LiteralPath $cache -Pattern '^Qt6_DIR:[A-Z]+=(.+)$' | Select-Object -First 1
        if ($line) { $QtDir = (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $line.Matches[0].Groups[1].Value))) }
    }
}
if ($QtDir) { $windeployqt = Join-Path $QtDir 'bin\windeployqt.exe' }
if (-not $DryRun -and (-not $windeployqt -or -not (Test-Path -LiteralPath $windeployqt))) {
    Stop-Install 'windeployqt.exe was not found: pass -QtDir with the Qt installation directory (for example C:\Qt\6.8.3\msvc2022_64).'
}

# Backend settings: what an option gives wins, then what the installed service holds.
$backendListen = '127.0.0.1'
$backendModelsDir = $defaultModelsDir
$backendConfigDir = $defaultConfigDir
$backendEnvironment = [ordered]@{}
$toolPaths = [ordered]@{}
if ($doBackend) {
    if ($existingBackend) {
        $value = Get-CommandLineOption $existingBackend.ImagePath 'listen'
        if ($value) { $backendListen = $value }
        $value = Get-CommandLineOption $existingBackend.ImagePath 'models-dir'
        if ($value) { $backendModelsDir = $value }
        $value = Get-CommandLineOption $existingBackend.ImagePath 'config-dir'
        if ($value) { $backendConfigDir = $value }
        foreach ($key in $existingBackend.Environment.Keys) { $backendEnvironment[$key] = $existingBackend.Environment[$key] }
    }
    if ($Listen) { $backendListen = $Listen }
    if ($ModelsDir) { $backendModelsDir = $ModelsDir.TrimEnd('\', '/'); $backendConfigDir = $ConfigDir.TrimEnd('\', '/') }

    $tools = @(
        @{ Label = 'sd-cli'; Option = '-SdCli'; Given = $SdCli; Variable = 'NUKLEUS_SD_CLI'; Executable = 'sd-cli' },
        @{ Label = 'llama-server'; Option = '-LlamaServer'; Given = $LlamaServer; Variable = 'NUKLEUS_LLAMA_SERVER'; Executable = 'llama-server' },
        @{ Label = 'llama-completion'; Option = '-LlamaCompletion'; Given = $LlamaCompletion; Variable = 'NUKLEUS_LLAMA_COMPLETION'; Executable = 'llama-completion' },
        @{ Label = 'ffmpeg'; Option = '-Ffmpeg'; Given = $Ffmpeg; Variable = 'NUKLEUS_FFMPEG'; Executable = 'ffmpeg' }
    )
    $warnings = @()
    foreach ($tool in $tools) {
        $path = $null
        if ($tool.Given) {
            $path = [System.IO.Path]::GetFullPath($tool.Given)
        } elseif ($backendEnvironment.Contains($tool.Variable) -and $backendEnvironment[$tool.Variable]) {
            $path = $backendEnvironment[$tool.Variable]
        } else {
            $command = Get-Command $tool.Executable -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($command) { $path = $command.Source }
        }
        if (-not $path) {
            $warnings += "Warning: $($tool.Label) was not found on PATH; image or text jobs that need it will fail until $($tool.Variable) is set. Install it machine-wide or pass $($tool.Option)."
            continue
        }
        if (Test-UnderUserProfile $path) {
            Stop-Install ("$($tool.Label) is at $path, under a user profile that the service account cannot read. " +
                "Install it machine-wide (for llama.cpp: winget install ggml.llamacpp --scope machine) or pass another path with $($tool.Option).")
        }
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
            $warnings += "Warning: $($tool.Label) was not found at $path."
        }
        $toolPaths[$tool.Variable] = $path
    }
}

$backendIsLan = -not (Test-LoopbackHost $backendListen)
$newToken = $null
if ($Token) { $newToken = $Token.Trim() }
if ($backendIsLan -and -not $newToken -and -not $tokenIsSet) {
    if ($interactive -and -not $DryRun) {
        $newToken = (Read-MaskedValue 'LAN token for the backend').Trim()
    } elseif ($interactive) {
        Write-Host 'Note: a masked prompt would ask for the LAN token.'
    } else {
        Stop-Install 'A LAN address needs a token: pass -Token when the session is not interactive.'
    }
    if (-not $newToken -and -not $DryRun -and $interactive) { Stop-Install 'A LAN address needs a token.' }
}

# Web service settings.
$webListenAddress = '127.0.0.1'
$webEnvironment = [ordered]@{}
$webBackendHostValue = $null
$newWebToken = $null
if ($doWeb) {
    if ($existingWeb) {
        foreach ($key in $existingWeb.Environment.Keys) { $webEnvironment[$key] = $existingWeb.Environment[$key] }
        if ($webEnvironment.Contains('NUKLEUS_WEB_LISTEN')) { $webListenAddress = $webEnvironment['NUKLEUS_WEB_LISTEN'] }
        if ($webEnvironment.Contains('NUKLEUS_WEB_BACKEND_HOST')) { $webBackendHostValue = $webEnvironment['NUKLEUS_WEB_BACKEND_HOST'] }
    }
    if ($WebListen) { $webListenAddress = $WebListen }
    if ($WebBackendHost) { $webBackendHostValue = $WebBackendHost.Trim() }
    if ($webBackendHostValue -and (Test-LoopbackHost $webBackendHostValue)) { $webBackendHostValue = $null }
    if (-not $webBackendHostValue -and -not $doBackend) {
        Stop-Install 'The web service needs the host of nukleusd on this machine or elsewhere: install the backend too, or pass -WebBackendHost.'
    }
    if ($webBackendHostValue) {
        if ($WebBackendToken) { $newWebToken = $WebBackendToken.Trim() }
        if (-not $newWebToken -and -not $webTokenIsSet) {
            if ($interactive -and -not $DryRun) {
                $newWebToken = (Read-MaskedValue "LAN token of nukleusd on $webBackendHostValue").Trim()
                if (-not $newWebToken) { Stop-Install 'The web service needs the LAN token of its backend.' }
            } elseif ($interactive) {
                Write-Host 'Note: a masked prompt would ask for the backend token of the web service.'
            } else {
                Stop-Install 'A remote backend needs its token: pass -WebBackendToken when the session is not interactive.'
            }
        }
    }
}
$webIsLan = -not (Test-LoopbackHost $webListenAddress)

# ---- Plan and execution ----------------------------------------------------------------------

$installing = -not ($existingBackend -or $existingWeb)
if ($DryRun) { Write-Host 'Dry run: nothing will be changed.' }
Write-Host ($(if ($installing) { 'Installing Nukleus.' } else { 'Updating Nukleus.' }))
if ($doBackend) { foreach ($warning in $warnings) { Write-Host $warning } }

$serviceNames = @()
if ($doBackend) { $serviceNames += $ServiceName }
if ($doWeb) { $serviceNames += $WebServiceName }

# 1. Stop the installed services so that their files can be replaced.
foreach ($pair in @(@($WebServiceName, $existingWeb), @($ServiceName, $existingBackend))) {
    if ($pair[1]) {
        $name = $pair[0]
        Invoke-Step "Stop-Service $name" {
            $service = Get-Service -Name $name -ErrorAction SilentlyContinue
            if ($service -and $service.Status -ne 'Stopped') {
                Stop-Service -Name $name -Force
                $service.WaitForStatus('Stopped', [TimeSpan]::FromSeconds(30))
            }
        }
    }
}

# 2. Program files.
Invoke-Step "New-Item -ItemType Directory $installDir" {
    New-Item -ItemType Directory -Force -Path $installDir | Out-Null
}
foreach ($name in ($executables.Keys | Sort-Object)) {
    $source = Join-Path $buildBinDir $name
    Invoke-Step "Copy-Item $source -> $installDir" { Copy-Item -LiteralPath $source -Destination $installDir -Force }
}
Invoke-Step "Copy-Item $(Join-Path $buildBinDir '*.dll') -> $installDir" {
    $libraries = @(Get-ChildItem -Path (Join-Path $buildBinDir '*.dll') -ErrorAction SilentlyContinue)
    if ($libraries.Count -gt 0) { Copy-Item -LiteralPath $libraries.FullName -Destination $installDir -Force }
}
foreach ($name in ($executables.Keys | Sort-Object)) {
    $target = Join-Path $installDir $name
    $deployTool = if ($windeployqt) { $windeployqt } else { 'windeployqt.exe' }
    Invoke-Step "$deployTool $target" {
        & $windeployqt --no-translations $target | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "windeployqt failed for $target with exit code $LASTEXITCODE." }
    }
}

# 3. Data directories.
$directories = @($dataDir, $logsDir)
if ($doBackend) {
    $directories += @($defaultConfigDir, $tmpDir, $backendModelsDir, (Join-Path $backendConfigDir 'catalog'))
    foreach ($category in @('generation', 'upscale', 'llm', 'companion', 'animation')) {
        $directories += (Join-Path $backendModelsDir $category)
    }
}
if ($doWeb) { $directories += $webDataDir }
foreach ($directory in ($directories | Select-Object -Unique)) {
    Invoke-Step "New-Item -ItemType Directory $directory" { New-Item -ItemType Directory -Force -Path $directory | Out-Null }
}

# 4. Services: registration, account, automatic start, recovery.
function Get-ImagePath([string]$Exe, [string[]]$ArgumentList) {
    return (('"' + (Join-Path $installDir $Exe) + '"') + $(if ($ArgumentList.Count) { ' ' + ($ArgumentList -join ' ') } else { '' }))
}

function Set-NukleusService([string]$Name, [string]$Display, [string]$Account, [string]$ImagePath, [string]$Description, $Existing) {
    $verb = if ($Existing) { 'config' } else { 'create' }
    Invoke-Step "sc.exe $verb $Name binPath= $(Quote-ScArgument $ImagePath) start= auto obj= `"$Account`" DisplayName= `"$Display`"" {
        Invoke-Sc @($verb, $Name, 'binPath=', (Quote-ScArgument $ImagePath), 'start=', 'auto', 'obj=', (Quote-ScArgument $Account),
            'DisplayName=', (Quote-ScArgument $Display))
    }
    Invoke-Step "sc.exe description $Name `"$Description`"" {
        Invoke-Sc @('description', $Name, (Quote-ScArgument $Description))
    }
    Invoke-Step "sc.exe failure $Name reset= 86400 actions= restart/5000/restart/5000/restart/5000" {
        Invoke-Sc @('failure', $Name, 'reset=', '86400', 'actions=', 'restart/5000/restart/5000/restart/5000')
    }
}

function Set-ServiceEnvironment([string]$Name, $Environment) {
    Write-Action "Set the Environment value of service $Name"
    foreach ($key in $Environment.Keys) { Write-Host "    $key=$($Environment[$key])" }
    if (-not $DryRun) {
        $lines = @($Environment.Keys | ForEach-Object { "$_=$($Environment[$_])" })
        New-ItemProperty -Path "$ServicesKey\$Name" -Name 'Environment' -PropertyType MultiString -Value $lines -Force | Out-Null
    }
}

if ($doBackend) {
    $arguments = @('--listen', $backendListen, '--port', "$BackendPort", '--token-file', ('"' + $tokenPath + '"'),
        '--models-dir', ('"' + $backendModelsDir + '"'), '--config-dir', ('"' + $backendConfigDir + '"'))
    Set-NukleusService $ServiceName $ServiceDisplayName $ServiceAccount (Get-ImagePath 'nukleusd.exe' $arguments) `
        'Nukleus backend for image generation and language models.' $existingBackend
    $backendEnvironment['NUKLEUS_LOG_FILE'] = Join-Path $logsDir 'nukleusd.log'
    $backendEnvironment['TEMP'] = $tmpDir
    $backendEnvironment['TMP'] = $tmpDir
    foreach ($key in $toolPaths.Keys) { $backendEnvironment[$key] = $toolPaths[$key] }
    Set-ServiceEnvironment $ServiceName $backendEnvironment
}
if ($doWeb) {
    Set-NukleusService $WebServiceName $WebDisplayName $WebAccount (Get-ImagePath 'nukleus-web.exe' @()) `
        'Nukleus web interface.' $existingWeb
    $webEnvironment['NUKLEUS_LOG_FILE'] = Join-Path $logsDir 'nukleus-web.log'
    if ($webIsLan) { $webEnvironment['NUKLEUS_WEB_LISTEN'] = $webListenAddress } else { $webEnvironment.Remove('NUKLEUS_WEB_LISTEN') }
    if ($webBackendHostValue) {
        $webEnvironment['NUKLEUS_WEB_BACKEND_HOST'] = $webBackendHostValue
        $webEnvironment['NUKLEUS_WEB_BACKEND_TOKEN_FILE'] = $webTokenPath
    } else {
        $webEnvironment.Remove('NUKLEUS_WEB_BACKEND_HOST')
        $webEnvironment.Remove('NUKLEUS_WEB_BACKEND_TOKEN_FILE')
    }
    Set-ServiceEnvironment $WebServiceName $webEnvironment
}

# 5. Permissions, once the virtual accounts exist, then the secrets.
function Grant-Access([string]$Path, [string[]]$Entries, [bool]$Replace) {
    $mode = if ($Replace) { @('/inheritance:r', '/grant:r') } else { @('/grant') }
    $text = "icacls $Path $($mode -join ' ') $($Entries -join ' ')"
    Invoke-Step $text { Invoke-Icacls (@($Path) + $mode + $Entries) }
}

if ($doBackend) {
    $backendOnly = @("${AdministratorsSid}:(OI)(CI)F", "${SystemSid}:(OI)(CI)F", "${ServiceAccount}:(OI)(CI)M")
    Grant-Access $defaultConfigDir $backendOnly $true
    Grant-Access $tmpDir $backendOnly $true
    Grant-Access $backendModelsDir @("${ServiceAccount}:(OI)(CI)M") $false
    if ($ConfigDir) { Grant-Access $backendConfigDir @("${ServiceAccount}:(OI)(CI)M") $false }
    if ($newToken -or $installing -or -not $tokenIsSet) {
        Invoke-Step "New-Item -ItemType File $tokenPath" {
            if (-not (Test-Path -LiteralPath $tokenPath)) { New-Item -ItemType File -Path $tokenPath | Out-Null }
        }
        Grant-Access $tokenPath @("${AdministratorsSid}:F", "${SystemSid}:F", "${ServiceAccount}:R") $true
        if ($newToken) {
            Invoke-Step "Write the LAN token to $tokenPath (the value is not shown)" {
                [System.IO.File]::WriteAllText($tokenPath, $newToken)
            }
        }
    }
}
if ($doWeb) {
    Grant-Access $webDataDir @("${AdministratorsSid}:(OI)(CI)F", "${SystemSid}:(OI)(CI)F", "${WebAccount}:(OI)(CI)M") $true
    if ($newWebToken) {
        Invoke-Step "New-Item -ItemType File $webTokenPath" {
            if (-not (Test-Path -LiteralPath $webTokenPath)) { New-Item -ItemType File -Path $webTokenPath | Out-Null }
        }
        Grant-Access $webTokenPath @("${AdministratorsSid}:F", "${SystemSid}:F", "${WebAccount}:R") $true
        Invoke-Step "Write the backend token to $webTokenPath (the value is not shown)" {
            [System.IO.File]::WriteAllText($webTokenPath, $newWebToken)
        }
    }
}
$logEntries = @("${AdministratorsSid}:(OI)(CI)F", "${SystemSid}:(OI)(CI)F")
if ($doBackend -or $existingBackend) { $logEntries += "${ServiceAccount}:(OI)(CI)M" }
if ($doWeb -or $existingWeb) { $logEntries += "${WebAccount}:(OI)(CI)M" }
Grant-Access $logsDir $logEntries $true

# 6. Firewall: a rule only for a LAN listener, for the executable alone, on the Private profile.
function Set-FirewallRule([string]$Display, [int]$Port, [string]$Exe, [bool]$Wanted) {
    $present = $existingRules -contains $Display
    if ($Wanted) {
        $program = Join-Path $installDir $Exe
        if ($present) { Invoke-Step "Remove-NetFirewallRule -DisplayName `"$Display`"" { Remove-NetFirewallRule -DisplayName $Display } }
        Invoke-Step "New-NetFirewallRule -DisplayName `"$Display`" -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port -Program `"$program`" -Profile Private" {
            New-NetFirewallRule -DisplayName $Display -Direction Inbound -Action Allow -Protocol TCP -LocalPort $Port `
                -Program $program -Profile Private | Out-Null
        }
    } elseif ($present) {
        Invoke-Step "Remove-NetFirewallRule -DisplayName `"$Display`"" { Remove-NetFirewallRule -DisplayName $Display }
    }
}
if ($doBackend) { Set-FirewallRule $ServiceDisplayName $BackendPort 'nukleusd.exe' $backendIsLan }
if ($doWeb) { Set-FirewallRule $WebDisplayName $WebPort 'nukleus-web.exe' $webIsLan }

# 7. Start menu shortcut for the manager.
if ($doBackend) {
    $manager = Join-Path $installDir 'nukleus-manager.exe'
    Invoke-Step "Create the shortcut $shortcutPath -> $manager --gui" {
        $shell = New-Object -ComObject WScript.Shell
        $shortcut = $shell.CreateShortcut($shortcutPath)
        $shortcut.TargetPath = $manager
        $shortcut.Arguments = '--gui'
        $shortcut.WorkingDirectory = $installDir
        $shortcut.Description = 'Nukleus model manager'
        $shortcut.Save()
    }
}

# 8. Start the services.
if ($NoStart) {
    Write-Host 'The services were not started (-NoStart).'
} else {
    foreach ($name in $serviceNames) {
        Invoke-Step "Start-Service $name" { Start-Service -Name $name }
    }
}

Write-Host ($(if ($DryRun) { 'Dry run complete.' } elseif ($installing) { 'Installed Nukleus.' } else { 'Updated Nukleus.' }))
Write-Host "Data and logs are under $dataDir. Check the services with: Get-Service $($serviceNames -join ', ')"
if ($doBackend) {
    $addresses = if ($backendIsLan) { "127.0.0.1:$BackendPort and ${backendListen}:$BackendPort" } else { "127.0.0.1:$BackendPort" }
    Write-Host "The backend listens on $addresses."
}
if ($doWeb) {
    $addresses = if ($webIsLan) { "127.0.0.1:$WebPort and ${webListenAddress}:$WebPort" } else { "127.0.0.1:$WebPort" }
    Write-Host "The web interface listens on $addresses."
}
