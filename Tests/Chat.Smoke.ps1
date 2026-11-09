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
    $process.StartInfo.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    [void]$process.Start()
    $processes.Add($process)
    return $process
}

function Expect-Line($Reader, [string]$Expected) {
    for ($i = 0; $i -lt 30; ++$i) {
        $line = $Reader.ReadLineAsync()
        if (-not $line.Wait(5000)) { throw "Timeout waiting for: $Expected" }
        if ($null -eq $line.Result) { throw "Connection ended waiting for: $Expected" }
        if ($line.Result.Contains($Expected)) { return }
    }
    throw "Expected line not found: $Expected"
}

function Send-Input($Process, [string]$Message) {
    # .NET Framework's default pipe writer may use the system ANSI code page.
    # Match the client's UTF-8 console input explicitly.
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Message + "`n")
    $Process.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    $Process.StandardInput.BaseStream.Flush()
}

$raw = $null
$oversized = $null
try {
    $server = Start-Chat 'server'
    Expect-Line $server.StandardOutput 'Chat server listening'
    # Drain logs asynchronously so a full pipe cannot block the server.
    $serverOutput = $server.StandardOutput.ReadToEndAsync()
    $serverErrors = $server.StandardError.ReadToEndAsync()

    $alice = Start-Chat "client $ServerHost"
    Expect-Line $alice.StandardOutput 'You are user1.'
    $bob = Start-Chat "chat-client $ServerHost"
    Expect-Line $bob.StandardOutput 'You are user2.'
    Send-Input $alice 'hello from Alice'
    Expect-Line $alice.StandardOutput '[user1] hello from Alice'
    Expect-Line $bob.StandardOutput '[user1] hello from Alice'
    Send-Input $bob 'hello from Bob'
    Expect-Line $alice.StandardOutput '[user2] hello from Bob'
    Expect-Line $bob.StandardOutput '[user2] hello from Bob'
    Write-Output 'PASS: two real chat clients send and receive broadcasts'

    $raw = [System.Net.Sockets.TcpClient]::new($ServerHost, 27017)
    $stream = $raw.GetStream()
    $reader = [System.IO.StreamReader]::new($stream, [System.Text.Encoding]::UTF8)
    Expect-Line $reader 'You are user3.'
    # Split even a UTF-8 multibyte character across different send calls.
    $korean = -join ([char[]](0xC548, 0xB155, 0xD558, 0xC138, 0xC694))
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($korean + "`n")
    for ($i = 0; $i -lt $bytes.Length; ++$i) {
        $stream.Write($bytes, $i, 1)
        Start-Sleep -Milliseconds 5
    }
    Expect-Line $alice.StandardOutput "[user3] $korean"
    Expect-Line $bob.StandardOutput "[user3] $korean"
    Expect-Line $reader "[user3] $korean"
    Write-Output 'PASS: fragmented UTF-8 Korean message reconstructed exactly'

    Send-Input $bob $korean
    Expect-Line $alice.StandardOutput "[user2] $korean"
    Expect-Line $bob.StandardOutput "[user2] $korean"
    Write-Output 'PASS: Korean input through the actual chat client'

    $bytes = [System.Text.Encoding]::UTF8.GetBytes("first`nsecond`n")
    $stream.Write($bytes, 0, $bytes.Length)
    foreach ($client in @($alice, $bob)) {
        Expect-Line $client.StandardOutput '[user3] first'
        Expect-Line $client.StandardOutput '[user3] second'
    }
    Write-Output 'PASS: multiple messages in one send remain separate'

    $oversized = [System.Net.Sockets.TcpClient]::new($ServerHost, 27017)
    $largeStream = $oversized.GetStream()
    $largeReader = [System.IO.StreamReader]::new($largeStream)
    Expect-Line $largeReader 'You are user4.'
    $bytes = [System.Text.Encoding]::ASCII.GetBytes(('x' * 1025) + "`n")
    $largeStream.Write($bytes, 0, $bytes.Length)
    Expect-Line $largeReader 'Message too long'
    Expect-Line $alice.StandardOutput 'user4 left'
    Write-Output 'PASS: oversized message rejected without stopping server'

    Send-Input $bob '/quit'
    if (-not $bob.WaitForExit(5000) -or $bob.ExitCode -ne 0) { throw '/quit did not exit cleanly' }
    Expect-Line $alice.StandardOutput 'user2 left'
    Send-Input $alice 'still connected'
    Expect-Line $alice.StandardOutput '[user1] still connected'
    Write-Output 'PASS: /quit closes one client; remaining client still chats'

    $server.Kill()
    $server.WaitForExit()
    $logs = $serverOutput.Result
    foreach ($expected in @(
        "user1 ${ServerHost}:",
        'ACCEPT: established connection',
        'SEND ',
        'RECV EOF: peer gracefully closed',
        'SHUTDOWN SD_SEND:',
        'CLOSE: socket released'
    )) {
        if (-not $logs.Contains($expected)) { throw "Missing TCP log: $expected" }
    }
    Write-Output 'PASS: server logs peer endpoint, IO, and graceful disconnect lifecycle'
    Expect-Line $alice.StandardOutput 'Disconnected from server'
    Send-Input $alice ''
    if (-not $alice.WaitForExit(5000)) { throw 'Client failed to exit after server disconnect' }
    Write-Output 'PASS: server disconnect detected; Enter exits client'
}
finally {
    if ($raw) { $raw.Dispose() }
    if ($oversized) { $oversized.Dispose() }
    foreach ($process in $processes) {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
    }
}
