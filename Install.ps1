#Requires -RunAsAdministrator
<#
.SYNOPSIS
    RIME Standalone Installer - Installs the APO, bridge, and startup task.
.DESCRIPTION
    This script performs a complete installation of the RIME Standalone audio bridge:
    1. Copies rime_apo.dll to System32 and rime_bridge.exe to Program Files
    2. Registers the APO COM server in the registry
    3. Lets you pick which audio device to attach the APO to
    4. Creates a default config file
    5. Registers a startup task so the bridge launches automatically
.PARAMETER Uninstall
    Removes all RIME components from the system.
#>
param(
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"

# --- Constants ---
$ApoClsid       = "{1A348F28-0904-4F9A-A5AB-5BAF6B5C58C3}"
$InstallDir      = "$env:ProgramFiles\RIME Standalone"
$ConfigDir       = "$env:APPDATA\RIME Standalone"
$ConfigFile      = "$ConfigDir\config.json"
$DllSource       = Join-Path $PSScriptRoot "build\Release\rime_apo.dll"
$BridgeSource    = Join-Path $PSScriptRoot "build\Release\rime_bridge.exe"
$DllDest         = "C:\Windows\System32\rime_apo.dll"
$BridgeDest      = "$InstallDir\rime_bridge.exe"
$TaskName        = "RIME Standalone Bridge"
$StartupRegPath  = "HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Run"

# --- TrustedInstaller Bypass (needed for FxProperties) ---
$tokenDef = @"
using System;
using System.Runtime.InteropServices;
public static class TokenManipulator {
    [DllImport("advapi32.dll", ExactSpelling = true, SetLastError = true)]
    internal static extern bool AdjustTokenPrivileges(IntPtr htok, bool disall, ref TokPriv1Luid newst, int len, IntPtr prev, IntPtr relen);
    [DllImport("advapi32.dll", ExactSpelling = true, SetLastError = true)]
    internal static extern bool OpenProcessToken(IntPtr h, int acc, ref IntPtr phtok);
    [DllImport("advapi32.dll", SetLastError = true)]
    internal static extern bool LookupPrivilegeValue(string host, string name, ref long pluid);
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    internal struct TokPriv1Luid { public int Count; public long Luid; public int Attr; }
    public static bool AddPrivilege(string privilege) {
        TokPriv1Luid tp;
        IntPtr hproc = System.Diagnostics.Process.GetCurrentProcess().Handle;
        IntPtr htok = IntPtr.Zero;
        OpenProcessToken(hproc, 0x00000020 | 0x00000008, ref htok);
        tp.Count = 1; tp.Luid = 0; tp.Attr = 0x00000002;
        LookupPrivilegeValue(null, privilege, ref tp.Luid);
        return AdjustTokenPrivileges(htok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
    }
}
"@
try { Add-Type -TypeDefinition $tokenDef } catch {}
[TokenManipulator]::AddPrivilege("SeTakeOwnershipPrivilege") | Out-Null
[TokenManipulator]::AddPrivilege("SeRestorePrivilege") | Out-Null

function Unlock-RegistryKey {
    param([string]$DeviceId)
    $subKeyPath = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$DeviceId\FxProperties"
    
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subKeyPath, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [System.Security.AccessControl.RegistryRights]::TakeOwnership)
    if ($key) {
        $acl = $key.GetAccessControl()
        $acl.SetOwner([System.Security.Principal.NTAccount]"Administrators")
        $key.SetAccessControl($acl)
        $key.Close()
    }

    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subKeyPath, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [System.Security.AccessControl.RegistryRights]::ChangePermissions)
    if ($key) {
        $acl = $key.GetAccessControl()
        $rule = New-Object System.Security.AccessControl.RegistryAccessRule("Administrators", "FullControl", "Allow")
        $acl.SetAccessRule($rule)
        $key.SetAccessControl($acl)
        $key.Close()
    }
}

function Get-AudioDevices {
    $devices = @()
    $renderPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
    if (Test-Path $renderPath) {
        foreach ($key in Get-ChildItem -Path $renderPath) {
            $propPath = "$($key.PSPath)\Properties"
            if (Test-Path $propPath) {
                $prop = Get-ItemProperty -Path $propPath -Name "{a45c254e-df1c-4efd-8020-67d146a850e0},2" -ErrorAction SilentlyContinue
                if ($prop) {
                    $name = $prop."{a45c254e-df1c-4efd-8020-67d146a850e0},2"
                    $fxPath = "$($key.PSPath)\FxProperties"
                    $installed = $false
                    if (Test-Path $fxPath) {
                        $fxProp = Get-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -ErrorAction SilentlyContinue
                        if ($fxProp -and $fxProp."{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -eq $ApoClsid) {
                            $installed = $true
                        }
                    }
                    $devices += [PSCustomObject]@{
                        Id        = $key.PSChildName
                        Name      = $name
                        FxPath    = $fxPath
                        Installed = $installed
                    }
                }
            }
        }
    }
    return $devices
}

