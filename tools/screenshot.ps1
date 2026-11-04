# Снимок экрана метеостанции по USB.
# Пример: powershell -ExecutionPolicy Bypass -File tools\screenshot.ps1 -Port COM3 -Out shot.png -Pre "2"
#   -Pre: команды перед снимком (d - демо, 0..4 - демо уровня, r - реальные данные)

param(
  [string]$Port = 'COM3',
  [string]$Out = 'shot.png',
  [string]$Pre = '',
  [int]$Scale = 2,
  [int]$WaitMs = 1500
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$sp = New-Object System.IO.Ports.SerialPort $Port, 115200
$sp.ReadTimeout = 5000
$sp.DtrEnable = $true
$sp.Open()
try {
  Start-Sleep -Milliseconds 200
  $sp.DiscardInBuffer()
  if ($Pre) { $sp.Write($Pre); Start-Sleep -Milliseconds $WaitMs }
  $sp.DiscardInBuffer()
  $sp.Write('s')

  # Ждём заголовок "SHOT W H\n"
  $line = ''
  $deadline = (Get-Date).AddSeconds(10)
  while ($true) {
    if ((Get-Date) -gt $deadline) { throw 'Нет ответа от платы' }
    $ch = $sp.ReadChar()
    if ($ch -eq 10) {
      if ($line.StartsWith('SHOT')) { break }
      $line = ''
    } elseif ($ch -ne 13) { $line += [char]$ch }
  }
  $parts = $line.Split(' ')
  $w = [int]$parts[1]; $h = [int]$parts[2]
  $total = $w * $h * 2
  $data = New-Object byte[] $total
  $got = 0
  while ($got -lt $total) {
    $n = $sp.Read($data, $got, $total - $got)
    $got += $n
  }
} finally {
  $sp.Close()
}

$bmp = New-Object System.Drawing.Bitmap ($w * $Scale), ($h * $Scale)
$rect = New-Object System.Drawing.Rectangle 0, 0, ($w * $Scale), ($h * $Scale)
$bd = $bmp.LockBits($rect, 'WriteOnly', 'Format32bppArgb')
$px = New-Object byte[] ($w * $Scale * $h * $Scale * 4)
$stride = $w * $Scale * 4
for ($y = 0; $y -lt $h; $y++) {
  for ($x = 0; $x -lt $w; $x++) {
    $i = ($y * $w + $x) * 2
    $c = [int]$data[$i] -bor ([int]$data[$i + 1] -shl 8)
    $r = (($c -shr 11) -band 0x1F) * 255 / 31
    $g = (($c -shr 5) -band 0x3F) * 255 / 63
    $b = ($c -band 0x1F) * 255 / 31
    for ($sy = 0; $sy -lt $Scale; $sy++) {
      $o = ($y * $Scale + $sy) * $stride + $x * $Scale * 4
      for ($sx = 0; $sx -lt $Scale; $sx++) {
        $px[$o] = [byte]$b; $px[$o + 1] = [byte]$g; $px[$o + 2] = [byte]$r; $px[$o + 3] = 255
        $o += 4
      }
    }
  }
}
[System.Runtime.InteropServices.Marshal]::Copy($px, 0, $bd.Scan0, $px.Length)
$bmp.UnlockBits($bd)
$outPath = Join-Path (Get-Location) $Out
New-Item -ItemType Directory -Force (Split-Path -Parent $outPath) | Out-Null
$bmp.Save($outPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "Saved $Out ($w x $h)"
