# Build a release: both firmware variants with marker bytes instead of the stock-derived data
# (PixArt SROM, HyperX LED tables), without mouse\local.conf.
#   .\build-release.ps1   -> ..\release\v<version>\
# Users complete the files with tools\make_firmware.py and their own flash dump.
$fw = $PSScriptRoot
$f = $fw -replace '\\','/'
$a = "$f/mouse"
$m = "$a/mcuboot"
$ver = (Select-String -Path "$m/app.conf" -Pattern 'SIGN_VERSION="([0-9.]+)').Matches[0].Groups[1].Value
$out = Join-Path (Split-Path $fw) "release\v$ver"

function Build($dir, $extra) {
    $cmd = "set ZEPHYR_BASE=C:\ncs\v3.4.1\zephyr&& west build -p always -b pulsedart -d $dir . -- -DBOARD_ROOT=$f $extra"
    & C:\ncs\bin\nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 --chdir (Join-Path $fw "mouse") -- cmd /c "$cmd 2>&1" |
        Select-String -Pattern 'error|FAILED|FLASH:|Completed'
    if ($LASTEXITCODE) { throw "build $dir failed" }
}

# 1. behind the stock boot code (0x50000)
Build "build-release" "-DEXTRA_CONF_FILE=$a/release.conf"
# 2. MCUboot + signed image (slot 0 at 0x10000)
Build "build-release-mcuboot" ("-DSB_CONF_FILE=$m/sysbuild.conf " +
    "-DEXTRA_CONF_FILE=`"$m/app.conf;$a/release.conf`" " +
    "-DEXTRA_DTC_OVERLAY_FILE=`"$m/mcuboot.overlay`" " +
    "-Dmcuboot_EXTRA_DTC_OVERLAY_FILE=$m/mcuboot.overlay " +
    "-Dmcuboot_EXTRA_ZEPHYR_MODULES=$f/mcuboot_hooks")

New-Item -ItemType Directory -Force $out | Out-Null
Copy-Item "$fw\mouse\build-release\mouse\zephyr\zephyr.bin" "$out\pulsedart-stockboot.bin"
Copy-Item "$fw\mouse\build-release-mcuboot\mcuboot\zephyr\zephyr.bin" "$out\pulsedart-mcuboot.bin"
Copy-Item "$fw\mouse\build-release-mcuboot\mouse\zephyr\zephyr.signed.bin" "$out\pulsedart-app.signed.bin"
Get-ChildItem $out -Filter *.bin | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content -Encoding ascii "$out\SHA256SUMS"
Get-Content "$out\SHA256SUMS"
"release written to $out"