# ============================================================
#  UNINSTALL
# ============================================================
if ($Uninstall) {
    Write-Host ""
    Write-Host "=== RIME Standalone Uninstaller ===" -ForegroundColor Cyan
    Write-Host ""

    # 1. Stop bridge
    Write-Host "[1/6] Stopping bridge process..." -ForegroundColor Yellow
    Stop-Process -Name rime_bridge -Force -ErrorAction SilentlyContinue

    # 2. Remove startup entry
    Write-Host "[2/6] Removing startup entry..." -ForegroundColor Yellow
    Remove-ItemProperty -Path $StartupRegPath -Name $TaskName -ErrorAction SilentlyContinue

    # 3. Remove APO from all devices
    Write-Host "[3/6] Removing APO from audio devices..." -ForegroundColor Yellow
    $devices = Get-AudioDevices
    foreach ($dev in $devices | Where-Object { $_.Installed }) {
        try {
            Unlock-RegistryKey -DeviceId $dev.Id
            
            # Check if we have a backup of the original APO
            $backupProp = Get-ItemProperty -Path $dev.FxPath -Name "RimeBackup_EndpointEffectClsid" -ErrorAction SilentlyContinue
            if ($backupProp) {
                # Restore the original APO
                $originalVal = $backupProp.RimeBackup_EndpointEffectClsid
                Set-ItemProperty -Path $dev.FxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -Value $originalVal -Type String -ErrorAction Stop
                Remove-ItemProperty -Path $dev.FxPath -Name "RimeBackup_EndpointEffectClsid" -ErrorAction SilentlyContinue
            } else {
                # If no backup, just remove our APO to return to pure passthrough
                Remove-ItemProperty -Path $dev.FxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -ErrorAction Stop
            }
            Write-Host "  Removed from: $($dev.Name)" -ForegroundColor Gray
        } catch {
            Write-Warning "  Could not remove from: $($dev.Name)"
        }
    }

    # 4. Unregister COM
    Write-Host "[4/6] Removing COM registration..." -ForegroundColor Yellow
    $ClsidPath = "HKLM:\SOFTWARE\Classes\CLSID\$ApoClsid"
    $ApoKeyPath1 = "HKLM:\SOFTWARE\Classes\AudioProcessingObject\$ApoClsid"
    $ApoKeyPath2 = "HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$ApoClsid"
    foreach ($p in @($ClsidPath, $ApoKeyPath1, $ApoKeyPath2)) {
        if (Test-Path $p) { Remove-Item -Path $p -Recurse -Force }
    }

    # 5. Remove files
    Write-Host "[5/6] Removing files..." -ForegroundColor Yellow
    Stop-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue
    Remove-Item -Path $DllDest -Force -ErrorAction SilentlyContinue
    Remove-Item -Path $InstallDir -Recurse -Force -ErrorAction SilentlyContinue
    Start-Service -Name Audiosrv -ErrorAction SilentlyContinue

    # 6. Restore APO signing enforcement
    Write-Host "[6/7] Restoring APO signing enforcement..." -ForegroundColor Yellow
    $audioRegPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio"
    Set-ItemProperty -Path $audioRegPath -Name "DisableProtectedAudioDG" -Value 0 -Type DWord -ErrorAction SilentlyContinue
    Write-Host "  Set DisableProtectedAudioDG=0"

    # 7. Restart audio
    Write-Host "[7/7] Restarting audio service..." -ForegroundColor Yellow
    Restart-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue

    Write-Host ""
    Write-Host "RIME Standalone has been uninstalled." -ForegroundColor Green
    Write-Host "Config files preserved at: $ConfigDir" -ForegroundColor Gray
    Write-Host "(Delete manually if you want a full cleanup)" -ForegroundColor Gray
    exit
}

# ============================================================
#  INSTALL
# ============================================================
Write-Host ""
Write-Host "=== RIME Standalone Installer ===" -ForegroundColor Cyan
Write-Host ""

