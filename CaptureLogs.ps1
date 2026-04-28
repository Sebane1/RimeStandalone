$pipeName = "\\.\pipe\RimeApoLog"
$logFile = "C:\Users\stel9\Desktop\apo_debug.log"

Write-Host "Listening to $pipeName..."
$stream = New-Object System.IO.Pipelines.Pipe
# Actually, NamedPipeServerStream is better
$pipeServer = New-Object System.IO.Pipes.NamedPipeServerStream("RimeApoLog", [System.IO.Pipes.PipeDirection]::InOut, 1, [System.IO.Pipes.PipeTransmissionMode]::Byte, [System.IO.Pipes.PipeOptions]::Asynchronous)

Write-Host "Waiting for connection..."
$pipeServer.WaitForConnection()
Write-Host "Connected!"

$reader = New-Object System.IO.StreamReader($pipeServer)
while ($true) {
    $line = $reader.ReadLine()
    if ($line -eq $null) { break }
    Add-Content -Path $logFile -Value $line
    Write-Host $line
}
