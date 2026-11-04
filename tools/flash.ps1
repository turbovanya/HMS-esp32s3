# Сборка и прошивка через arduino-cli из комплекта Arduino IDE 2.
# Пример: powershell -ExecutionPolicy Bypass -File tools\flash.ps1 -Port COM3
#   -NoUpload: только собрать

param(
  [string]$Port = 'COM3',
  [switch]$NoUpload
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$cli = 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe'
$cfg = Join-Path $env:USERPROFILE '.arduinoIDE\arduino-cli.yaml'
$fqbn = 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,' +
        'DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=4M,' +
        'PartitionScheme=default,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,' +
        'EraseFlash=none,JTAGAdapter=default'
$sketch = Join-Path $root 'MeteoStation'
$build = Join-Path $root 'build'

& $cli compile --config-file $cfg --fqbn $fqbn --build-path $build $sketch
if ($LASTEXITCODE -ne 0) { throw 'Сборка не удалась' }
if (-not $NoUpload) {
  & $cli upload --config-file $cfg --fqbn $fqbn -p $Port --input-dir $build $sketch
  if ($LASTEXITCODE -ne 0) { throw 'Прошивка не удалась' }
}
