#Requires -Version 5.1
<#
.SYNOPSIS
    Installs Phazite with its installer for this user only, silently, into a folder of its own, checks what it installed,
    then uninstalls it and checks that all of it went. CI runs it after building the installer (.github/workflows/ci.yml).

.DESCRIPTION
    Needs no administrator rights: the installer runs with /CURRENTUSER, so what it registers goes into this user's part of
    the registry (HKCU), and its shortcut into this user's Start menu. It checks:
      - the files: Phazite.exe, PhaziteHost.exe, the fonts, the presets, the licenses and NOTICES.txt;
      - that a project (.phazite) opens in Phazite, and that DAWproject files (.dawproject) offer it in Open With;
      - the Start menu's shortcut and the uninstaller's entry, with the installer's version;
    then, after the uninstaller: that none of them is left. It stops before installing if Phazite is open, or if this user's
    registry already has the associations or the uninstall entry (a Phazite installed here), which the test would remove.

    Exit codes (the last line printed, "Result: ...", says the same): 0 = everything was as it should be; 1 = something
    wasn't (the FAILED lines say what); 2 = it could not start.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-installer.ps1 build\windows-release\installer\Phazite-0.1.0-Setup.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Installer
)

$ErrorActionPreference = 'Stop'
$script:Failures = 0

function Write-Check([bool]$Ok, [string]$What) {
    if ($Ok) { Write-Host ('ok      ' + $What) }
    else { Write-Host ('FAILED  ' + $What) -ForegroundColor Red; $script:Failures++ }
}

function Exit-Test([int]$Code, [string]$Why) {
    Write-Host ('Result: {0} ({1})' -f $Code, $Why)
    exit $Code
}

function Get-Default([string]$Key) {
    $item = Get-Item -LiteralPath $Key -ErrorAction SilentlyContinue
    if ($null -eq $item) { return $null }
    return $item.GetValue('')
}

$classes = 'HKCU:\Software\Classes'
$uninstallKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\io.github.august-byrne.phazite_is1'
$shortcut = Join-Path $env:APPDATA 'Microsoft\Windows\Start Menu\Programs\Phazite.lnk'

if (-not (Test-Path -LiteralPath $Installer)) { Exit-Test 2 "there is no installer at $Installer" }
$Installer = (Resolve-Path -LiteralPath $Installer).Path
$version = if ((Split-Path -Leaf $Installer) -match '^Phazite-(.+)-Setup\.exe$') { $Matches[1] } else { '' }

# Phazite holds this mutex while it runs (src/app/Main.cpp); its installer would wait for it.
$open = $false
foreach ($name in @('PhaziteRunning', 'Global\PhaziteRunning')) {
    $mutex = $null
    if ([System.Threading.Mutex]::TryOpenExisting($name, [ref]$mutex)) { $open = $true; $mutex.Dispose() }
}
if ($open) { Exit-Test 2 'Phazite is open: close it first' }

foreach ($key in @("$classes\.phazite", "$classes\Phazite.Project", "$classes\Phazite.DAWproject", $uninstallKey)) {
    if (Test-Path -LiteralPath $key) { Exit-Test 2 "$key is there already: a Phazite is installed for this user, which this test would remove" }
}

# Another DAW may have registered DAWproject files already: then their key stays, and only Phazite's part of it goes.
$hadDawProject = Test-Path -LiteralPath "$classes\.dawproject"

$folder = Join-Path ([System.IO.Path]::GetTempPath()) ('Phazite installer test ' + [guid]::NewGuid().ToString('N').Substring(0, 8))
$app = Join-Path $folder 'Phazite'
New-Item -ItemType Directory -Force -Path $folder | Out-Null
Write-Host "Installing $Installer into $app"

# Silently, for this user, with the tasks as they start (no desktop icon), and Inno Setup's log.
$arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', ('/DIR="{0}"' -f $app),
               ('/LOG="{0}"' -f (Join-Path $folder 'install.log')))
$install = Start-Process -FilePath $Installer -ArgumentList $arguments -Wait -PassThru
Write-Check ($install.ExitCode -eq 0) ('the installer ended with exit code {0}' -f $install.ExitCode)

