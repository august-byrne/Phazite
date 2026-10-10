#Requires -Version 5.1
<#
.SYNOPSIS
    Installs or updates everything Phazite needs to build on Windows, then prints the versions in use.

.DESCRIPTION
    Safe to run again at any time: missing tools are installed, installed tools are updated, and
    nothing else changes. It asks for administrator rights once (one UAC prompt) and runs the installers
    in a second window. The simplest way to run it is tools\setup-windows.cmd, which needs no
    execution-policy change.

    From winget's official source (Windows' built-in package manager), machine-wide:
      Git.Git                            Git for Windows
      Kitware.CMake                      CMake (MSI, added to the system PATH)
      Ninja-build.Ninja                  Ninja (linked from C:\Program Files\WinGet\Links)
      Microsoft.VisualStudio.BuildTools  VS 2026 Build Tools: MSVC x64/x86 + Windows 11 SDK, no IDE
    Optional: LLVM.LLVM (-WithLlvm) and the Surge XT VST3 test plug-in (-WithTestPlugin).
    Also (admin): crash dumps for Phazite.exe in %LOCALAPPDATA%\CrashDumps (Windows Error Reporting).
    Per user, without admin rights: git settings (long paths, HTTPS for JUCE) and the VS Code
    C/C++ and CMake Tools extensions.

    Exit codes (the last line printed, "Result: ...", says the same; Claude Code's shell tool shows
    3010 as 194): 0 = done; 3010 = done, but restart Windows and run it again; 1 = a step failed (see the
    FAILED lines and the log); 2 = could not start (no winget, administrator rights refused, ...).
    With -CheckOnly: 0 = everything present; 1 = something missing or not on this shell's PATH yet;
    3010 = Visual Studio is waiting for a reboot.

.EXAMPLE
    tools\setup-windows.cmd
.EXAMPLE
    tools\setup-windows.cmd -CheckOnly
#>
[CmdletBinding()]
param(
    [switch]$CheckOnly,             # only print what is installed; change nothing (no admin needed)
    [switch]$SkipUpdates,           # install what is missing, but leave installed tools as they are
    [switch]$WithLlvm,              # also install LLVM (clangd, clang-tidy, clang-format)
    [switch]$WithTestPlugin,        # also install Surge XT (GPL-3.0 VST3) to test plug-in scanning
    [switch]$SkipVSCodeExtensions,  # don't install/update the VS Code C/C++ and CMake Tools extensions
    [switch]$AdminStepsOnly,        # internal: the elevated copy runs only the admin part
    [string]$LogPath                # internal: log file shared by the elevated copy and its parent
)

$ErrorActionPreference = 'Stop'
$script:InheritedPath = $env:Path   # PATH of the program that started this script (to spot stale shells)
$script:LogPath = $LogPath
$script:Failed = New-Object System.Collections.Generic.List[string]
$script:RebootRequired = $false
$script:Missing = $false
$script:StalePath = $false

# winget exit codes. Windows PowerShell 5.1 reads these 8-digit hex literals as the same negative
# Int32 values that $LASTEXITCODE holds.
$WG_UPDATE_NOT_APPLICABLE       = 0x8A15002B  # installed, and nothing newer is available
$WG_ALREADY_INSTALLED           = 0x8A150061
$WG_APP_IN_USE                  = 0x8A150101  # the application is running
$WG_INSTALL_IN_PROGRESS         = 0x8A150102  # another installer is running
$WG_FILE_IN_USE                 = 0x8A150103  # one or more files are in use
$WG_REBOOT_REQUIRED_FOR_INSTALL = 0x8A15010A
$WG_REBOOT_INITIATED            = 0x8A15010B

# VS 2026 Build Tools components. The C++ workload only *recommends* the compiler and the SDK, so both
# are listed explicitly (--includeRecommended would also add CMake, vcpkg and more).
$VsComponents = @(
    'Microsoft.VisualStudio.Workload.VCTools',             # "Desktop development with C++" (required parts)
    'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',   # MSVC compiler, latest version
    'Microsoft.VisualStudio.Component.Windows11SDK.26100'  # Windows 11 SDK
)
$VsCodeExtensions = @('ms-vscode.cpptools', 'ms-vscode.cmake-tools')

