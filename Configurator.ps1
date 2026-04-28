param()

# Require Administrator
if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Warning "Please run this script as Administrator to modify device registry keys."
    exit
}

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

$ApoClsid = "{1A348F28-0904-4F9A-A5AB-5BAF6B5C58C3}"

# C# snippet to enable SeTakeOwnershipPrivilege so we can bypass TrustedInstaller
$definition = @"
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
    internal struct TokPriv1Luid {
        public int Count;
        public long Luid;
        public int Attr;
    }
    public static bool AddPrivilege(string privilege) {
        TokPriv1Luid tp;
        IntPtr hproc = System.Diagnostics.Process.GetCurrentProcess().Handle;
        IntPtr htok = IntPtr.Zero;
        OpenProcessToken(hproc, 0x00000020 | 0x00000008, ref htok);
        tp.Count = 1;
        tp.Luid = 0;
        tp.Attr = 0x00000002;
        LookupPrivilegeValue(null, privilege, ref tp.Luid);
        return AdjustTokenPrivileges(htok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero);
    }
}
"@
Add-Type -TypeDefinition $definition
[TokenManipulator]::AddPrivilege("SeTakeOwnershipPrivilege") | Out-Null
[TokenManipulator]::AddPrivilege("SeRestorePrivilege") | Out-Null

function Unlock-RegistryKey {
    param([string]$DeviceId)
    $subKeyPath = "SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render\$DeviceId\FxProperties"
    
    # 1. Open with TakeOwnership rights
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subKeyPath, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [System.Security.AccessControl.RegistryRights]::TakeOwnership)
    if ($key) {
        $acl = $key.GetAccessControl()
        $acl.SetOwner([System.Security.Principal.NTAccount]"Administrators")
        $key.SetAccessControl($acl)
        $key.Close()
    }

    # 2. Open with ChangePermissions rights
    $key = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subKeyPath, [Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree, [System.Security.AccessControl.RegistryRights]::ChangePermissions)
    if ($key) {
        $acl = $key.GetAccessControl()
        $rule = New-Object System.Security.AccessControl.RegistryAccessRule("Administrators", "FullControl", "Allow")
        $acl.SetAccessRule($rule)
        $key.SetAccessControl($acl)
        $key.Close()
    }
}

# Get Audio Devices
$devices = @()
$renderPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render"
if (Test-Path $renderPath) {
    $keys = Get-ChildItem -Path $renderPath
    foreach ($key in $keys) {
        $propPath = "$($key.PSPath)\Properties"
        if (Test-Path $propPath) {
            # PKEY_Device_FriendlyName
            $prop = Get-ItemProperty -Path $propPath -Name "{a45c254e-df1c-4efd-8020-67d146a850e0},2" -ErrorAction SilentlyContinue
            if ($prop) {
                $name = $prop."{a45c254e-df1c-4efd-8020-67d146a850e0},2"
                
                # Check if our APO is installed
                $fxPath = "$($key.PSPath)\FxProperties"
                $installed = $false
                if (Test-Path $fxPath) {
                    $fxProp = Get-ItemProperty -Path $fxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -ErrorAction SilentlyContinue
                    if ($fxProp -and $fxProp."{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -eq $ApoClsid) {
                        $installed = $true
                    }
                }

                $devices += [PSCustomObject]@{
                    Id = $key.PSChildName
                    Name = $name
                    FxPath = $fxPath
                    Installed = $installed
                    DisplayName = if ($installed) { "[INSTALLED] $name" } else { $name }
                }
            }
        }
    }
}

# Build GUI
$form = New-Object System.Windows.Forms.Form
$form.Text = "Rime APO Configurator"
$form.Size = New-Object System.Drawing.Size(400,300)
$form.StartPosition = "CenterScreen"

$label = New-Object System.Windows.Forms.Label
$label.Text = "Select an Output Device to install the Rime APO:"
$label.Location = New-Object System.Drawing.Point(10,10)
$label.Size = New-Object System.Drawing.Size(360,20)
$form.Controls.Add($label)

$listBox = New-Object System.Windows.Forms.ListBox
$listBox.Location = New-Object System.Drawing.Point(10,30)
$listBox.Size = New-Object System.Drawing.Size(360,180)
$listBox.DisplayMember = "DisplayName"
foreach ($d in $devices) {
    $listBox.Items.Add($d) | Out-Null
}
$form.Controls.Add($listBox)

$installBtn = New-Object System.Windows.Forms.Button
$installBtn.Text = "Install APO"
$installBtn.Location = New-Object System.Drawing.Point(10,220)
$installBtn.Size = New-Object System.Drawing.Size(100,30)
$form.Controls.Add($installBtn)

$removeBtn = New-Object System.Windows.Forms.Button
$removeBtn.Text = "Remove APO"
$removeBtn.Location = New-Object System.Drawing.Point(120,220)
$removeBtn.Size = New-Object System.Drawing.Size(100,30)
$form.Controls.Add($removeBtn)

# Button Handlers
$installBtn.Add_Click({
    if ($listBox.SelectedItem) {
        $dev = $listBox.SelectedItem
        try {
            if (-not (Test-Path $dev.FxPath)) {
                # If FxProperties doesn't exist, we must create it by unlocking the parent key first
                Unlock-RegistryKey -DeviceId $dev.Id
                New-Item -Path $dev.FxPath -Force -ErrorAction Stop | Out-Null
            }
            
            Unlock-RegistryKey -DeviceId $dev.Id

            # Set PKEY_FX_EndpointEffectClsid
            Set-ItemProperty -Path $dev.FxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -Value $ApoClsid -Type String -ErrorAction Stop
            
            # Restart audio service to apply
            $label.Text = "Restarting audio service..."
            $form.Refresh()
            Restart-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue
            
            $label.Text = "Installed and audio service restarted!"
            $installBtn.Enabled = $false
        } catch {
            $label.Text = "Error: Access Denied. (TrustedInstaller protection)"
        }
    }
})

$removeBtn.Add_Click({
    if ($listBox.SelectedItem) {
        $dev = $listBox.SelectedItem
        try {
            if (Test-Path $dev.FxPath) {
                Unlock-RegistryKey -DeviceId $dev.Id
                Remove-ItemProperty -Path $dev.FxPath -Name "{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},5" -ErrorAction Stop
                
                # Restart audio service to apply
                $label.Text = "Restarting audio service..."
                $form.Refresh()
                Restart-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue
                
                $label.Text = "Removed and audio service restarted!"
                $removeBtn.Enabled = $false
            }
        } catch {
            $label.Text = "Error: Access Denied. (TrustedInstaller protection)"
        }
    }
})

$form.ShowDialog() | Out-Null
