# Averages sets of the engine's TGA screenshots of the same (paused) frame, in
# linear light (sRGB decoded first: averaging sRGB values makes a noisier image
# look darker), and compares two sets: their means, the share of 60-pixel
# blocks (above -MaxY, e.g. the HUD rows) whose means differ by more than 5 %
# and 10 %, and each set's noise (the root of the mean per-pixel variance over
# the mean, relative). A paused frame still gets new random numbers each frame,
# so the average of N shots is an N-sample image. -Dir holds the shots
# (hexenNN.tga), -A and -B their numbers; with -OutDir, writes the averages as
# <Name>-A.tga / <Name>-B.tga (sRGB) for tga2png.ps1.
param([string]$Dir, [int[]]$A, [int[]]$B = @(), [string]$Name = 'mean', [int]$Block = 60, [int]$MaxY = 100000,
      [string]$OutDir = '')
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class TgaMean {
    public static int W, H;
    static double[] lut = MakeLut();
    static double[] MakeLut() {
        var t = new double[256];
        for (int i = 0; i < 256; i++) { double v = i / 255.0; t[i] = v <= 0.04045 ? v / 12.92 : Math.Pow((v + 0.055) / 1.055, 2.4); }
        return t;
    }
    // the linear mean of the files (top-down RGB) and the mean per-pixel variance above maxY
    public static double[] Load(string[] files, int maxY, out double variance) {
        double[] s = null, s2 = null;
        foreach (var f in files) {
            var b = File.ReadAllBytes(f);
            int w = b[12] | (b[13] << 8), h = b[14] | (b[15] << 8), ofs = 18 + b[0];
            bool top = (b[17] & 0x20) != 0;
            if (b[2] != 2 || b[16] != 24)
                throw new Exception(f + ": not an uncompressed 24-bit TGA");
            if (s == null && W == 0) { W = w; H = h; }	// the first set's size for both sets
            if (w != W || h != H)
                throw new Exception(String.Format("{0}: {1}x{2}, the others are {3}x{4}", f, w, h, W, H));
            if (s == null) { s = new double[w * h * 3]; s2 = new double[w * h * 3]; }
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++)
                    for (int c = 0; c < 3; c++) {
                        double v = lut[b[ofs + (y * w + x) * 3 + c]];
                        int i = ((top ? y : h - 1 - y) * w + x) * 3 + (2 - c);
                        s[i] += v; s2[i] += v * v;
                    }
        }
        int n = files.Length, region = Math.Min(maxY, H) * W * 3;
        variance = 0;
        for (int i = 0; i < s.Length; i++) {
            s[i] /= n;
            if (n > 1 && i < region) variance += Math.Max(s2[i] / n - s[i] * s[i], 0) * n / (n - 1);
        }
        variance /= region;
        return s;
    }
    public static double Mean(double[] a, int maxY) {
        double m = 0; int rows = Math.Min(maxY, H);
        for (int i = 0; i < rows * W * 3; i++) m += a[i];
        return m / (rows * W * 3);
    }
    // blocks whose means differ by more than 5 % and 10 % (dark blocks skipped)
    public static string Blocks(double[] a, double[] b, int block, int maxY) {
        int blocks = 0, over5 = 0, over10 = 0, rows = Math.Min(maxY, H);
        double worst = 0;
        for (int by = 0; by + block <= rows; by += block)
            for (int bx = 0; bx + block <= W; bx += block) {
                double sa = 0, sb = 0;
                for (int y = by; y < by + block; y++)
                    for (int x = bx; x < bx + block; x++)
                        for (int c = 0; c < 3; c++) { sa += a[(y * W + x) * 3 + c]; sb += b[(y * W + x) * 3 + c]; }
                if (sa < 0.005 * block * block * 3 && sb < 0.005 * block * block * 3) continue;
                blocks++;
                double r = Math.Abs(sa - sb) / Math.Max(sa, sb);
                if (r > 0.05) over5++;
                if (r > 0.10) over10++;
                worst = Math.Max(worst, r);
            }
        return String.Format("{0} lit blocks: {1} differ > 5 %, {2} > 10 %, worst {3:F1} %", blocks, over5, over10, worst * 100);
    }
    public static void Save(double[] img, string path) {
        var o = new byte[18 + W * H * 3];
        o[2] = 2; o[12] = (byte)(W & 255); o[13] = (byte)(W >> 8); o[14] = (byte)(H & 255); o[15] = (byte)(H >> 8); o[16] = 24;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                for (int c = 0; c < 3; c++) {
                    double v = img[(y * W + x) * 3 + c];
                    v = v <= 0.0031308 ? 12.92 * v : 1.055 * Math.Pow(v, 1 / 2.4) - 0.055;
                    o[18 + ((H - 1 - y) * W + x) * 3 + (2 - c)] = (byte)Math.Max(0, Math.Min(255, Math.Round(v * 255)));
                }
        File.WriteAllBytes(path, o);
    }
}
'@
function Files([int[]]$n) { $n | ForEach-Object { Join-Path $Dir ('hexen{0:D2}.tga' -f $_) } }
[TgaMean]::W = 0; [TgaMean]::H = 0	# the type lives on in the PowerShell session
$va = 0.0; $vb = 0.0
$ia = [TgaMean]::Load([string[]](Files $A), $MaxY, [ref]$va)
$ma = [TgaMean]::Mean($ia, $MaxY)
$line = "${Name}: A mean {0:F4} noise {1:F3}" -f $ma, ([math]::Sqrt($va) / [math]::Max($ma, 1e-9))
if ($B.Count) {
    $ib = [TgaMean]::Load([string[]](Files $B), $MaxY, [ref]$vb)
    $mb = [TgaMean]::Mean($ib, $MaxY)
    $line += "; B mean {0:F4} noise {1:F3} ({2:+0.0;-0.0} %); " -f $mb, ([math]::Sqrt($vb) / [math]::Max($mb, 1e-9)), (($mb - $ma) / [math]::Max($ma, 1e-9) * 100)
    $line += [TgaMean]::Blocks($ia, $ib, $Block, $MaxY)
}
$line
if ($OutDir) {
    New-Item -ItemType Directory -Force $OutDir | Out-Null
    [TgaMean]::Save($ia, (Join-Path $OutDir "$Name-A.tga"))
    if ($B.Count) { [TgaMean]::Save($ib, (Join-Path $OutDir "$Name-B.tga")) }
}
