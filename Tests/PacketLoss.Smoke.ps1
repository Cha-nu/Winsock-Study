param(
    [string]$Exe = "$PSScriptRoot/../build/Chat/Winsock.exe",
    [string]$ServerHost = '127.0.0.1'
)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path -LiteralPath $Exe).Path
$processes = [System.Collections.Generic.List[System.Diagnostics.Process]]::new()
function Start-Chat([string]$Arguments) {
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo.FileName = $Exe
    $process.StartInfo.Arguments = $Arguments
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardInput = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    [void]$process.Start()
    $processes.Add($process)
    return $process
}
function Wait-Line($Reader, [string]$Expected) {
    for ($i = 0; $i -lt 30; ++$i) {
        $task = $Reader.ReadLineAsync()
        if (-not $task.Wait(7000)) { throw "Timeout: $Expected" }
        if ($null -eq $task.Result) { throw "Process ended: $Expected" }
        if ($task.Result.Contains('[loss] WFP unavailable') -or $task.Result.Contains('Filter creation failed')) {
            throw $task.Result
        }
        if ($task.Result.Contains($Expected)) { return }
    }
    throw "Missing: $Expected"
}
function Send-Line($Process, [string]$Message) {
    $Process.StandardInput.WriteLine($Message)
    $Process.StandardInput.Flush()
}
try {
    $server = Start-Chat 'server'
    Wait-Line $server.StandardOutput 'Chat server listening'
    $serverLog = $server.StandardOutput.ReadToEndAsync()
    $serverErr = $server.StandardError.ReadToEndAsync()
    $alice = Start-Chat "client $ServerHost"
    Wait-Line $alice.StandardOutput 'You are user1.'
    $bob = Start-Chat "client $ServerHost"
    Wait-Line $bob.StandardOutput 'You are user2.'

    foreach ($invalid in @('/loss 0', '/loss 6', '/loss -1', '/loss abc', '/loss 2x', '/loss 999999999999')) {
        Send-Line $alice $invalid
        Wait-Line $alice.StandardOutput 'Usage: /loss'
    }
    Send-Line $alice '/loss 3'
    Wait-Line $alice.StandardOutput '[loss] ACTIVE 3s:'
    $aliceLog = $alice.StandardOutput.ReadToEndAsync()
    # This client must remain responsive while Alice's flow is blackholed.
    Send-Line $bob 'unaffected peer'
    Wait-Line $bob.StandardOutput '[user2] unaffected peer'
    $bobLog = $bob.StandardOutput.ReadToEndAsync()
    Start-Sleep -Seconds 4
    Send-Line $alice 'after real packet loss'
    # Allow retransmission backoff and the server's observation window to end.
    Start-Sleep -Seconds 11
    Send-Line $alice '/quit'
    Send-Line $bob '/quit'
    foreach ($client in @($alice, $bob)) {
        if (-not $client.WaitForExit(5000) -or $client.ExitCode -ne 0) { throw 'Client did not exit cleanly' }
    }
    $server.Kill()
    $server.WaitForExit()
    $logs = $serverLog.Result
    foreach ($expected in @('LOSS-TEST requested:', 'RETRANSMISSION detected:', 'LOSS-TEST observation complete:', '(retransmission observed)')) {
        if (-not $logs.Contains($expected)) { throw "Missing server evidence: $expected`n$logs" }
    }
    foreach ($expected in @('[loss] RESTORED:', '[loss-probe]', '[user1] after real packet loss')) {
        if (-not $aliceLog.Result.Contains($expected)) { throw "Missing client recovery: $expected`n$($aliceLog.Result)" }
    }
    if (-not $bobLog.Result.Contains('[user1] after real packet loss')) { throw 'Peer did not receive recovered chat' }
    $logs.Split("`n") | Where-Object { $_ -match 'LOSS-TEST|RETRANSMISSION' } | Write-Output
    Write-Output 'PASS: real TCP retransmissions, automatic filter removal, recovered chat, unaffected peer, invalid duration rejection'
}
finally {
    foreach ($process in $processes) {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}