# Pre-flight check
if (-not (Test-Path $DllSource)) {
    Write-Error "Could not find rime_apo.dll at $DllSource. Please build the project first (cmake --build build --config Release)."
    exit
}
if (-not (Test-Path $BridgeSource)) {
    Write-Error "Could not find rime_bridge.exe at $BridgeSource. Please build the project first."
    exit
}

# --- Step 1: Copy Files ---
Write-Host "[1/6] Installing files..." -ForegroundColor Yellow

Stop-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue
Stop-Process -Name rime_bridge -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1

if (-not (Test-Path $InstallDir)) { New-Item -Path $InstallDir -ItemType Directory -Force | Out-Null }
Copy-Item -Path $DllSource -Destination $DllDest -Force
Copy-Item -Path $BridgeSource -Destination $BridgeDest -Force
Write-Host "  rime_bridge.exe -> $BridgeDest"

# --- Step 1b: Optional Virtual Audio Cable Bundling ---
Write-Host "[1b/6] Checking for Virtual Audio Cable driver pack..." -ForegroundColor Yellow
$VBCableSetup = Join-Path $PSScriptRoot "VBCABLE\VBCABLE_Setup_x64.exe"
if (Test-Path $VBCableSetup) {
    Write-Host "  Found VB-Audio Virtual Cable installer. Silently installing..."
    Start-Process $VBCableSetup -ArgumentList "-i", "-h" -Wait -NoNewWindow
    Write-Host "  VB-Audio Virtual Cable installed."
} else {
    Write-Host "  VB-Audio Virtual Cable driver pack not found in VBCABLE folder. Skipping."
}

# --- Step 2: Disable APO Signing Enforcement ---
Write-Host "[2/6] Disabling APO signing enforcement..." -ForegroundColor Yellow

$audioRegPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio"
if (-not (Test-Path $audioRegPath)) { New-Item -Path $audioRegPath -Force | Out-Null }
$currentVal = (Get-ItemProperty -Path $audioRegPath -Name "DisableProtectedAudioDG" -ErrorAction SilentlyContinue).DisableProtectedAudioDG
if ($currentVal -eq 1) {
    Write-Host "  Already disabled (DisableProtectedAudioDG=1)"
} else {
    Set-ItemProperty -Path $audioRegPath -Name "DisableProtectedAudioDG" -Value 1 -Type DWord
    Write-Host "  Set DisableProtectedAudioDG=1 (unsigned APOs allowed)"
}

# --- Step 3: Register COM ---
Write-Host "[3/6] Registering APO COM server..." -ForegroundColor Yellow

$ClsidPath = "HKLM:\SOFTWARE\Classes\CLSID\$ApoClsid"
if (-not (Test-Path $ClsidPath)) { New-Item -Path $ClsidPath -Force | Out-Null }
Set-ItemProperty -Path $ClsidPath -Name "(default)" -Value "Rime Audio Processing Object"

$InProcPath = "$ClsidPath\InProcServer32"
if (-not (Test-Path $InProcPath)) { New-Item -Path $InProcPath -Force | Out-Null }
Set-ItemProperty -Path $InProcPath -Name "(default)" -Value $DllDest
Set-ItemProperty -Path $InProcPath -Name "ThreadingModel" -Value "Both"

$ApoKeyPath1 = "HKLM:\SOFTWARE\Classes\AudioProcessingObject\$ApoClsid"
$ApoKeyPath2 = "HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$ApoClsid"
foreach ($ApoKeyPath in @($ApoKeyPath1, $ApoKeyPath2)) {
    if (-not (Test-Path $ApoKeyPath)) { New-Item -Path $ApoKeyPath -Force | Out-Null }
    Set-ItemProperty -Path $ApoKeyPath -Name "Name" -Value "Rime APO"
    Set-ItemProperty -Path $ApoKeyPath -Name "FriendlyName" -Value "Rime APO"
    Set-ItemProperty -Path $ApoKeyPath -Name "Copyright" -Value "Neumann"
    Set-ItemProperty -Path $ApoKeyPath -Name "MajorVersion" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "MinorVersion" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "Flags" -Value 0x0000000f
    Set-ItemProperty -Path $ApoKeyPath -Name "MinInputConnections" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "MaxInputConnections" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "MinOutputConnections" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "MaxOutputConnections" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "MaxInstances" -Value 0xffffffff
    Set-ItemProperty -Path $ApoKeyPath -Name "NumAPOInterfaces" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "APOInterface0" -Value "{5CA5EA24-13B5-4D6D-A263-D13CE52D5392}"
}
Write-Host "  COM registration complete"

# --- Step 4: Select Audio Device ---
Write-Host "[4/6] Selecting audio device..." -ForegroundColor Yellow
Write-Host ""

