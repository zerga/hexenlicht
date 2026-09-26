param([string[]]$Files, [int]$X, [int]$Y, [int]$W, [int]$H, [string]$Out, [int]$Scale = 2)
# crops the same rectangle out of each PNG and puts the crops side by side (scaled up)
Add-Type -AssemblyName System.Drawing
$strip = New-Object System.Drawing.Bitmap ($W * $Scale * $Files.Count + 4 * ($Files.Count - 1)), ($H * $Scale)
$g = [System.Drawing.Graphics]::FromImage($strip)
$g.Clear([System.Drawing.Color]::Magenta)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
$g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
$i = 0
foreach ($f in $Files) {
	$img = [System.Drawing.Image]::FromFile($f)
	$dst = New-Object System.Drawing.Rectangle ($i * ($W * $Scale + 4)), 0, ($W * $Scale), ($H * $Scale)
	$src = New-Object System.Drawing.Rectangle $X, $Y, $W, $H
	$g.DrawImage($img, $dst, $src, [System.Drawing.GraphicsUnit]::Pixel)
	$img.Dispose()
	$i++
}
$g.Dispose()
$strip.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$strip.Dispose()
"wrote $Out"
