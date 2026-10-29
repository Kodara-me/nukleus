<#
Tests the Windows installer and uninstaller by running them with -DryRun against a fake build
directory and a fake installed state (-StateFile), and checking the printed plan. Nothing is
registered, copied, secured or opened in the firewall.

Usage: pwsh -File NukleusWindowsInstallerTest.ps1 <project root>
#>
param([Parameter(Mandatory = $true)][string]$ProjectRoot)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$install = Join-Path $ProjectRoot 'scripts\install-nukleus.ps1'
$uninstall = Join-Path $ProjectRoot 'scripts\uninstall-nukleus.ps1'
$shell = (Get-Process -Id $PID).Path
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("nukleus-installer-test-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$emptyPath = Join-Path $work 'empty-path'
New-Item -ItemType Directory -Path $emptyPath | Out-Null

$programFiles = if ($env:ProgramW6432) { $env:ProgramW6432 } else { $env:ProgramFiles }
$installDir = Join-Path $programFiles 'Nukleus'
$dataDir = Join-Path $env:ProgramData 'Nukleus'
$shortcut = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\Nukleus Manager.lnk'
$system32 = Join-Path $env:SystemRoot 'System32'
# Existing programs outside any user profile, standing in for the runtime tools.
$tools = @('-SdCli', (Join-Path $system32 'cmd.exe'), '-LlamaServer', (Join-Path $system32 'whoami.exe'),
    '-LlamaCompletion', (Join-Path $system32 'hostname.exe'), '-Ffmpeg', (Join-Path $system32 'more.com'))

# The fake build directory: empty files are enough for a dry run.
$buildDir = Join-Path $work 'build'
New-Item -ItemType Directory -Path (Join-Path $buildDir 'src') | Out-Null
foreach ($name in @('nukleusd.exe', 'nukleus-manager.exe', 'nukleus-web.exe', 'Qt6Core.dll', 'archive.dll')) {
    New-Item -ItemType File -Path (Join-Path $buildDir "src\$name") | Out-Null
}
$qtDir = Join-Path $work 'Qt'

$failures = New-Object System.Collections.Generic.List[string]
function Assert-That([bool]$Condition, [string]$Message) {
    if (-not $Condition) { $failures.Add($Message); Write-Host "FAIL: $Message" }
}
function Assert-Match([string]$Text, [string]$Pattern, [string]$Message) {
    Assert-That ($Text -match $Pattern) "$Message (expected /$Pattern/)"
}
function Assert-NoMatch([string]$Text, [string]$Pattern, [string]$Message) {
    Assert-That (-not ($Text -match $Pattern)) "$Message (unexpected /$Pattern/)"
}

function Quote-Argument([string]$Value) { '"' + ($Value -replace '"', '\"') + '"' }

# Runs a script in a fresh shell with standard input closed, as a non-interactive session. The
# plan shows the service command line escaped as sc.exe receives it; the output unescapes it.
function Invoke-Script([string]$Script, [string[]]$Arguments, $State, [string]$SearchPath = $emptyPath) {
    $stateFile = Join-Path $work ([guid]::NewGuid().ToString('N') + '.json')
    $stateObject = if ($State) { $State } else { @{ services = @{}; tokenSet = $false; webBackendTokenSet = $false; firewall = @() } }
    ConvertTo-Json -InputObject $stateObject -Depth 6 | Set-Content -LiteralPath $stateFile -Encoding UTF8
    $info = New-Object System.Diagnostics.ProcessStartInfo
    $info.FileName = $shell
    $all = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $Script) + $Arguments + @('-DryRun', '-StateFile', $stateFile)
    $info.Arguments = ($all | ForEach-Object { Quote-Argument $_ }) -join ' '
    $info.UseShellExecute = $false
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.EnvironmentVariables['PATH'] = $SearchPath
    $process = [System.Diagnostics.Process]::Start($info)
    $process.StandardInput.Close()
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    return @{ Out = (($stdout.Result -replace "`r`n", "`n") -replace '\\"', '"'); Err = ($stderr.Result -replace "`r`n", "`n"); Code = $process.ExitCode }
}