if ($install.ExitCode -eq 0) {
    # The files.
    foreach ($file in @('Phazite.exe', 'PhaziteHost.exe', 'NOTICES.txt', 'licenses\Phazite.txt', 'licenses\JUCE.txt',
                        'licenses\RubberBand.txt', 'licenses\Inter.txt', 'licenses\ASIO-SDK.txt', 'fonts\Inter\OFL.txt',
                        'fonts\Inter\Inter-Regular.ttf', 'unins000.exe')) {
        Write-Check (Test-Path -LiteralPath (Join-Path $app $file) -PathType Leaf) "installed $file"
    }
    $presets = @(Get-ChildItem -LiteralPath (Join-Path $app 'presets') -Recurse -File -ErrorAction SilentlyContinue)
    Write-Check ($presets.Count -gt 0) ('installed {0} presets' -f $presets.Count)

    # The file associations.
    $exe = Join-Path $app 'Phazite.exe'
    Write-Check ((Get-Default "$classes\.phazite") -eq 'Phazite.Project') '.phazite is a Phazite Project'
    Write-Check ((Get-Default "$classes\Phazite.Project\shell\open\command") -eq ('"{0}" "%1"' -f $exe)) 'a project opens in the installed Phazite'
    Write-Check ((Get-Default "$classes\Phazite.Project\DefaultIcon") -eq ('{0},0' -f $exe)) "a project has Phazite's icon"
    $openWith = Get-ItemProperty -LiteralPath "$classes\.dawproject\OpenWithProgids" -ErrorAction SilentlyContinue
    Write-Check ($null -ne $openWith -and $null -ne $openWith.'Phazite.DAWproject') 'a DAWproject offers Phazite in Open With'
    Write-Check ((Get-Default "$classes\Phazite.DAWproject\shell\open\command") -eq ('"{0}" "%1"' -f $exe)) 'a DAWproject imports into the installed Phazite'

    # The shortcut and the uninstaller's entry.
    Write-Check (Test-Path -LiteralPath $shortcut) "the Start menu's shortcut"
    $entry = Get-ItemProperty -LiteralPath $uninstallKey -ErrorAction SilentlyContinue
    Write-Check ($null -ne $entry -and $entry.DisplayName -eq 'Phazite') "the uninstaller's entry, Phazite"
    if ($version) { Write-Check ($null -ne $entry -and $entry.DisplayVersion -eq $version) ('its version, {0}' -f $version) }

    # Then the uninstaller, which runs from a copy of itself in the temporary folder: wait for what it removes to go.
    $uninstaller = Join-Path $app 'unins000.exe'
    if (Test-Path -LiteralPath $uninstaller) {
        Write-Host 'Uninstalling'
        $uninstall = Start-Process -FilePath $uninstaller -ArgumentList @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART') -Wait -PassThru
        Write-Check ($uninstall.ExitCode -eq 0) ('the uninstaller ended with exit code {0}' -f $uninstall.ExitCode)
        $deadline = (Get-Date).AddSeconds(60)
        while ((Get-Date) -lt $deadline -and ((Test-Path -LiteralPath $exe) -or (Test-Path -LiteralPath $uninstallKey))) {
            Start-Sleep -Milliseconds 250
        }
    }
}

# Nothing of it left, whatever happened above.
Write-Check (-not (Test-Path -LiteralPath (Join-Path $app 'Phazite.exe'))) 'the app went'
Write-Check (-not (Test-Path -LiteralPath (Join-Path $app 'licenses'))) 'the licenses went'
foreach ($key in @("$classes\.phazite", "$classes\Phazite.Project", "$classes\Phazite.DAWproject", "$classes\Applications\Phazite.exe", $uninstallKey)) {
    Write-Check (-not (Test-Path -LiteralPath $key)) "$key went"
}
$leftOpenWith = Get-ItemProperty -LiteralPath "$classes\.dawproject\OpenWithProgids" -ErrorAction SilentlyContinue
Write-Check ($null -eq $leftOpenWith -or $null -eq $leftOpenWith.'Phazite.DAWproject') "DAWproject's Open With lost Phazite"
if (-not $hadDawProject) { Write-Check (-not (Test-Path -LiteralPath "$classes\.dawproject")) "$classes\.dawproject, which the installer made, went" }
Write-Check (-not (Test-Path -LiteralPath $shortcut)) "the Start menu's shortcut went"

if ($script:Failures -gt 0) { Exit-Test 1 ('{0} checks failed; the install log is in {1}' -f $script:Failures, $folder) }
Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
Exit-Test 0 'installed, checked and uninstalled'