$devices = Get-AudioDevices

if ($devices.Count -eq 0) {
    Write-Warning "No audio render devices found!"
    exit
}

for ($i = 0; $i -lt $devices.Count; $i++) {
    $marker = if ($devices[$i].Installed) { " [INSTALLED]" } else { "" }
    Write-Host "  [$i] $($devices[$i].Name)$marker"
}
Write-Host ""

$alreadyInstalled = $devices | Where-Object { $_.Installed }
if ($alreadyInstalled) {
    $choice = Read-Host "APO already installed on a device. Press Enter to keep current, or enter a number to change"
    if ([string]::IsNullOrWhiteSpace($choice)) {
        Write-Host "  Keeping current device configuration"
        $selectedDev = $null
    } else {
        $selectedDev = $devices[[int]$choice]
    }
} else {
    $choice = Read-Host "Select output device number"
    $selectedDev = $devices[[int]$choice]
}

if ($selectedDev) {
    # Remove from any other device first
    foreach ($dev in $devices | Where-Object { $_.Installed -and $_.Id -ne $selectedDev.Id }) {
        try {
            Unlock-RegistryKey -DeviceId $dev.Id
            Remove-ItemProperty -Path $dev.FxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -ErrorAction Stop
        } catch {}
    }

    # Install on selected device
    try {
        if (-not (Test-Path $selectedDev.FxPath)) {
            Unlock-RegistryKey -DeviceId $selectedDev.Id
            New-Item -Path $selectedDev.FxPath -Force -ErrorAction Stop | Out-Null
        }
        Unlock-RegistryKey -DeviceId $selectedDev.Id
        
        # Backup the original APO if it exists and isn't our own APO
        $existingProp = Get-ItemProperty -Path $selectedDev.FxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -ErrorAction SilentlyContinue
        if ($existingProp) {
            $existingVal = $existingProp.PSObject.Properties["{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5"].Value
            if ($existingVal -ne $ApoClsid) {
                Set-ItemProperty -Path $selectedDev.FxPath -Name "RimeBackup_EndpointEffectClsid" -Value $existingVal -Type String -ErrorAction Stop
            }
        }
        
        Set-ItemProperty -Path $selectedDev.FxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -Value $ApoClsid -Type String -ErrorAction Stop
        Write-Host "  APO installed on: $($selectedDev.Name)" -ForegroundColor Green
    } catch {
        Write-Error "Failed to install APO on device: $_"
    }
}

# --- Step 5: Create Config ---
Write-Host "[5/6] Creating configuration..." -ForegroundColor Yellow

if (-not (Test-Path $ConfigDir)) { New-Item -Path $ConfigDir -ItemType Directory -Force | Out-Null }

if (-not (Test-Path $ConfigFile)) {
    $defaultConfig = @{
        pluginPath   = "C:\Program Files\Common Files\VST3\Neumann RIME.vst3\Contents\x86_64-win\Neumann RIME.vst3"
        sampleRate   = 48000
        outputGainDb = 3.0
    } | ConvertTo-Json -Depth 2
    $defaultConfig | Set-Content -Path $ConfigFile -Encoding UTF8
    Write-Host "  Config created: $ConfigFile"
} else {
    Write-Host "  Config already exists: $ConfigFile (preserved)"
}

# --- Step 6: Register Startup ---
Write-Host "[6/6] Registering startup..." -ForegroundColor Yellow

Set-ItemProperty -Path $StartupRegPath -Name $TaskName -Value "`"$BridgeDest`"" -Type String
Write-Host "  Startup entry added: $TaskName"

# --- Restart Audio Service ---
Write-Host ""
Write-Host "Restarting Windows Audio Service..." -ForegroundColor Yellow
Start-Service -Name Audiosrv
Start-Sleep -Seconds 2

# --- Launch Bridge ---
Write-Host ""
$launch = Read-Host "Launch RIME bridge now? (Y/n)"
if ($launch -ne "n") {
    Start-Process -FilePath $BridgeDest
    Write-Host "Bridge launched!" -ForegroundColor Green
}

Write-Host ""
Write-Host "============================================" -ForegroundColor Cyan
Write-Host " Installation Complete!" -ForegroundColor Green
Write-Host " Config: $ConfigFile" -ForegroundColor Gray
Write-Host " Bridge: $BridgeDest" -ForegroundColor Gray
Write-Host " The bridge will start automatically on login." -ForegroundColor Gray
Write-Host "============================================" -ForegroundColor Cyan
Write-Host ""
