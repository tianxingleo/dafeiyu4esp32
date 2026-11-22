param(
    [ValidateSet('Setup', 'Ports', 'Compile', 'Upload')]
    [string]$Action = 'Compile',

    [string]$Port,

    [string]$Sketch = 'WhaleMate'
)

$ErrorActionPreference = 'Stop'
if ($Action -eq 'Upload' -and $Port -notmatch '^COM[1-9][0-9]*$') {
    throw 'Specify a serial port with -Port COM6.'
}

$usedLetters = (Get-PSDrive -PSProvider FileSystem).Name
$letter = 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z' |
    Where-Object { $_ -notin $usedLetters } |
    Select-Object -First 1
if (-not $letter) {
    throw 'No free drive letter from S: to Z:.'
}

$drive = "${letter}:"
# Keep the ESP32 compiler's Windows paths short.
subst.exe $drive $PSScriptRoot
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

Push-Location "$drive\"
try {
    $cli = '.\.arduino\bin\arduino-cli.exe'
    if (-not (Test-Path -LiteralPath $cli -PathType Leaf)) {
        $command = Get-Command arduino-cli -CommandType Application -ErrorAction SilentlyContinue
        if (-not $command) {
            throw 'Install Arduino CLI and add it to PATH, or put arduino-cli.exe in .arduino\bin.'
        }
        $cli = $command.Source
    }
    $config = '.\arduino-cli.yaml'
    $board = 'esp32:esp32:esp32s3:FlashSize=8M,FlashMode=qio,PSRAM=opi,PartitionScheme=default_8MB,USBMode=hwcdc,CDCOnBoot=cdc'
    if ($Sketch -eq 'HelloQoder') {
        $Sketch = 'examples\HelloQoder'
    }
    $build = Join-Path '.\.build' (Split-Path -Leaf $Sketch)
    switch ($Action) {
        'Setup' {
            & $cli --config-file $config core update-index
            if ($LASTEXITCODE -ne 0) { break }
            & $cli --config-file $config core install 'esp32:esp32@3.3.10-cn'
            if ($LASTEXITCODE -ne 0) { break }
            & $cli --config-file $config lib install 'M5GFX@0.3.0' 'M5Unified@0.3.0'
        }
        'Ports' { & $cli --config-file $config board list }
        'Compile' {
            & $cli --config-file $config compile --fqbn $board --build-path $build $Sketch
        }
        'Upload' {
            & $cli --config-file $config upload --fqbn $board --port $Port --input-dir $build --verify $Sketch
        }
    }
    $arduinoExit = $LASTEXITCODE
}
finally {
    Pop-Location
    subst.exe $drive /D
}
exit $arduinoExit
