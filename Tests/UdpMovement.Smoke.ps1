param([string]$Executable = "$PSScriptRoot\..\build\Winsock.exe")
$ErrorActionPreference = 'Stop'
$server = $null
$clients = @()
function Send-Text($client, [string]$message) {
    $bytes = [Text.Encoding]::ASCII.GetBytes($message)
    [void]$client.Send($bytes, $bytes.Length)
}
function Read-Until($client, [string]$prefix) {
    $deadline = [DateTime]::UtcNow.AddSeconds(3)
    while ([DateTime]::UtcNow -lt $deadline) {
        $remote = [Net.IPEndPoint]::new([Net.IPAddress]::Any, 0)
        $text = [Text.Encoding]::ASCII.GetString($client.Receive([ref]$remote))
        if ($text.StartsWith($prefix)) { return $text }
    }
    throw "Did not receive $prefix"
}
function Latest-State($client) {
    $text = Read-Until $client 'STATE '
    while ($client.Available -gt 0) { $text = Read-Until $client 'STATE ' }
    return ,($text.Split(' '))
}
try {
    $server = Start-Process -FilePath $Executable -ArgumentList 'udp-server' -WindowStyle Hidden -PassThru
    Start-Sleep -Milliseconds 250
    if ($server.HasExited) { throw 'Server exited before test' }
    for ($i = 0; $i -lt 3; $i++) {
        $client = [Net.Sockets.UdpClient]::new()
        $client.Connect('127.0.0.1', 27016)
        $client.Client.ReceiveTimeout = 3000
        $clients += $client
        Send-Text $client 'HELLO'
        if ($i -lt 2) {
            $welcome = Read-Until $client 'WELCOME '
            if ($welcome -ne "WELCOME $($i + 1)") { throw "Unexpected ID: $welcome" }
        } elseif ((Read-Until $client 'FULL') -ne 'FULL') { throw 'Capacity check failed' }
    }
    # Repeated HELLO must reuse the same player ID.
    Send-Text $clients[0] 'HELLO'
    if ((Read-Until $clients[0] 'WELCOME ') -ne 'WELCOME 1') { throw 'HELLO is not idempotent' }
    Send-Text $clients[0] 'INPUT 10 1 0'
    Send-Text $clients[0] 'INPUT 9 -1 0' # stale: must not reverse movement
    Send-Text $clients[0] 'INPUT 11 999 0' # invalid: must not change movement
    Start-Sleep -Milliseconds 180
    $state = Latest-State $clients[1]
    if ($state.Length -ne 8 -or $state[2] -ne '1' -or $state[5] -ne '2') { throw 'Both players not replicated' }
    $x = [double]::Parse($state[3], [Globalization.CultureInfo]::InvariantCulture)
    if ($x -le 0 -or $x -gt 1.25 -or $state[6] -ne '0.00') { throw "Movement validation failed: $state" }
    Start-Sleep -Milliseconds 400
    $stopped = Latest-State $clients[1]
    Start-Sleep -Milliseconds 200
    $later = Latest-State $clients[1]
    if ($stopped[3] -ne $later[3]) { throw 'Input timeout did not stop movement' }
    # Both clients should receive the same authoritative positions once stopped.
    $other = Latest-State $clients[0]
    if (($other[2..7] -join ' ') -ne ($later[2..7] -join ' ')) { throw 'Client states differ' }
    Write-Output 'PASS: two IDs, repeated HELLO, capacity, movement replication, stale/invalid input rejection, input timeout.'
} finally {
    foreach ($client in $clients) { $client.Dispose() }
    if ($server -and !$server.HasExited) { Stop-Process -Id $server.Id }
}
