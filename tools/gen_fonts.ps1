# Генерирует сглаженные 4-битные шрифты для прошивки из файла Inter SemiBold в tools/fonts.
# Запуск: powershell -ExecutionPolicy Bypass -File tools\gen_fonts.ps1
# Результат: MeteoStation\Fonts.cpp и MeteoStation\Fonts.h

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Runtime.InteropServices;
using System.Text;

public static class FontGen {
    public static string Generate(string name, string fontPath, float px, string chars, bool gridFit, StringBuilder decl) {
        using (var fonts = new PrivateFontCollection()) {
        fonts.AddFontFile(fontPath);
        using (var ff = fonts.Families[0]) {
        using (var font = new Font(ff, px, FontStyle.Regular, GraphicsUnit.Pixel)) {
        float ascentPx = px * ff.GetCellAscent(FontStyle.Regular) / ff.GetEmHeight(FontStyle.Regular);
        float descentPx = px * ff.GetCellDescent(FontStyle.Regular) / ff.GetEmHeight(FontStyle.Regular);

        int S = (int)Math.Ceiling(px * 3);
        int ox = (int)Math.Ceiling(px * 0.75f), oy = (int)Math.Ceiling(px * 0.5f);
        int baseline = (int)Math.Round(oy + ascentPx);

        var fmt = (StringFormat)StringFormat.GenericTypographic.Clone();
        fmt.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces;

        // Уникальные символы, отсортированные по коду
        var codes = new SortedSet<int>();
        foreach (char c in chars) codes.Add(c);

        var bmpBytes = new List<byte>();
        var glyphs = new StringBuilder();
        var codeList = new StringBuilder();
        int capH = 0;

        foreach (int code in codes) {
            string s = ((char)code).ToString();
            using (var bmp = new Bitmap(S, S, PixelFormat.Format32bppArgb))
            using (var g = Graphics.FromImage(bmp)) {
                g.Clear(Color.Black);
                g.TextRenderingHint = gridFit ? TextRenderingHint.AntiAliasGridFit : TextRenderingHint.AntiAlias;
                g.DrawString(s, font, Brushes.White, ox, oy, fmt);
                float adv = g.MeasureString(s, font, new PointF(0, 0), fmt).Width;

                var data = bmp.LockBits(new Rectangle(0, 0, S, S), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
                byte[] px32 = new byte[S * S * 4];
                Marshal.Copy(data.Scan0, px32, 0, px32.Length);
                bmp.UnlockBits(data);

                int minX = S, minY = S, maxX = -1, maxY = -1;
                for (int y = 0; y < S; y++)
                    for (int x = 0; x < S; x++) {
                        int v = px32[(y * S + x) * 4 + 2];
                        if (v > 10) {
                            if (x < minX) minX = x; if (x > maxX) maxX = x;
                            if (y < minY) minY = y; if (y > maxY) maxY = y;
                        }
                    }

                int w = 0, h = 0, xo = 0, yo = 0;
                int offset = bmpBytes.Count;
                if (maxX >= 0) {
                    w = maxX - minX + 1; h = maxY - minY + 1;
                    xo = minX - ox; yo = minY - baseline;
                    for (int y = minY; y <= maxY; y++) {
                        int acc = 0; bool hi = true;
                        for (int x = minX; x <= maxX; x++) {
                            int v = px32[(y * S + x) * 4 + 2];
                            int q = (v * 15 + 127) / 255;
                            if (hi) { acc = q << 4; hi = false; }
                            else { bmpBytes.Add((byte)(acc | q)); hi = true; }
                        }
                        if (!hi) bmpBytes.Add((byte)acc);
                    }
                }
                if (code == 'H') capH = h;
                int advPx = (int)Math.Round(adv);
                glyphs.AppendFormat("  {{{0}, {1}, {2}, {3}, {4}, {5}}},\n", offset, w, h, xo, yo, advPx);
                codeList.AppendFormat("{0},", code);
            }
        }
        if (capH == 0) {
            // Шрифт только из цифр: высота заглавной по цифре 0
            capH = MeasureCap(font, ox, oy, S, gridFit);
        }

        var sb = new StringBuilder();
        sb.AppendFormat("// {0}: {1}, {2}px, {3} символов\n", name, ff.Name, px, codes.Count);
        sb.AppendFormat("static const uint8_t {0}_bmp[] = {{\n", name);
        for (int i = 0; i < bmpBytes.Count; i++) {
            if (i % 24 == 0) sb.Append("  ");
            sb.AppendFormat("0x{0:X2},", bmpBytes[i]);
            if (i % 24 == 23) sb.Append("\n");
        }
        sb.Append("\n};\n");
        sb.AppendFormat("static const uint16_t {0}_codes[] = {{{1}}};\n", name, codeList.ToString());
        sb.AppendFormat("static const Glyph {0}_glyphs[] = {{\n{1}}};\n", name, glyphs.ToString());
        sb.AppendFormat("const Font {0} = {{{0}_bmp, {0}_codes, {0}_glyphs, {1}, {2}, {3}, {4}}};\n\n",
            name, codes.Count, (int)Math.Round(ascentPx), (int)Math.Round(descentPx), capH);
        decl.AppendFormat("extern const Font {0};\n", name);
        return sb.ToString();
        }
        }
        }
    }

    static int MeasureCap(Font font, int ox, int oy, int S, bool gridFit) {
        using (var bmp = new Bitmap(S, S, PixelFormat.Format32bppArgb))
        using (var g = Graphics.FromImage(bmp)) {
            g.Clear(Color.Black);
            g.TextRenderingHint = gridFit ? TextRenderingHint.AntiAliasGridFit : TextRenderingHint.AntiAlias;
            g.DrawString("0", font, Brushes.White, ox, oy, StringFormat.GenericTypographic);
            int minY = S, maxY = -1;
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++)
                    if (bmp.GetPixel(x, y).R > 10) { if (y < minY) minY = y; if (y > maxY) maxY = y; }
            return maxY - minY + 1;
        }
    }
}
'@

