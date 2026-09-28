# Build the MCUboot variant of the mouse firmware (MCUboot at 0x0 with USB recovery,
# signed app in slot 0, USB DFU updates into slot 1).
#   .\build-mcuboot.ps1          -> mouse\build-mcuboot\
#   .\build-mcuboot.ps1 -Debug   -> mouse\build-mcuboot-debug\ (log also on a USB COM port)
param([switch]$Debug)
$fw = $PSScriptRoot
$app = Join-Path $fw "mouse"
$a = $app -replace '\\','/'
$m = "$a/mcuboot"
# personal settings (mouse\local.conf, not in git) go after the MCUboot app fragment
$conf = "$m/app.conf"
if (Test-Path (Join-Path $app "local.conf")) { $conf = "$conf;$a/local.conf" }
$overlay = "$m/mcuboot.overlay"
$dir = "build-mcuboot"
if ($Debug) {
    $conf = "$conf;$a/debug.conf"
    $overlay = "$overlay;$a/debug.overlay"
    $dir = "build-mcuboot-debug"
}
$cmd = "set ZEPHYR_BASE=C:\ncs\v3.4.1\zephyr&& west build -p auto -b pulsedart -d $dir . -- " +
       "-DBOARD_ROOT=$($fw -replace '\\','/') " +
       "-DSB_CONF_FILE=$m/sysbuild.conf " +
       "-DEXTRA_CONF_FILE=`"$conf`" " +
       "-DEXTRA_DTC_OVERLAY_FILE=`"$overlay`" " +
       "-Dmcuboot_EXTRA_DTC_OVERLAY_FILE=$m/mcuboot.overlay " +
       "-Dmcuboot_EXTRA_ZEPHYR_MODULES=$($fw -replace '\\','/')/mcuboot_hooks"
& C:\ncs\bin\nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 --chdir $app -- cmd /c "$cmd 2>&1"
exit $LASTEXITCODE
