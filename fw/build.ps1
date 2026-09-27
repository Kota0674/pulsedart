# Build a firmware app for the Pulsedart board with the installed NCS toolchain.
# usage: .\build.ps1 [app-dir] [extra west args...]
# If <app-dir>\local.conf exists (personal settings, not in git) it is added as an
# extra Kconfig fragment.
param([string]$App = "app", [Parameter(ValueFromRemainingArguments)]$Rest)
$fw = $PSScriptRoot
$appDir = Join-Path $fw $App
$extra = ""
if (Test-Path (Join-Path $appDir "local.conf")) { $extra = " -DEXTRA_CONF_FILE=local.conf" }
$cmd = "set ZEPHYR_BASE=C:\ncs\v3.4.1\zephyr&& west build -p auto -b pulsedart -d build $Rest . -- -DBOARD_ROOT=$($fw -replace '\\','/')$extra"
& C:\ncs\bin\nrfutil.exe sdk-manager toolchain launch --ncs-version v3.4.1 --chdir $appDir -- cmd /c "$cmd 2>&1"
exit $LASTEXITCODE