$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $root 'MeteoStation'
New-Item -ItemType Directory -Force $out | Out-Null

$ascii = -join ((32..126) | ForEach-Object { [char]$_ })
$cyr = -join ((0x410..0x44F) | ForEach-Object { [char]$_ }) + [char]0x401 + [char]0x451
$extra = [string][char]0xB0 + [char]0xB5 + [char]0xB2 + [char]0xB3 + [char]0x2082 + [char]0xB7
$text = $ascii + $cyr + $extra
$digits = '0123456789.-+% ' + [char]0xB0

$decl = New-Object System.Text.StringBuilder
$body = New-Object System.Text.StringBuilder
$fam = (Join-Path $PSScriptRoot 'fonts/Inter-SemiBold.ttf')
[void]$body.Append([FontGen]::Generate('fontHero',  $fam, 46, '0123456789- ', $false, $decl))
[void]$body.Append([FontGen]::Generate('fontBig',   $fam, 34, $digits, $false, $decl))
[void]$body.Append([FontGen]::Generate('fontMed',   $fam, 24, $digits, $false, $decl))
[void]$body.Append([FontGen]::Generate('fontLabel', $fam, 14, $text, $true, $decl))
[void]$body.Append([FontGen]::Generate('fontSmall', $fam, 11, $text, $true, $decl))

$hdr = "// Сгенерировано tools/gen_fonts.ps1, руками не править.`n#pragma once`n#include `"Gfx.h`"`n`n" + $decl.ToString()
$cpp = "// Сгенерировано tools/gen_fonts.ps1, руками не править.`n#include `"Fonts.h`"`n`n" + $body.ToString().TrimEnd() + "`n"

$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllText((Join-Path $out 'Fonts.h'), $hdr, $utf8)
[System.IO.File]::WriteAllText((Join-Path $out 'Fonts.cpp'), $cpp, $utf8)
Write-Host "OK:" (Get-Item (Join-Path $out 'Fonts.cpp')).Length "bytes"
