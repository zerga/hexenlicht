# Scores a fine checkerboard (the two checkerboard fields showing different
# surfaces, story 3.12) in the engine's TGA screenshots. The shots (-Dir,
# hexenNN.tga numbered by -Shots) are averaged in linear light first, as in
# tga_mean.ps1 (a paused frame gets new random numbers each frame, so noise
# averages out and a checkerboard that stays in place doesn't). Per pixel the
# luminance minus the mean of its four neighbours, with the sign flipped on
# every other pixel, is averaged over -Block square blocks: a checkerboard
# keeps one sign and scores up to 2 (|a - b| / mean of a and b), texture
# detail and leftover noise score low. Prints the highest block and how
# many are above -Threshold, inside -Region (x, y, width, height; default
# the rows above -MaxY). With -Heat, writes the average with the blocks in
# red (brighter = higher) as a TGA for tga2png.ps1.
param([string]$Dir, [int[]]$Shots, [int]$Block = 8, [int]$MaxY = 100000, [int[]]$Region = @(),
      [double]$Threshold = 0.3, [string]$Heat = '')
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class TgaChecker {
    public static int W, H;
    static double[] lut = MakeLut();
    static double[] MakeLut() {
        var t = new double[256];
        for (int i = 0; i < 256; i++) { double v = i / 255.0; t[i] = v <= 0.04045 ? v / 12.92 : Math.Pow((v + 0.055) / 1.055, 2.4); }
        return t;
    }
    // the linear mean of the files, top-down RGB
    public static double[] Load(string[] files) {
        double[] s = null;
        foreach (var f in files) {
            var b = File.ReadAllBytes(f);
            int w = b[12] | (b[13] << 8), h = b[14] | (b[15] << 8), ofs = 18 + b[0];
            bool top = (b[17] & 0x20) != 0;
            if (b[2] != 2 || b[16] != 24)
                throw new Exception(f + ": not an uncompressed 24-bit TGA");
            if (s == null) { W = w; H = h; s = new double[w * h * 3]; }
            if (w != W || h != H)
                throw new Exception(String.Format("{0}: {1}x{2}, the others are {3}x{4}", f, w, h, W, H));
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    for (int c = 0; c < 3; c++)
                        s[((top ? y : h - 1 - y) * w + x) * 3 + (2 - c)] += lut[b[ofs + (y * w + x) * 3 + c]];
        }
        for (int i = 0; i < s.Length; i++) s[i] /= files.Length;
        return s;
    }
    // the score of each block (row-major, W / block per row)
    public static double[] Score(double[] img, int block) {
        var L = new double[W * H];
        for (int i = 0; i < W * H; i++) L[i] = 0.2126 * img[i * 3] + 0.7152 * img[i * 3 + 1] + 0.0722 * img[i * 3 + 2];
        int bw = W / block, bh = H / block;
        var m = new double[bw * bh];
        for (int by = 0; by < bh; by++)
            for (int bx = 0; bx < bw; bx++) {
                double s = 0, a = 0; int n = 0;
                for (int y = Math.Max(by * block, 1); y < Math.Min(by * block + block, H - 1); y++)
                    for (int x = Math.Max(bx * block, 1); x < Math.Min(bx * block + block, W - 1); x++) {
                        int i = y * W + x;
                        double c = L[i] - 0.25 * (L[i - 1] + L[i + 1] + L[i - W] + L[i + W]);
                        s += ((x + y) & 1) == 0 ? c : -c; a += L[i]; n++;
                    }
                m[by * bw + bx] = n > 0 ? Math.Abs(s / n) / Math.Max(a / n, 0.02) : 0;
            }
        return m;
    }
    public static void SaveHeat(double[] img, double[] m, int block, string path) {
        int bw = W / block;
        var o = new byte[18 + W * H * 3];
        o[2] = 2; o[12] = (byte)(W & 255); o[13] = (byte)(W >> 8); o[14] = (byte)(H & 255); o[15] = (byte)(H >> 8); o[16] = 24;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int bx = x / block, by = y / block;
                double t = (bx < bw && by < H / block) ? Math.Min(1.0, m[by * bw + bx]) : 0;
                for (int c = 0; c < 3; c++) {
                    double v = img[(y * W + x) * 3 + c] * 0.4;
                    v = v <= 0.0031308 ? 12.92 * v : 1.055 * Math.Pow(v, 1 / 2.4) - 0.055;
                    v = c == 0 ? v + (1 - v) * t : v * (1 - t);
                    o[18 + ((H - 1 - y) * W + x) * 3 + (2 - c)] = (byte)Math.Max(0, Math.Min(255, Math.Round(v * 255)));
                }
            }
        File.WriteAllBytes(path, o);
    }
}
'@
if (-not $Shots) { throw '-Shots: the numbers of the shots to average' }
$Dir = Convert-Path $Dir	# absolute: [IO.File] resolves against the process's folder
if ($Heat) { $Heat = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Heat) }
$files = [string[]]($Shots | ForEach-Object { Join-Path $Dir ('hexen{0:D2}.tga' -f $_) })
$img = [TgaChecker]::Load($files)
$w = [TgaChecker]::W; $h = [TgaChecker]::H
$m = [TgaChecker]::Score($img, $Block)
$rx, $ry, $rw, $rh = if ($Region.Count -eq 4) { $Region } else { 0, 0, $w, [math]::Min($MaxY, $h) }
$bw = [math]::Floor($w / $Block); $bh = [math]::Floor($h / $Block)
$max = 0.0; $at = ''; $over = 0; $count = 0
for ($by = 0; $by -lt $bh; $by++) {
	for ($bx = 0; $bx -lt $bw; $bx++) {
		$x = $bx * $Block; $y = $by * $Block
		if ($x -lt $rx -or $y -lt $ry -or $x + $Block -gt $rx + $rw -or $y + $Block -gt $ry + $rh) { continue }
		$v = $m[$by * $bw + $bx]; $count++
		if ($v -gt $Threshold) { $over++ }
		if ($v -gt $max) { $max = $v; $at = "($x,$y)" }
	}
}
"{0} shot(s): highest block {1:F3} at {2}; {3} of {4} blocks above {5}" -f $files.Count, $max, $at, $over, $count, $Threshold
if ($Heat) { [TgaChecker]::SaveHeat($img, $m, $Block, $Heat) }
