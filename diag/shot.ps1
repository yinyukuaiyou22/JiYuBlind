param([string]$label = "shot", [int]$sx = 660, [int]$sy = 480)
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms
$vs = [System.Windows.Forms.SystemInformation]::VirtualScreen
$bmp = New-Object System.Drawing.Bitmap $vs.Width, $vs.Height
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.CopyFromScreen($vs.Left, $vs.Top, 0, 0, $bmp.Size)
$px = $sx - $vs.Left
$py = $sy - $vs.Top
$c = $bmp.GetPixel($px, $py)
$out = Join-Path "E:\反制录屏app\.probe" ("$label.png")
$bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
"{0}  screen={1}x{2}  sample=({3},{4})  pixel=0x{5:X2}{6:X2}{7:X2}" -f $label, $vs.Width, $vs.Height, $sx, $sy, $c.R, $c.G, $c.B
$g.Dispose(); $bmp.Dispose()