function New-InstalledState($Backend, $Web, [bool]$TokenSet, [bool]$WebTokenSet, [string[]]$Firewall) {
    $services = @{}
    if ($Backend) { $services['nukleus'] = $Backend }
    if ($Web) { $services['nukleus-web'] = $Web }
    return @{ services = $services; tokenSet = $TokenSet; webBackendTokenSet = $WebTokenSet; firewall = $Firewall }
}

function Get-Snapshot {
    $items = @()
    foreach ($directory in @($installDir, $dataDir)) {
        if (Test-Path -LiteralPath $directory) {
            $items += Get-ChildItem -LiteralPath $directory -Recurse -Force -ErrorAction SilentlyContinue |
                ForEach-Object { "$($_.FullName)|$($_.LastWriteTimeUtc.Ticks)" }
        } else { $items += "absent:$directory" }
    }
    $items += "shortcut:$(Test-Path -LiteralPath $shortcut)"
    foreach ($name in @('nukleus', 'nukleus-web')) {
        $items += "service:${name}:$([bool](Get-Service -Name $name -ErrorAction SilentlyContinue))"
    }
    return ($items -join "`n")
}

try {
    $before = Get-Snapshot

    # ---- First install of the backend, loopback only (R13, R14, R15, R17, R20, R22) ----------
    $run = Invoke-Script $install (@('-InstallBackend', '-BuildDir', $buildDir, '-QtDir', $qtDir) + $tools) $null
    $out = $run.Out
    Assert-That ($run.Code -eq 0) "backend install plan exits 0 (got $($run.Code)): $($run.Err)"
    Assert-Match $out '(?m)^Installing Nukleus\.' 'a first install says so'
    Assert-Match $out '(?m)^\+ sc\.exe create nukleus binPath= "' 'nukleus is registered with sc.exe create'
    Assert-Match $out ([regex]::Escape("`"$installDir\nukleusd.exe`" --listen 127.0.0.1 --port 9384 --token-file `"$dataDir\config\token`" --models-dir `"$dataDir\models`" --config-dir `"$dataDir\config`"")) 'the daemon command line holds listen, port, token file and the split layout'
    Assert-Match $out 'start= auto obj= "NT SERVICE\\nukleus" DisplayName= "Nukleus"' 'automatic start, virtual account and display name of nukleus'
    Assert-Match $out '(?m)^\+ sc\.exe failure nukleus reset= \d+ actions= restart/5000' 'nukleus restarts after 5 seconds'
    Assert-NoMatch $out 'nukleus-web' 'the backend alone plans nothing for the web service'
    foreach ($variable in @('NUKLEUS_SD_CLI', 'NUKLEUS_LLAMA_SERVER', 'NUKLEUS_LLAMA_COMPLETION', 'NUKLEUS_FFMPEG')) {
        Assert-Match $out "(?m)^    $variable=[A-Za-z]:\\" "the environment holds $variable as an absolute path"
    }
    Assert-Match $out ('(?m)^    NUKLEUS_SD_CLI=' + [regex]::Escape((Join-Path $system32 'cmd.exe')) + '$') 'the sd-cli path is the one given'
    Assert-Match $out ('(?m)^    NUKLEUS_LOG_FILE=' + [regex]::Escape("$dataDir\logs\nukleusd.log")) 'the daemon log file'
    Assert-Match $out ('(?m)^    TEMP=' + [regex]::Escape("$dataDir\tmp") + '$') 'TEMP is the private tmp directory'
    Assert-Match $out ('(?m)^    TMP=' + [regex]::Escape("$dataDir\tmp") + '$') 'TMP is the private tmp directory'
    Assert-Match $out ('(?m)^\+ New-Item -ItemType Directory ' + [regex]::Escape("$dataDir\models")) 'models directory'
    Assert-Match $out ('(?m)^\+ New-Item -ItemType Directory ' + [regex]::Escape("$dataDir\config\catalog")) 'catalog directory'
    Assert-Match $out ('(?m)^\+ Copy-Item .*nukleusd\.exe -> ' + [regex]::Escape($installDir)) 'the daemon is copied'
    Assert-Match $out ('(?m)^\+ Copy-Item .*\*\.dll -> ' + [regex]::Escape($installDir)) 'the DLLs next to it are copied'
    Assert-Match $out ('(?m)^\+ .*windeployqt\.exe ' + [regex]::Escape("$installDir\nukleusd.exe")) 'windeployqt runs on the installed daemon'
    Assert-Match $out ('(?m)^\+ Create the shortcut ' + [regex]::Escape($shortcut) + ' -> .*nukleus-manager\.exe --gui') 'the Start menu shortcut starts the manager with --gui'
    Assert-NoMatch $out 'NetFirewallRule' 'loopback only has no firewall rule'
    Assert-Match $out '(?m)^\+ Start-Service nukleus$' 'the service is started'
    $configAcl = ($out -split "`n" | Where-Object { $_ -match '^\+ icacls .*\\config /inheritance' }) -join "`n"
    Assert-Match $configAcl ([regex]::Escape('*S-1-5-32-544:(OI)(CI)F') + '.*' + [regex]::Escape('*S-1-5-18:(OI)(CI)F') + '.*NT SERVICE\\nukleus:\(OI\)\(CI\)M') 'config\ is for Administrators, SYSTEM and the daemon account'
    $tmpAcl = ($out -split "`n" | Where-Object { $_ -match '^\+ icacls .*\\tmp /inheritance' }) -join "`n"
    Assert-Match $tmpAcl 'NT SERVICE\\nukleus:\(OI\)\(CI\)M' 'tmp\ is for the daemon account'
    Assert-Match $out '(?m)^\+ icacls .*\\logs /inheritance:r /grant:r .*NT SERVICE\\nukleus:' 'logs\ is for the daemon account'
    Assert-Match $out '(?m)^\+ icacls .*\\models /grant .*NT SERVICE\\nukleus:\(OI\)\(CI\)M' 'the daemon account writes the weight directory'
    Assert-NoMatch $out '(?m)^\+ icacls .*\\models /inheritance' 'the permissions of the weight directory are kept'
    Assert-Match $out '(?m)^\+ icacls .*\\config\\token /inheritance:r /grant:r .*NT SERVICE\\nukleus:R' 'the token file is readable by the daemon account'

    # ---- A dry run changes nothing (R20) -------------------------------------------------------
    Assert-That ((Get-Snapshot) -eq $before) 'a dry run changes no file, directory, shortcut or service'

    # ---- Web service on its own: remote backend and its token (R16) ------------------------------
    $run = Invoke-Script $install @('-InstallWebService', '-BuildDir', $buildDir, '-QtDir', $qtDir, '-WebBackendHost', '192.168.1.10', '-WebBackendToken', 'web-secret-value') $null
    $out = $run.Out
    Assert-That ($run.Code -eq 0) "web-only install plan exits 0 (got $($run.Code)): $($run.Err)"
    Assert-Match $out '(?m)^\+ sc\.exe create nukleus-web binPath= "' 'nukleus-web is registered'
    Assert-Match $out 'obj= "NT SERVICE\\nukleus-web" DisplayName= "Nukleus web interface"' 'virtual account and display name of nukleus-web'
    Assert-NoMatch $out 'sc\.exe create nukleus ' 'the web component alone does not register the daemon'
    Assert-NoMatch $out 'nukleus-manager' 'the web component alone installs no manager'
    Assert-Match $out '(?m)^    NUKLEUS_WEB_BACKEND_HOST=192\.168\.1\.10$' 'backend host of the web service'
    Assert-Match $out ('(?m)^    NUKLEUS_WEB_BACKEND_TOKEN_FILE=' + [regex]::Escape("$dataDir\web\backend-token") + '$') 'backend token file of the web service'
    Assert-Match $out ('(?m)^    NUKLEUS_LOG_FILE=' + [regex]::Escape("$dataDir\logs\nukleus-web.log")) 'the web log file'
    Assert-Match $out '(?m)^\+ icacls .*\\web\\backend-token /inheritance:r /grant:r .*NT SERVICE\\nukleus-web:R' 'the backend token is readable by the web account'
    Assert-Match $out '(?m)^\+ icacls .*\\web /inheritance:r /grant:r .*NT SERVICE\\nukleus-web:' 'web\ is for the web account'
    Assert-NoMatch ($out -split "`n" | Where-Object { $_ -match '\\web /inheritance' }) 'NT SERVICE\\nukleus:' 'web\ is closed to the daemon account'
    Assert-NoMatch $out 'web-secret-value' 'the token is never printed'

    # ---- Both components, with a LAN address (R16, R18) ----------------------------------------
    $both = @('-InstallBackend', '-InstallWebService', '-BuildDir', $buildDir, '-QtDir', $qtDir, '-Listen', '10.0.0.93',
        '-Token', 'lan-secret-value', '-WebListen', '192.168.1.20') + $tools
    $run = Invoke-Script $install $both $null
    $out = $run.Out
    Assert-That ($run.Code -eq 0) "both-component install plan exits 0 (got $($run.Code)): $($run.Err)"
    Assert-Match $out '--listen 10\.0\.0\.93 --port 9384' 'the daemon listens on the LAN address'
    Assert-Match $out ('(?m)^\+ New-NetFirewallRule -DisplayName "Nukleus" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 9384 -Program "' + [regex]::Escape("$installDir\nukleusd.exe") + '" -Profile Private$') 'a Private rule for the daemon port and executable'
    Assert-Match $out ('(?m)^\+ New-NetFirewallRule -DisplayName "Nukleus web interface" -Direction Inbound -Action Allow -Protocol TCP -LocalPort 9385 -Program "' + [regex]::Escape("$installDir\nukleus-web.exe") + '" -Profile Private$') 'a Private rule for the web port and executable'
    Assert-Match $out '(?m)^    NUKLEUS_WEB_LISTEN=192\.168\.1\.20$' 'the web service listens on the LAN address'
    Assert-NoMatch $out 'NUKLEUS_WEB_BACKEND_HOST' 'a backend installed by the same run is reached over loopback'
    Assert-Match $out 'Write the LAN token to .*\\config\\token' 'the LAN token is written to config\token'
    Assert-NoMatch $out 'lan-secret-value' 'the token is never printed'
    Assert-Match $out '(?m)^\+ icacls .*\\logs /inheritance:r /grant:r .*nukleus:.*nukleus-web:' 'logs\ is for both service accounts'

    # ---- An earlier rule is removed when the listener is loopback only (R18) --------------------
    $stale = New-InstalledState @{ imagePath = "`"$installDir\nukleusd.exe`" --listen 127.0.0.1 --port 9384"; environment = @() } $null $false $false @('Nukleus')
    $run = Invoke-Script $install @('-BuildDir', $buildDir, '-QtDir', $qtDir) $stale
    Assert-Match $run.Out '(?m)^\+ Remove-NetFirewallRule -DisplayName "Nukleus"$' 'a rule left by an earlier run is removed'
    Assert-NoMatch $run.Out 'New-NetFirewallRule' 'no rule for a loopback-only listener'

    # ---- Update keeps the configuration and restarts the services (R19) --------------------------
    $backendState = @{
        imagePath = "`"$installDir\nukleusd.exe`" --listen 10.0.0.93 --port 9384 --token-file `"$dataDir\config\token`" --models-dir `"D:\lab\models`" --config-dir `"D:\lab\config`""
        environment = @('NUKLEUS_SD_CLI=D:\tools\sd-cli.exe', 'NUKLEUS_FFMPEG=D:\tools\ffmpeg.exe', 'NUKLEUS_LOG_FILE=old.log')
    }
    $webState = @{ imagePath = "`"$installDir\nukleus-web.exe`""; environment = @('NUKLEUS_WEB_LISTEN=192.168.1.20') }
    $installed = New-InstalledState $backendState $webState $true $false @('Nukleus', 'Nukleus web interface')
    $run = Invoke-Script $install @('-BuildDir', $buildDir, '-QtDir', $qtDir) $installed
    $out = $run.Out
    Assert-That ($run.Code -eq 0) "update plan exits 0 (got $($run.Code)): $($run.Err)"
    Assert-Match $out '(?m)^Updating Nukleus\.' 'an update says so'
    Assert-Match $out '(?m)^\+ Stop-Service nukleus-web$' 'the web service is stopped for the update'
    Assert-Match $out '(?m)^\+ Stop-Service nukleus$' 'the daemon is stopped for the update'
    Assert-Match $out '(?m)^\+ sc\.exe config nukleus binPath= ' 'the existing service is reconfigured, not created again'
    Assert-NoMatch $out 'sc\.exe create' 'no service is created again'
    Assert-Match $out '--listen 10\.0\.0\.93 --port 9384' 'the listen address is kept'
    Assert-Match $out ([regex]::Escape('--models-dir "D:\lab\models" --config-dir "D:\lab\config"')) 'the storage directories are kept'
    Assert-Match $out '(?m)^    NUKLEUS_SD_CLI=D:\\tools\\sd-cli\.exe$' 'the sd-cli path is kept'
    Assert-Match $out '(?m)^    NUKLEUS_FFMPEG=D:\\tools\\ffmpeg\.exe$' 'the ffmpeg path is kept'
    Assert-Match $out '(?m)^    NUKLEUS_WEB_LISTEN=192\.168\.1\.20$' 'the web listen address is kept'
    Assert-NoMatch $out 'Write the LAN token|New-Item -ItemType File' 'the existing token is kept'
    Assert-Match $out '(?m)^\+ Remove-NetFirewallRule -DisplayName "Nukleus"$' 'the LAN rule of the daemon is renewed'
    Assert-Match $out '(?m)^\+ New-NetFirewallRule -DisplayName "Nukleus" ' 'the LAN rule of the daemon is added again'
    Assert-Match $out '(?m)^\+ Start-Service nukleus$' 'the daemon is restarted'
    Assert-Match $out '(?m)^\+ Start-Service nukleus-web$' 'the web service is restarted'
    Assert-NoMatch $out 'catalog\.json|Remove-Item' 'the update touches neither models nor the catalogue'
    Assert-Match $out '(?m)^\+ icacls D:\\lab\\models /grant ' 'the permissions of a directory given by option are extended, not replaced'

    # ---- -NoStart ------------------------------------------------------------------------------
    $run = Invoke-Script $install (@('-InstallBackend', '-BuildDir', $buildDir, '-QtDir', $qtDir, '-NoStart') + $tools) $null
    Assert-NoMatch $run.Out 'Start-Service' '-NoStart starts nothing'

    # ---- Options for the storage directories ---------------------------------------------------
    $run = Invoke-Script $install (@('-InstallBackend', '-BuildDir', $buildDir, '-QtDir', $qtDir, '-ModelsDir', 'D:\lab\models', '-ConfigDir', 'D:\lab\config') + $tools) $null
    Assert-Match $run.Out ([regex]::Escape('--models-dir "D:\lab\models" --config-dir "D:\lab\config"')) '-ModelsDir and -ConfigDir reach the command line'
    Assert-Match $run.Out ([regex]::Escape('+ New-Item -ItemType Directory D:\lab\config\catalog')) 'the catalogue directory is created under -ConfigDir'
    Assert-Match $run.Out ([regex]::Escape("--token-file `"$dataDir\config\token`"")) 'the token stays in the ProgramData config directory'

    # ---- Refusals, all before any change (R13, R15, R16, R17) ----------------------------------
    function Assert-Refusal($Run, [string]$Pattern, [string]$Message) {
        Assert-That ($Run.Code -ne 0) "${Message}: exits nonzero"
        Assert-Match $Run.Err $Pattern "${Message}: message"
        Assert-NoMatch $Run.Out '(?m)^\+ ' "${Message}: nothing was planned"
    }
    Assert-Refusal (Invoke-Script $install @('-BuildDir', $buildDir) $null) '-InstallBackend' 'no component on a machine with nothing installed'
    Assert-Refusal (Invoke-Script $install (@('-InstallBackend', '-BuildDir', $buildDir, '-ModelsDir', 'D:\lab\models') + $tools) $null) '-ConfigDir' '-ModelsDir without -ConfigDir'
    Assert-Refusal (Invoke-Script $install (@('-InstallBackend', '-BuildDir', $buildDir, '-ConfigDir', 'D:\lab\config') + $tools) $null) '-ModelsDir' '-ConfigDir without -ModelsDir'
    Assert-Refusal (Invoke-Script $install (@('-InstallBackend', '-BuildDir', $buildDir, '-Listen', '10.0.0.93') + $tools) $null) '-Token' 'a LAN address without a token in a non-interactive session'
    $profilePath = Join-Path $env:SystemDrive 'Users\alice\AppData\Local\Programs\llama.cpp\llama-server.exe'
    $userTools = @('-SdCli', (Join-Path $system32 'cmd.exe'), '-LlamaServer', $profilePath, '-LlamaCompletion', (Join-Path $system32 'hostname.exe'), '-Ffmpeg', (Join-Path $system32 'more.com'))
    $run = Invoke-Script $install (@('-InstallBackend', '-BuildDir', $buildDir) + $userTools) $null
    Assert-Refusal $run 'llama-server' 'a tool path under a user profile'
    Assert-Match $run.Err ([regex]::Escape($profilePath)) 'the refusal names the path'
    Assert-Match $run.Err '--scope machine' 'the refusal names the fix'
    Assert-Match $run.Err '-LlamaServer' 'the refusal names the option'
    Assert-Refusal (Invoke-Script $install @('-InstallWebService', '-BuildDir', $buildDir) $null) '-WebBackendHost' 'the web service alone without a backend host'
    Assert-Refusal (Invoke-Script $install @('-InstallWebService', '-BuildDir', $buildDir, '-WebBackendHost', '192.168.1.10') $null) '-WebBackendToken' 'a remote backend without a token'
    Assert-Refusal (Invoke-Script $install @('-InstallBackend', '-BuildDir', (Join-Path $work 'missing')) $null) 'was not found' 'a build directory without executables'

    # ---- A tool that cannot be found is a warning (R17) -----------------------------------------
    $run = Invoke-Script $install @('-InstallBackend', '-BuildDir', $buildDir, '-QtDir', $qtDir) $null
    Assert-That ($run.Code -eq 0) "missing tools do not stop the installer (got $($run.Code)): $($run.Err)"
    foreach ($tool in @('sd-cli', 'llama-server', 'llama-completion', 'ffmpeg')) {
        Assert-Match $run.Out "(?m)^Warning: $tool was not found on PATH" "warning for $tool"
    }
    Assert-NoMatch $run.Out '(?m)^    NUKLEUS_SD_CLI=' 'no path is stored for a tool that was not found'

    # ---- Without elevation, a real run stops before any change (R12) ----------------------------
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $info = New-Object System.Diagnostics.ProcessStartInfo
        $info.FileName = $shell
        $info.Arguments = "-NoProfile -ExecutionPolicy Bypass -File $(Quote-Argument $install) -InstallBackend -BuildDir $(Quote-Argument $buildDir)"
        $info.UseShellExecute = $false
        $info.RedirectStandardInput = $true
        $info.RedirectStandardOutput = $true
        $info.RedirectStandardError = $true
        $process = [System.Diagnostics.Process]::Start($info)
        $process.StandardInput.Close()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        Assert-That ($process.ExitCode -ne 0) 'a non-elevated install exits nonzero'
        Assert-Match $stderr.Result 'elevated' 'a non-elevated install says why'
        Assert-NoMatch $stdout.Result '(?m)^\+ ' 'a non-elevated install plans nothing'
    } else {
        Write-Host 'SKIP: the non-elevated refusal needs a non-elevated session.'
    }

    # ---- Uninstall, with and without -Purge (R21) -----------------------------------------------
    $run = Invoke-Script $uninstall @() $installed
    $out = $run.Out
    Assert-That ($run.Code -eq 0) "uninstall plan exits 0 (got $($run.Code)): $($run.Err)"
    foreach ($name in @('nukleus', 'nukleus-web')) {
        Assert-Match $out "(?m)^\+ Stop-Service $name$" "$name is stopped"
        Assert-Match $out "(?m)^\+ sc\.exe delete $name$" "$name is deleted"
    }
    Assert-Match $out '(?m)^\+ Remove-NetFirewallRule -DisplayName "Nukleus"$' 'the daemon rule is removed'
    Assert-Match $out '(?m)^\+ Remove-NetFirewallRule -DisplayName "Nukleus web interface"$' 'the web rule is removed'
    Assert-Match $out ('(?m)^\+ Remove-Item ' + [regex]::Escape($shortcut) + '$') 'the shortcut is removed'
    Assert-Match $out ('(?m)^\+ Remove-Item -Recurse ' + [regex]::Escape($installDir) + '$') 'the program directory is removed'
    Assert-NoMatch $out ('(?m)^\+ Remove-Item -Recurse ' + [regex]::Escape($dataDir) + '$') 'the data directory is kept'
    Assert-Match $out ('(?m)^Kept: ' + [regex]::Escape($dataDir) + '$') 'the kept data directory is printed'
    Assert-Match $out '(?m)^Kept: D:\\lab\\models$' 'the weight directory given by option is printed'
    Assert-Match $out '(?m)^Kept: D:\\lab\\config$' 'the configuration directory given by option is printed'

    $run = Invoke-Script $uninstall @('-Purge', '-Yes') $installed
    $out = $run.Out
    Assert-Match $out ('(?m)^\+ Remove-Item -Recurse ' + [regex]::Escape($dataDir) + '$') '-Purge -Yes removes the data directory'
    Assert-NoMatch $out '(?m)^\+ Remove-Item.*D:\\lab' 'a purge never removes a directory outside the data directory'
    Assert-Match $out '(?m)^Kept: D:\\lab\\models$' 'a purge still keeps the weight directory given by option'

    $run = Invoke-Script $uninstall @('-Purge') $installed
    Assert-NoMatch $run.Out ('(?m)^\+ Remove-Item -Recurse ' + [regex]::Escape($dataDir) + '$') '-Purge without confirmation keeps the data'

    $run = Invoke-Script $uninstall @() $null
    Assert-That ($run.Code -eq 0) 'uninstalling a machine with nothing installed succeeds'
    Assert-NoMatch $run.Out 'sc\.exe delete' 'no service is deleted when none is installed'

    Assert-That ((Get-Snapshot) -eq $before) 'no dry run changed the machine'
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host "$($failures.Count) check(s) failed."
    exit 1
}
Write-Host 'All Windows installer checks passed.'
exit 0