$VsInstallerDir    = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
$Vswhere           = Join-Path $VsInstallerDir 'vswhere.exe'
$VsSetup           = Join-Path $VsInstallerDir 'setup.exe'
$WindowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$CrashDumpKey      = 'HKLM:\SOFTWARE\Microsoft\Windows\Windows Error Reporting\LocalDumps\Phazite.exe'

#region Helpers ----------------------------------------------------------------------------------------
function Write-Log {
    param([string]$Message = '', [ConsoleColor]$Color = 'Gray')
    Write-Host $Message -ForegroundColor $Color
    if ($script:LogPath) { Add-Content -LiteralPath $script:LogPath -Value $Message -Encoding UTF8 }
}

function Invoke-Step([string]$Name, [scriptblock]$Body) {
    # Runs one step; a failure is reported and remembered, and the remaining steps still run.
    try { & $Body }
    catch { Write-Log ('FAILED ({0}): {1}' -f $Name, $_.Exception.Message) Red; $script:Failed.Add($Name) }
}

function Test-IsAdmin {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Update-SessionPath {
    # Rebuild this session's PATH from the registry (Machine, then User), the same way winget does,
    # so tools installed a moment ago can be found without opening a new window.
    $seen = @{}
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($scope in @('Machine', 'User')) {
        $value = [Environment]::GetEnvironmentVariable('Path', $scope)
        if (-not $value) { continue }
        foreach ($entry in $value.Split(';')) {
            $e = $entry.Trim()
            $key = $e.TrimEnd('\').ToLowerInvariant()
            if ($key -and -not $seen.ContainsKey($key)) { $seen[$key] = $true; $parts.Add($e) }
        }
    }
    $env:Path = $parts -join ';'
}

function Invoke-Native {
    # Runs a console program, copies its output to the log, and returns its exit code.
    param([string]$FilePath, [string[]]$Arguments, [switch]$Quiet)
    $ErrorActionPreference = 'Continue'   # stderr lines must not become terminating errors in PS 5.1
    if ($Quiet) { & $FilePath @Arguments 2>&1 | Out-Null }
    else { & $FilePath @Arguments 2>&1 | ForEach-Object { Write-Log ('    ' + $_) DarkGray } }
    return $LASTEXITCODE
}

function Test-PendingReboot {
    if (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Component Based Servicing\RebootPending') { return $true }
    if (Test-Path 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\WindowsUpdate\Auto Update\RebootRequired') { return $true }
    $pending = (Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager' `
                    -Name PendingFileRenameOperations -ErrorAction SilentlyContinue).PendingFileRenameOperations
    return ($null -ne $pending -and @($pending).Count -gt 0)
}

function Wait-IfDoubleClicked {
    # Keep the window open when setup-windows.cmd was started from Explorer, so the report stays readable.
    try {
        if ([Console]::IsInputRedirected) { return }   # Claude Code, CI: no interactive console
        $me = Get-CimInstance Win32_Process -Filter ('ProcessId={0}' -f $PID)
        $parent = Get-CimInstance Win32_Process -Filter ('ProcessId={0}' -f $me.ParentProcessId)
        $grandparent = Get-CimInstance Win32_Process -Filter ('ProcessId={0}' -f $parent.ParentProcessId)
        if ($parent.Name -ieq 'cmd.exe' -and $parent.CommandLine -match '(?i)\s/c\s' -and $grandparent.Name -ieq 'explorer.exe') {
            [void](Read-Host 'Press Enter to close this window')
        }
    } catch { }
}

function Exit-Script([int]$Code) {
    # Every user-facing run ends with one line that states the result, then exits with that code.
    if (-not $AdminStepsOnly) {
        if ($CheckOnly) {
            $meaning = switch ($Code) { 0 { 'everything present' } 1 { "something is missing or not on this shell's PATH" } 3010 { 'Visual Studio is waiting for a reboot' } default { 'could not check' } }
        }
        else {
            $meaning = switch ($Code) { 0 { 'done' } 1 { 'a step failed' } 2 { 'could not start' } 3010 { 'reboot required' } default { 'unexpected' } }
        }
        Write-Log ('Result: exit code {0} ({1})' -f $Code, $meaning)
        Wait-IfDoubleClicked
    }
    exit $Code
}
#endregion

#region winget packages ---------------------------------------------------------------------------------
function Install-WingetPackage {
    # Installs the package if it is missing, otherwise upgrades it; "nothing newer" counts as success.
    param([Parameter(Mandatory = $true)][string]$Id, [string[]]$Extra = @())
    $query = @('--id', $Id, '--exact', '--source', 'winget', '--accept-source-agreements', '--disable-interactivity')
    $installed = ((Invoke-Native $script:Winget (@('list') + $query) -Quiet) -eq 0)
    if ($installed -and $SkipUpdates) { Write-Log ('==> {0}: installed (updates skipped)' -f $Id) Green; return }
    $verb = 'install'
    if ($installed) { $verb = 'upgrade' }
    Write-Log ('==> {0}: winget {1}' -f $Id, $verb) Cyan
    $wingetArgs = @($verb) + $query + @('--accept-package-agreements', '--silent', '--no-progress') + $Extra
    $code = Invoke-Native $script:Winget $wingetArgs
    if ($code -eq $WG_INSTALL_IN_PROGRESS) {
        Write-Log '    another installation is running; trying again in 60 seconds' Yellow
        Start-Sleep -Seconds 60
        $code = Invoke-Native $script:Winget $wingetArgs
    }
    if ($code -eq 0) { return }
    if ($code -eq $WG_UPDATE_NOT_APPLICABLE -or $code -eq $WG_ALREADY_INSTALLED) { Write-Log '    already up to date' Green; return }
    if ($code -eq $WG_APP_IN_USE -or $code -eq $WG_FILE_IN_USE) {
        throw ('{0} is in use. Close VS Code and any running build, then run this script again.' -f $Id)
    }
    if ($code -eq $WG_REBOOT_REQUIRED_FOR_INSTALL -or $code -eq $WG_REBOOT_INITIATED) {
        $script:RebootRequired = $true
        throw ('{0} needs a reboot before it can be installed. Restart Windows, then run this script again.' -f $Id)
    }
    throw ('winget {0} {1} failed with 0x{2:X8} (run "winget error 0x{2:X8}" for details)' -f $verb, $Id, $code)
}
#endregion

#region Visual Studio Build Tools ----------------------------------------------------------------------
function Get-BuildToolsInstance {
    # The newest VS 2026 (18.x) Build Tools instance. -all also returns incomplete instances and ones
    # that are waiting for a reboot.
    if (-not (Test-Path -LiteralPath $Vswhere)) { return $null }
    $raw = & $Vswhere -products 'Microsoft.VisualStudio.Product.BuildTools' -version '[18.0,19.0)' -all -format json -utf8 -nologo
    if (-not $raw) { return $null }
    $items = ($raw -join "`n") | ConvertFrom-Json   # assign first: PS 5.1 emits a JSON array as one object
    $best = $null
    foreach ($i in @($items)) {
        if ($null -eq $best -or [version]$i.installationVersion -gt [version]$best.installationVersion) { $best = $i }
    }
    return $best
}

function Get-MsvcToolsetVersion([string]$InstallPath) {
    $file = Join-Path $InstallPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
    if (Test-Path -LiteralPath $file) { return (Get-Content -LiteralPath $file -TotalCount 1).Trim() }
    return $null
}

function Test-BuildToolsComponents([string]$InstallPath) {
    # True only if the instance is complete (no -all) and has every component, as the toolchain requires.
    $vswhereArgs = @('-products', '*', '-version', '[18.0,19.0)', '-requires') + $VsComponents + @('-property', 'installationPath', '-nologo')
    return (@(& $Vswhere @vswhereArgs) -contains $InstallPath)
}

function Get-VsInstallerProcess {
    @(Get-Process -Name 'setup' -ErrorAction SilentlyContinue |
      Where-Object { $_.Path -and $_.Path.StartsWith($VsInstallerDir + '\', [StringComparison]::OrdinalIgnoreCase) })
}

function Invoke-VsInstaller([string]$ArgumentLine) {
    # setup.exe has no --wait option and must not be started from its own folder. It can update itself and
    # restart; as Microsoft's VS DSC module does, wait for the first process, then for any restarted copy,
    # and judge the restarted copy's exit code (that is the real result).
    if ((Get-VsInstallerProcess).Count -gt 0) {
        throw 'The Visual Studio Installer is already running. Close its window (or let it finish), then run this script again.'
    }
    Write-Log ('    setup.exe ' + $ArgumentLine) DarkGray
    $p = Start-Process -FilePath $VsSetup -ArgumentList $ArgumentLine -WorkingDirectory $env:TEMP -PassThru
    $null = $p.Handle   # keeps ExitCode readable even if setup.exe exits immediately
    $p.WaitForExit()
    $code = $p.ExitCode
    $restarted = @()
    for ($i = 0; $i -lt 5 -and $restarted.Count -eq 0; $i++) { Start-Sleep -Seconds 1; $restarted = Get-VsInstallerProcess }
    if ($restarted.Count -gt 0) {
        Write-Log '    the Visual Studio Installer restarted itself; waiting for it to finish' DarkGray
        foreach ($r in $restarted) { try { $null = $r.Handle } catch { } }
        $restarted | Wait-Process -Timeout 7200 -ErrorAction SilentlyContinue
        $code = 0
        foreach ($r in $restarted) {
            if (-not $r.HasExited) { throw 'The Visual Studio Installer is still running after 2 hours.' }
            try { if ($r.ExitCode -ne 0) { $code = $r.ExitCode } } catch { }
        }
    }
    switch ($code) {
        0       { return }
        3010    { $script:RebootRequired = $true; return }   # done, reboot needed before use
        862968  { $script:RebootRequired = $true; return }   # done, reboot recommended
        default { throw ('Visual Studio Installer failed with exit code {0} (740 = not elevated, 1001/1618 = another install running, 1003/8006 = close running builds). Logs: %TEMP%\dd_*.log' -f $code) }
    }
}

function Install-BuildTools {
    $adds = ($VsComponents | ForEach-Object { '--add ' + $_ }) -join ' '
    $vs = Get-BuildToolsInstance
    $toolsetBefore = $null

    if ($null -eq $vs) {
        Write-Log '==> Microsoft.VisualStudio.BuildTools: winget install (about 2 GB download, 5-7 GB on disk, 15-30 minutes)' Cyan
        # --override replaces every default installer switch, so --wait (return only when finished) is
        # passed explicitly. The component IDs contain no spaces, so no nested quotes are needed.
        $override = '--passive --wait --norestart ' + $adds
        $code = Invoke-Native $script:Winget @('install', '--id', 'Microsoft.VisualStudio.BuildTools', '--exact',
            '--source', 'winget', '--accept-package-agreements', '--accept-source-agreements',
            '--disable-interactivity', '--no-progress', '--override', $override)
        if ($code -ne 0) { throw ('VS Build Tools installation failed with 0x{0:X8}' -f $code) }
    }
    elseif ($vs.isRebootRequired) {
        $script:RebootRequired = $true
        Write-Log '    Visual Studio is waiting for a reboot. Restart Windows, then run this script again.' Yellow
        return
    }
    elseif ($SkipUpdates -or -not $vs.isComplete) {
        Write-Log ('==> VS Build Tools {0}: update skipped' -f $vs.installationVersion) Green
    }
    else {
        $toolsetBefore = Get-MsvcToolsetVersion $vs.installationPath
        Write-Log ('==> VS Build Tools {0}: update' -f $vs.installationVersion) Cyan
        Invoke-VsInstaller ('update --installPath "{0}" --passive --norestart' -f $vs.installationPath)
    }

    $vs = Get-BuildToolsInstance
    if ($null -eq $vs) { throw 'vswhere does not report a VS 2026 Build Tools installation.' }
    if ($vs.isRebootRequired) {
        $script:RebootRequired = $true
        Write-Log '    Visual Studio needs a reboot to finish; the component check runs next time.' Yellow
        return
    }
    if (-not (Test-BuildToolsComponents $vs.installationPath)) {
        Write-Log '==> VS Build Tools: incomplete or missing components; running modify' Cyan
        Invoke-VsInstaller ('modify --installPath "{0}" {1} --passive --norestart' -f $vs.installationPath, $adds)
        $vs = Get-BuildToolsInstance
        if ($vs.isRebootRequired) { $script:RebootRequired = $true; return }
        if (-not (Test-BuildToolsComponents $vs.installationPath)) {
            throw ('VS Build Tools at "{0}" is still incomplete. Repair it with: "{1}" repair --installPath "{0}" --passive --norestart (then run this script again; logs: %TEMP%\dd_*.log)' -f $vs.installationPath, $VsSetup)
        }
    }

    $toolsetAfter = Get-MsvcToolsetVersion $vs.installationPath
    if ($toolsetBefore -and $toolsetAfter -and $toolsetBefore -ne $toolsetAfter) {
        Write-Log ('    MSVC changed from {0} to {1}. Reconfigure existing build folders once with: cmake --preset <name> --fresh' -f $toolsetBefore, $toolsetAfter) Yellow
    }
}
#endregion

#region Steps ------------------------------------------------------------------------------------------
function Enable-CrashDumps {
    # Windows Error Reporting then keeps a dump whenever Phazite.exe crashes, whoever started it:
    # %LOCALAPPDATA%\CrashDumps\Phazite.exe.<pid>.dmp (open it with VS Code's "Open crash dump" entry).
    $current = Get-ItemProperty -Path $CrashDumpKey -ErrorAction SilentlyContinue
    if ($current -and $current.DumpType -eq 2 -and $current.DumpCount -eq 3) { Write-Log '==> crash dumps for Phazite.exe: already on' Green; return }
    Write-Log '==> crash dumps for Phazite.exe: turning on (full dumps, keep the last 3)' Cyan
    if (-not (Test-Path -LiteralPath $CrashDumpKey)) { New-Item -Path $CrashDumpKey -Force | Out-Null }
    New-ItemProperty -Path $CrashDumpKey -Name DumpType -Value 2 -PropertyType DWord -Force | Out-Null
    New-ItemProperty -Path $CrashDumpKey -Name DumpCount -Value 3 -PropertyType DWord -Force | Out-Null
}

function Invoke-AdminSteps {
    Invoke-Step 'Git'            { Install-WingetPackage -Id 'Git.Git' -Extra @('--scope', 'machine') }
    Invoke-Step 'CMake'          { Install-WingetPackage -Id 'Kitware.CMake' -Extra @('--installer-type', 'wix') }  # the MSI, never the zip
    Invoke-Step 'Ninja'          { Install-WingetPackage -Id 'Ninja-build.Ninja' -Extra @('--scope', 'machine') }
    Invoke-Step 'VS Build Tools' { Install-BuildTools }
    Invoke-Step 'crash dumps'    { Enable-CrashDumps }
    if ($WithLlvm)       { Invoke-Step 'LLVM'     { Install-WingetPackage -Id 'LLVM.LLVM' } }            # no --scope: its manifest has none
    if ($WithTestPlugin) { Invoke-Step 'Surge XT' { Install-WingetPackage -Id 'SurgeSynth.SurgeXT' } }   # VST3 into Common Files\VST3
}

function Find-VsCodeCli {
    $cmd = Get-Command code.cmd -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    foreach ($candidate in @((Join-Path $env:LOCALAPPDATA 'Programs\Microsoft VS Code\bin\code.cmd'),
                             (Join-Path $env:ProgramFiles 'Microsoft VS Code\bin\code.cmd'))) {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

function Set-GitSettings {
    # Per user (runs without elevation, so it lands in this user's ~/.gitconfig).
    $git = Get-Command git.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $git) { throw 'git was not found, so the git settings could not be applied.' }
    Write-Log '==> git: long paths on; fetch JUCE over HTTPS (Tracktion lists it with an SSH URL)' Cyan
    foreach ($setting in @(@('core.longpaths', 'true'),
                           @('url.https://github.com/juce-framework/.insteadOf', 'git@github.com:juce-framework/'))) {
        $rc = Invoke-Native $git.Source (@('config', '--global', '--replace-all') + $setting)
        if ($rc -ne 0) { throw ('git config --global {0} failed (exit code {1})' -f $setting[0], $rc) }
    }
}

function Install-VsCodeExtensions {
    if ($SkipVSCodeExtensions) { return }
    $code = Find-VsCodeCli
    if (-not $code) { Write-Log '    VS Code not found; skipped the extensions' Yellow; return }
    foreach ($ext in $VsCodeExtensions) {
        Write-Log ('==> VS Code extension {0}' -f $ext) Cyan
        $extArgs = @('--install-extension', $ext)
        if (-not $SkipUpdates) { $extArgs += '--force' }   # --force also updates an installed extension
        $rc = Invoke-Native $code $extArgs
        if ($rc -ne 0) { throw ('VS Code could not install {0} (exit code {1})' -f $ext, $rc) }
    }
}

function Invoke-UserSteps {
    Invoke-Step 'git settings'       { Set-GitSettings }
    Invoke-Step 'VS Code extensions' { Install-VsCodeExtensions }
}

function Test-OnInheritedPath([string]$Dir) {
    $d = $Dir.TrimEnd('\')
    return (@($script:InheritedPath -split ';' | Where-Object { $_.Trim().TrimEnd('\') -ieq $d }).Count -gt 0)
}

function Get-ToolLine([string]$Name) {
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($null -eq $cmd) { $script:Missing = $true; return 'MISSING' }
    $ErrorActionPreference = 'Continue'
    $first = & $cmd.Source --version 2>&1 | Select-Object -First 1
    $note = ''
    if (-not (Test-OnInheritedPath (Split-Path -Parent $cmd.Source))) {
        $script:StalePath = $true
        $note = '   <- not yet on the PATH of the program that started this script'
    }
    return ('{0}  [{1}]{2}' -f ("$first").Trim(), $cmd.Source, $note)
}

function Write-Report {
    $ErrorActionPreference = 'Continue'
    Write-Log ''
    Write-Log '=== Phazite build tools ===' Green
    Write-Log ('winget  ' + (Get-ToolLine 'winget.exe'))
    Write-Log ('git     ' + (Get-ToolLine 'git.exe'))
    Write-Log ('cmake   ' + (Get-ToolLine 'cmake.exe'))
    Write-Log ('ninja   ' + (Get-ToolLine 'ninja.exe'))
    $clang = Join-Path $env:ProgramFiles 'LLVM\bin\clang.exe'
    if (Test-Path -LiteralPath $clang) { Write-Log ('clang   ' + (& $clang --version | Select-Object -First 1) + "  [$clang]") }

    $vs = Get-BuildToolsInstance
    if ($null -eq $vs) { Write-Log 'VS      MISSING' Red; $script:Missing = $true }
    else {
        $color = 'Gray'
        if ($vs.isRebootRequired) { $color = 'Yellow'; $script:RebootRequired = $true }   # waiting for a reboot
        elseif (-not $vs.isComplete) { $color = 'Red'; $script:Missing = $true }          # broken; run setup to repair
        Write-Log ('VS      {0} {1} ({2})  complete={3} rebootRequired={4}  [{5}]' -f $vs.displayName,
            $vs.catalog.productDisplayVersion, $vs.installationVersion, $vs.isComplete, $vs.isRebootRequired, $vs.installationPath) $color
        $msvc = Get-MsvcToolsetVersion $vs.installationPath
        if ($msvc) { Write-Log ('MSVC    ' + $msvc) } elseif (-not $vs.isRebootRequired) { Write-Log 'MSVC    MISSING' Red; $script:Missing = $true }
    }

    # Windows SDKs, found the way CMake finds them: KitsRoot10 plus Include\<version>\um\windows.h.
    $sdks = @()
    foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots', 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots')) {
        $roots = Get-ItemProperty -Path $key -ErrorAction SilentlyContinue
        if ($roots -and $roots.KitsRoot10) {
            $sdks = @(Get-ChildItem -LiteralPath (Join-Path $roots.KitsRoot10 'Include') -Directory -ErrorAction SilentlyContinue |
                      Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\windows.h') } | ForEach-Object { $_.Name })
            break
        }
    }
    if ($sdks.Count -gt 0) { Write-Log ('WinSDK  ' + ($sdks -join ', ')) } else { Write-Log 'WinSDK  MISSING' Red; $script:Missing = $true }

    $git = Get-Command git.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($git) {
        $longPaths = & $git.Source config --global --get core.longpaths 2>$null
        $jucePaths = & $git.Source config --global --get 'url.https://github.com/juce-framework/.insteadOf' 2>$null
        Write-Log ('gitcfg  core.longpaths={0}  JUCE over HTTPS={1}' -f $longPaths, [bool]$jucePaths)
        if ($longPaths -ne 'true' -or -not $jucePaths) { $script:Missing = $true }
    }

    $cli = Find-VsCodeCli
    if ($cli -and -not $SkipVSCodeExtensions) {
        $installedExtensions = @(& $cli --list-extensions --show-versions 2>$null)
        foreach ($ext in $VsCodeExtensions) {
            $hit = @($installedExtensions | Where-Object { $_ -like "$ext@*" }) | Select-Object -First 1
            if ($hit) { Write-Log ('vscode  ' + $hit) } else { Write-Log ('vscode  {0} MISSING' -f $ext) Red; $script:Missing = $true }
        }
    }

    $dumps = Get-ItemProperty -Path $CrashDumpKey -ErrorAction SilentlyContinue
    Write-Log ('dumps   crash dumps for Phazite.exe: {0}' -f $(if ($dumps) { 'on' } else { 'off' }))
    $surge = Join-Path $env:CommonProgramW6432 'VST3\Surge Synth Team\Surge XT.vst3'
    if (Test-Path -LiteralPath $surge) {
        $surgeVersion = ''
        try { $surgeVersion = (Get-Item -LiteralPath (Join-Path $surge 'Contents\x86_64-win\Surge XT.vst3')).VersionInfo.ProductVersion } catch { }
        Write-Log ('SurgeXT {0}  [{1}]' -f $surgeVersion, $surge)
    }
}

function Write-NextSteps {
    if (-not $script:StalePath) { return }
    Write-Log ''
    Write-Log 'Programs that were already running (this window too) still have the old PATH. Next:' Yellow
    Write-Log '  1. In VS Code: File > Exit. This closes every window; the Claude Code conversation is saved.' Yellow
    Write-Log '  2. In a terminal started from the Start menu, "Get-Process Code -ErrorAction SilentlyContinue" must print nothing.' Yellow
    Write-Log '  3. Start VS Code from the Start menu, close any terminals it restores, and reopen the conversation.' Yellow
    Write-Log '  4. For terminal builds, use a PowerShell window opened after this script finished.' Yellow
    Write-Log '  5. If "Get-Command git, cmake, ninja" still fails in a new window, sign out of Windows and back in.' Yellow
}
#endregion

#region Main -------------------------------------------------------------------------------------------
try { [Console]::OutputEncoding = [Text.Encoding]::UTF8 } catch { }   # winget and vswhere print UTF-8

$me = [Security.Principal.WindowsIdentity]::GetCurrent()
if ($me.IsSystem) { Write-Log 'Run this script as a normal user, not as SYSTEM (winget does not support SYSTEM).' Red; Exit-Script 2 }
if (-not [Environment]::Is64BitProcess) { Write-Log 'Run this script from 64-bit Windows PowerShell.' Red; Exit-Script 2 }

if ($CheckOnly) {
    Update-SessionPath
    Write-Report
    Write-NextSteps
    $checkCode = 0
    if ($script:RebootRequired) { $checkCode = 3010; Write-Log 'REBOOT REQUIRED: Visual Studio is waiting for a reboot. Restart Windows, then run tools\setup-windows.cmd again.' Yellow }
    if ($script:Missing -or $script:StalePath) { $checkCode = 1 }
    Exit-Script $checkCode
}

$script:Winget = $null
$wingetCmd = Get-Command winget.exe -ErrorAction SilentlyContinue
$wingetAlias = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
if ($wingetCmd) { $script:Winget = $wingetCmd.Source } elseif (Test-Path -LiteralPath $wingetAlias) { $script:Winget = $wingetAlias }
if (-not $script:Winget) { Write-Log 'winget was not found: install or update "App Installer" from the Microsoft Store.' Red; Exit-Script 2 }

if (-not (Test-IsAdmin)) {
    if ($AdminStepsOnly) {
        # Never elevate again from the elevated copy (that would start an endless chain of windows).
        Write-Log 'The elevated copy did not get administrator rights (is UAC turned off for a standard user?). Run the script from an administrator PowerShell instead.' Red
        exit 2
    }
    if (Test-PendingReboot) { Write-Log 'Note: Windows has a reboot pending. If an installer fails or asks for a reboot, restart Windows and run this again.' Yellow }

    # Run the admin part in an elevated copy of this script (one UAC prompt), then the per-user part here.
    $log = Join-Path $env:TEMP ('phazite-setup-{0:yyyyMMdd-HHmmss}.log' -f (Get-Date))
    $argLine = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -AdminStepsOnly -LogPath "{1}"' -f $PSCommandPath, $log
    foreach ($name in @('SkipUpdates', 'WithLlvm', 'WithTestPlugin')) {
        if ($PSBoundParameters.ContainsKey($name) -and $PSBoundParameters[$name]) { $argLine += ' -' + $name }
    }
    Write-Log 'Asking for administrator rights once (UAC). The installers then run in a separate window.'
    try { $child = Start-Process -FilePath $WindowsPowerShell -Verb RunAs -ArgumentList $argLine -Wait -PassThru }
    catch { Write-Log ('Could not start the elevated copy: ' + $_.Exception.Message) Red; Exit-Script 2 }
    $childCode = $child.ExitCode

    if (Test-Path -LiteralPath $log) { Get-Content -LiteralPath $log -Encoding UTF8 | ForEach-Object { Write-Host $_ } }
    else { Write-Log ('The elevated window closed without writing a log (exit code {0}). To see why, run tools\setup-windows.cmd from a PowerShell opened with "Run as administrator".' -f $childCode) Red }

    $script:LogPath = $log   # the per-user part and the report go into the same log
    Update-SessionPath
    Invoke-UserSteps
    Write-Report
    Write-NextSteps

    $exitCode = 0
    if ($childCode -eq 3010 -or $script:RebootRequired) { $exitCode = 3010 }
    if ($childCode -eq 2) {
        Write-Log 'The elevated part could not start; see its message above.' Red
        $exitCode = 2
    }
    elseif (($childCode -ne 0 -and $childCode -ne 3010) -or $script:Failed.Count -gt 0) {
        Write-Log ('Setup did not finish (elevated part exit code {0}). See the FAILED lines and the log above, fix the cause, then run it again.' -f $childCode) Red
        $exitCode = 1
    }
    elseif ($exitCode -eq 3010) { Write-Log 'REBOOT REQUIRED to finish: restart Windows, then run this script again (it is safe to repeat).' Yellow }
    Write-Log ('Full log: ' + $log)
    Exit-Script $exitCode
}

# Elevated: either the copy started above (-AdminStepsOnly) or a user who opened an admin PowerShell.
if (-not $AdminStepsOnly -and (Test-PendingReboot)) { Write-Log 'Note: Windows has a reboot pending. If an installer fails or asks for a reboot, restart Windows and run this again.' Yellow }
Invoke-AdminSteps
if (-not $AdminStepsOnly) {
    Update-SessionPath
    Invoke-UserSteps
    Write-Report
    Write-NextSteps
}
$exitCode = 0
if ($script:RebootRequired) { $exitCode = 3010 }
if ($script:Failed.Count -gt 0) { $exitCode = 1 }
if (-not $AdminStepsOnly) {
    if ($exitCode -eq 1) { Write-Log 'Setup did not finish. See the FAILED lines above, fix the cause, then run it again.' Red }
    elseif ($exitCode -eq 3010) { Write-Log 'REBOOT REQUIRED to finish: restart Windows, then run this script again (it is safe to repeat).' Yellow }
}
Exit-Script $exitCode
#endregion
