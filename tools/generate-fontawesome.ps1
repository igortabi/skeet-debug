<#
  Regenerates the Font Awesome Free data used by skeetsdk.get_icon:

    skeet/fontawesome_icons.inc                 icon names -> code points (included by fontawesome.cpp)
    skeet/third_party/fontawesome/*.otf         the three fonts (embedded into skeet.dll by fontawesome.rc)
    skeet/third_party/fontawesome/LICENSE.txt   Font Awesome's licence (CC BY 4.0 icons, OFL 1.1 fonts, MIT code)
    skeet/third_party/fontawesome/README.md     which release this came from

  Input is the official "desktop" release zip, extracted:
    https://github.com/FortAwesome/Font-Awesome/releases  ->  fontawesome-free-<version>-desktop.zip

  Usage (from the repo root):
    powershell -File tools\generate-fontawesome.ps1 -Release C:\path\to\fontawesome-free-7.3.1-desktop [-ZipSha256 <hash>]

  Rules: icons whose code point is not in the Private Use Area (digits, letters, "*") are skipped so that
  ordinary text can never turn into an icon. Every icon has a primary style (solid, else brands, else
  regular) at its real code point; any other free style (regular, and brands for the two icons that have
  both) gets an unused Private Use code point as an alias, because GDI picks glyphs by code point only.
#>
param(
    [Parameter(Mandatory = $true)] [string] $Release,
    [string] $Root = (Split-Path -Parent $PSScriptRoot),
    [string] $ZipSha256 = ''
)

$ErrorActionPreference = 'Stop'
$eol = "`r`n"

function U16([byte[]]$b, [int]$o) { [int]$b[$o] * 256 + [int]$b[$o + 1] }
function U32([byte[]]$b, [int]$o) { [int]$b[$o] * 16777216 + [int]$b[$o + 1] * 65536 + [int]$b[$o + 2] * 256 + [int]$b[$o + 3] }

# GDI picks a font by its name ID 1 (family), so read it from the file instead of assuming it.
function Get-FontFamily([string]$path) {
    $b = [System.IO.File]::ReadAllBytes($path)
    $n = U16 $b 4
    $name = $null
    for ($i = 0; $i -lt $n; $i++) {
        $o = 12 + 16 * $i
        if ([System.Text.Encoding]::ASCII.GetString($b, $o, 4) -eq 'name') { $name = U32 $b ($o + 8) }
    }
    if ($null -eq $name) { throw "no name table in $path" }
    $count = U16 $b ($name + 2)
    $strings = $name + (U16 $b ($name + 4))
    for ($i = 0; $i -lt $count; $i++) {
        $r = $name + 6 + 12 * $i
        if ((U16 $b $r) -eq 3 -and (U16 $b ($r + 6)) -eq 1) {
            return [System.Text.Encoding]::BigEndianUnicode.GetString($b, $strings + (U16 $b ($r + 10)), (U16 $b ($r + 8)))
        }
    }
    throw "no Windows family name in $path"
}

$fontFiles = [ordered]@{
    Solid   = 'Font Awesome 7 Free-Solid-900.otf'
    Regular = 'Font Awesome 7 Free-Regular-400.otf'
    Brands  = 'Font Awesome 7 Brands-Regular-400.otf'
}
$destFonts = [ordered]@{ Solid = 'fa-solid-900.otf'; Regular = 'fa-regular-400.otf'; Brands = 'fa-brands-400.otf' }

$pkg = (Resolve-Path $Release).Path
$meta = Join-Path $pkg 'metadata\icons.json'
$version = (Split-Path -Leaf $pkg) -replace '^fontawesome-free-', '' -replace '-desktop$', ''
if ($version -notmatch '^[0-9]+(\.[0-9]+)+$') { throw "cannot tell the Font Awesome version from the folder name '$pkg' (expected fontawesome-free-<version>-desktop)" }

$faces = @{}
foreach ($k in $fontFiles.Keys) {
    $p = Join-Path $pkg ('otfs\' + $fontFiles[$k])
    if (-not (Test-Path $p)) { throw "missing $p" }
    $faces[$k] = Get-FontFamily $p
}

$icons = Get-Content $meta -Raw | ConvertFrom-Json

# Code points taken by any icon (also Pro-only ones, to stay clear of them): aliases are allocated outside.
$used = New-Object 'System.Collections.Generic.HashSet[int]'
foreach ($p in $icons.PSObject.Properties) { [void]$used.Add([Convert]::ToInt32($p.Value.unicode, 16)) }
$nextAlias = 0xF8FF
function New-AliasCodePoint {
    while ($used.Contains($script:nextAlias)) { $script:nextAlias-- }
    $cp = $script:nextAlias
    [void]$used.Add($cp)
    $script:nextAlias--
    if ($cp -lt 0xE000) { throw 'ran out of Private Use code points' }
    return $cp
}

$styleIndex = @{ solid = 0; regular = 1; brands = 2 }
$order = @('solid', 'brands', 'regular')

$names = [System.Collections.Generic.SortedDictionary[string, int]]::new([System.StringComparer]::Ordinal)
$rows = New-Object System.Collections.Generic.List[object]
$skipped = 0
foreach ($p in ($icons.PSObject.Properties | Sort-Object { $_.Name } -CaseSensitive)) {
    $v = $p.Value
    $free = @($v.free)
    if ($free.Count -eq 0) { continue }
    $cp = [Convert]::ToInt32($v.unicode, 16)
    if ($cp -lt 0xE000 -or $cp -gt 0xF8FF) { $skipped++; continue }
    $primary = $order | Where-Object { $free -contains $_ } | Select-Object -First 1
    $regular = 0
    $brands = 0
    if ($free -contains 'regular' -and $primary -ne 'regular') { $regular = New-AliasCodePoint }
    if ($free -contains 'brands' -and $primary -ne 'brands') { $brands = New-AliasCodePoint }
    $idx = $rows.Count
    $rows.Add([pscustomobject]@{ Name = $p.Name; Cp = $cp; Style = $styleIndex[$primary]; Regular = $regular; Brands = $brands })
    $all = @($p.Name) + @($v.aliases.names)
    foreach ($n in $all) {
        if ($n -and -not $names.ContainsKey($n)) { $names[$n] = $idx }
    }
}

$sb = New-Object System.Text.StringBuilder
[void]$sb.Append("// Generated by tools/generate-fontawesome.ps1 from Font Awesome Free $version. Do not edit.$eol")
[void]$sb.Append("// Font Awesome Free by @fontawesome - https://fontawesome.com$eol")
[void]$sb.Append("// License - https://fontawesome.com/license/free (Icons: CC BY 4.0, Fonts: SIL OFL 1.1, Code: MIT)$eol")
[void]$sb.Append("// Copyright Fonticons, Inc. The fonts are embedded unmodified; see skeet/third_party/fontawesome/LICENSE.txt.$eol$eol")
[void]$sb.Append("constexpr char kVersion[] = `"$version`";$eol")
[void]$sb.Append("constexpr wchar_t kFaceSolid[] = L`"$($faces.Solid)`";$eol")
[void]$sb.Append("constexpr wchar_t kFaceRegular[] = L`"$($faces.Regular)`";$eol")
[void]$sb.Append("constexpr wchar_t kFaceBrands[] = L`"$($faces.Brands)`";$eol$eol")
[void]$sb.Append("// One entry per icon. primary: its real code point, in primary_style (0 solid, 1 regular, 2 brands).$eol")
[void]$sb.Append("// regular / brands: alias code points for the other free styles, 0 when the icon has no such style.$eol")
[void]$sb.Append("constexpr Icon kIcons[] = {$eol")
foreach ($r in $rows) {
    [void]$sb.Append(('    {{ 0x{0:X4}, {1}, 0x{2:X4}, 0x{3:X4} }}, // {4}{5}' -f $r.Cp, $r.Style, $r.Regular, $r.Brands, $r.Name, $eol))
}
[void]$sb.Append("};$eol$eol")
[void]$sb.Append("// Names (icon names and their aliases), sorted for binary search; index points into kIcons.$eol")
[void]$sb.Append("constexpr Name kNames[] = {$eol")
foreach ($kv in $names.GetEnumerator()) {
    [void]$sb.Append(('    {{ "{0}", {1} }},{2}' -f $kv.Key, $kv.Value, $eol))
}
[void]$sb.Append("};$eol")

$inc = Join-Path $Root 'skeet\fontawesome_icons.inc'
[System.IO.File]::WriteAllText($inc, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

$dest = Join-Path $Root 'skeet\third_party\fontawesome'
New-Item -ItemType Directory -Force $dest | Out-Null
foreach ($k in $fontFiles.Keys) {
    Copy-Item (Join-Path $pkg ('otfs\' + $fontFiles[$k])) (Join-Path $dest $destFonts[$k]) -Force
}
Copy-Item (Join-Path $pkg 'LICENSE.txt') (Join-Path $dest 'LICENSE.txt') -Force

$readme = @"
# Font Awesome Free $version

These fonts are embedded into ``skeet.dll`` (see ``skeet/fontawesome.rc``) and drive ``skeetsdk.get_icon``.

- Source: the official ``fontawesome-free-$version-desktop.zip`` from https://github.com/FortAwesome/Font-Awesome/releases/tag/$version
- Zip SHA-256: ``$ZipSha256``
- Files: ``fa-solid-900.otf``, ``fa-regular-400.otf``, ``fa-brands-400.otf`` are byte for byte the ``otfs/`` files of that release.
- ``LICENSE.txt`` is Font Awesome's licence: icons CC BY 4.0, fonts SIL OFL 1.1, code MIT.
- ``skeet/fontawesome_icons.inc`` is generated from the release's ``metadata/icons.json`` by ``tools/generate-fontawesome.ps1``.

Font Awesome Free by @fontawesome - https://fontawesome.com
"@
[System.IO.File]::WriteAllText((Join-Path $dest 'README.md'), ($readme -replace "`r?`n", $eol) + $eol, (New-Object System.Text.UTF8Encoding($false)))

"Font Awesome Free $version"
"  icons in table      : $($rows.Count)   (skipped $skipped ASCII icons)"
"  names (with aliases): $($names.Count)"
"  regular aliases     : $(@($rows | Where-Object { $_.Regular }).Count)   brands aliases: $(@($rows | Where-Object { $_.Brands }).Count)"
"  faces               : solid='$($faces.Solid)' regular='$($faces.Regular)' brands='$($faces.Brands)'"
"  wrote $inc ($([math]::Round((Get-Item $inc).Length / 1KB)) KB) and fonts into $dest"
