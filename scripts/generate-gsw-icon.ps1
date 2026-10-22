# Encode the checked-in generated PNG as a multi-resolution Windows ICO.
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
Add-Type -AssemblyName System.Drawing
$source=[Drawing.Image]::FromFile((Join-Path $root 'assets/gsw-icon.png'))
$target=Join-Path $root 'gsw/Assets/GSW.ico'
New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
$sizes=@(16,20,24,32,40,48,64,128,256)
$frames=@()
try {
    foreach($size in $sizes){
        $bitmap=[Drawing.Bitmap]::new($size,$size,[Drawing.Imaging.PixelFormat]::Format32bppArgb)
        $graphics=[Drawing.Graphics]::FromImage($bitmap)
        $stream=[IO.MemoryStream]::new()
        try {
            $graphics.Clear([Drawing.Color]::Transparent)
            $graphics.CompositingMode=[Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.DrawImage($source,0,0,$size,$size)
            $bitmap.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
            $frames+=,@($size,$stream.ToArray())
        } finally {$stream.Dispose();$graphics.Dispose();$bitmap.Dispose()}
    }
    $file=[IO.File]::Create($target)
    $writer=[IO.BinaryWriter]::new($file)
    try {
        $writer.Write([uint16]0);$writer.Write([uint16]1);$writer.Write([uint16]$sizes.Count)
        $offset=6+16*$sizes.Count
        foreach($frame in $frames){
            $side=if($frame[0] -eq 256){0}else{$frame[0]}
            $writer.Write([byte]$side);$writer.Write([byte]$side);$writer.Write([byte]0);$writer.Write([byte]0)
            $writer.Write([uint16]1);$writer.Write([uint16]32)
            $writer.Write([uint32]$frame[1].Length);$writer.Write([uint32]$offset)
            $offset+=$frame[1].Length
        }
        foreach($frame in $frames){$writer.Write([byte[]]$frame[1])}
    } finally {$writer.Dispose()}
} finally {$source.Dispose()}
Write-Output 'Encoded GSW.ico: 16/20/24/32/40/48/64/128/256 px; alpha retained'
