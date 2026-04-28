param(
    [string]$DllPath = "C:\Windows\System32\rime_apo.dll"
)

# Requires Run As Administrator
if (!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Warning "Please run this script as Administrator."
    exit
}

$ApoClsid = "{1A348F28-0904-4F9A-A5AB-5BAF6B5C58C3}"
$ClsidPath = "HKLM:\SOFTWARE\Classes\CLSID\$ApoClsid"

if (-not (Test-Path $DllPath)) {
    Write-Error "Could not find APO DLL at $DllPath. Please build the project first."
    exit
}

Write-Host "Registering Rime APO COM Server..."

# Register CLSID
if (-not (Test-Path $ClsidPath)) {
    New-Item -Path $ClsidPath -Force | Out-Null
}
Set-ItemProperty -Path $ClsidPath -Name "(default)" -Value "Rime Audio Processing Object"

# Register InProcServer32
$InProcPath = "$ClsidPath\InProcServer32"
if (-not (Test-Path $InProcPath)) {
    New-Item -Path $InProcPath -Force | Out-Null
}
Set-ItemProperty -Path $InProcPath -Name "(default)" -Value $DllPath
Set-ItemProperty -Path $InProcPath -Name "ThreadingModel" -Value "Both"

# Register AudioProcessingObject
$ApoKeyPath1 = "HKLM:\SOFTWARE\Classes\AudioProcessingObject\$ApoClsid"
$ApoKeyPath2 = "HKLM:\SOFTWARE\Classes\AudioEngine\AudioProcessingObjects\$ApoClsid"

foreach ($ApoKeyPath in @($ApoKeyPath1, $ApoKeyPath2)) {
    if (-not (Test-Path $ApoKeyPath)) {
        New-Item -Path $ApoKeyPath -Force | Out-Null
    }
    Set-ItemProperty -Path $ApoKeyPath -Name "Name" -Value "Rime APO"
    Set-ItemProperty -Path $ApoKeyPath -Name "FriendlyName" -Value "Rime APO"
    Set-ItemProperty -Path $ApoKeyPath -Name "Copyright" -Value "Neumann"
    Set-ItemProperty -Path $ApoKeyPath -Name "MajorVersion" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "MinorVersion" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "Flags" -Value 0x0000000f # APO_FLAG_INPLACE | APO_FLAG_SAMPLESPERFRAME_MUST_MATCH | APO_FLAG_FRAMESPERSECOND_MUST_MATCH | APO_FLAG_BITSPERSAMPLE_MUST_MATCH
    Set-ItemProperty -Path $ApoKeyPath -Name "MinInputConnections" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "MaxInputConnections" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "MinOutputConnections" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "MaxOutputConnections" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "MaxInstances" -Value 0xffffffff
    Set-ItemProperty -Path $ApoKeyPath -Name "NumAPOInterfaces" -Value 1
    Set-ItemProperty -Path $ApoKeyPath -Name "APOInterface0" -Value "{5CA5EA24-13B5-4D6D-A263-D13CE52D5392}" # IAudioSystemEffects
}



Write-Host "COM Registration Complete!"
Write-Host "Note: To attach this to an audio device, you must add this CLSID to the target device's FXProperties registry key."
