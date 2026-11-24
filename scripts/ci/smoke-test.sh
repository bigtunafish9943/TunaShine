#!/bin/bash
# Smoke test for a packaged Windows build, run by CI in Git Bash on the build machine.
#
# It cannot test streaming (the build machine has no GPU, display driver or client), but it
# does prove that the packaged program starts, serves its web interface, accepts a login,
# and that the settings and actions TunaStream adds are wired up. It also exercises the
# installer script that lets TunaStream sit next to Sunshine or Apollo.
#
# usage: smoke-test.sh <path to the portable .zip> <path to repo root> <expected version>

set -u
zip_path=$(cygpath -u "$1")
repo_root=$(cygpath -u "$2")
expected_version=$3

failures=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; failures=$((failures + 1)); }
expect() {  # expect <description> <actual> <wanted substring>
  if [[ "$2" == *"$3"* ]]; then pass "$1"; else fail "$1 (wanted '$3', got '${2:0:300}')"; fi
}

work=$(mktemp -d)
echo "Working in $work"

############################################################
# 1. Side-by-side install script
############################################################
echo "== Side-by-side install script =="
root="$work/coexist"
mkdir -p "$root/scripts" "$root/config"
cp "$repo_root/src_assets/windows/misc/migration/coexist-setup.bat" "$root/scripts/"
bat_win=$(cygpath -w "$root/scripts/coexist-setup.bat")
conf="$root/config/tunastream.conf"
run_bat() { cmd.exe //c "$bat_win" > "$work/bat.out" 2>&1; echo "exit=$?" >> "$work/bat.out"; }

# No other host installed: nothing should be written
sc.exe delete SunshineService > /dev/null 2>&1
sc.exe delete ApolloService > /dev/null 2>&1
run_bat
expect "script exits cleanly with no other host" "$(cat "$work/bat.out")" "exit=0"
if [ -f "$conf" ]; then fail "config left alone with no other host (file was created)"; else pass "config left alone with no other host"; fi

# Pretend Sunshine is installed, with existing settings that do not end in a newline
sc.exe create SunshineService binPath= "C:\\Windows\\System32\\svchost.exe" start= demand > "$work/sc.out" 2>&1 \
  || fail "could not create a stand-in SunshineService: $(cat "$work/sc.out")"
printf 'min_log_level = 2' > "$conf"
run_bat
conf_text=$(tr -d '\r' < "$conf")
expect "script exits cleanly with Sunshine present" "$(cat "$work/bat.out")" "exit=0"
expect "own port is set" "$conf_text" "port = 48989"
expect "host is given a distinct name" "$conf_text" "tunastream_name = ${COMPUTERNAME}-TunaStream"
if grep -q -x "min_log_level = 2" <<< "$conf_text"; then pass "existing setting kept on its own line"; else fail "existing setting kept on its own line (file: $conf_text)"; fi

# Running it again must not add a second port
run_bat
count=$(tr -d '\r' < "$conf" | grep -c "^port")
if [ "$count" = "1" ]; then pass "running twice sets the port once"; else fail "running twice sets the port once (found $count)"; fi

# A port the user already chose must be respected
printf 'port = 50000\r\n' > "$conf"
run_bat
conf_text=$(tr -d '\r' < "$conf")
if [[ "$conf_text" == *"48989"* ]]; then fail "user's own port is respected (file: $conf_text)"; else pass "user's own port is respected"; fi

sc.exe delete SunshineService > /dev/null 2>&1

############################################################
# 2. The packaged program starts and serves its web interface
############################################################
echo "== Packaged program =="
app="$work/app"
mkdir -p "$app"
unzip -q "$zip_path" -d "$app" || fail "could not unzip $zip_path"
exe=$(find "$app" -iname "tunastream.exe" | head -n 1)
if [ -z "$exe" ]; then
  fail "tunastream.exe found in the portable zip"
  echo "RESULT: $failures failure(s)"
  exit 1
fi
pass "tunastream.exe found in the portable zip"
app_dir=$(dirname "$exe")

(cd "$app_dir" && ./tunastream.exe > "$work/run.log" 2>&1 &)

base="https://localhost:47990"
up=""
for _ in $(seq 1 90); do
  code=$(curl -sk -o /dev/null -w "%{http_code}" --max-time 3 "$base/api/configLocale" 2>/dev/null)
  if [ "$code" = "200" ]; then up=1; break; fi
  sleep 2
done
if [ -z "$up" ]; then
  fail "web interface answers within 3 minutes"
  echo "--- program output ---"; tail -n 40 "$work/run.log"
  taskkill //F //IM tunastream.exe > /dev/null 2>&1
  echo "RESULT: $failures failure(s)"
  exit 1
fi
pass "web interface answers"

json() { python -c "import json,sys; d=json.load(sys.stdin); print(d.get('$1'))" 2>/dev/null; }
jar="$work/cookies.txt"
post() { curl -sk --max-time 20 -b "$jar" -c "$jar" -H "Content-Type: application/json" -X POST -d "$2" "$base$1"; }
get() { curl -sk --max-time 20 -b "$jar" -c "$jar" "$base$1"; }

# First run sends you to account creation
redirect=$(curl -sk -o /dev/null -w "%{http_code} %{redirect_url}" --max-time 20 "$base/")
expect "first visit is sent to account creation" "$redirect" "/welcome"
expect "account creation page is TunaStream's" "$(get /welcome)" "TunaStream"

expect "account can be created" "$(post /api/password '{"newUsername":"ci","newPassword":"ci-Pass-123","confirmNewPassword":"ci-Pass-123"}' | json status)" "True"
# Nothing is served without logging in, and a wrong password is refused
expect "config is refused before login" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 "$base/api/config")" "401"
expect "wrong password is refused" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 -H "Content-Type: application/json" -X POST -d '{"username":"ci","password":"wrong"}' "$base/api/login")" "401"
# A successful login answers 200 with a session cookie and no body
expect "login works" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 -c "$jar" -H "Content-Type: application/json" -X POST -d '{"username":"ci","password":"ci-Pass-123"}' "$base/api/login")" "200"
if grep -q "auth" "$jar" 2>/dev/null; then pass "login sets a session cookie"; else fail "login sets a session cookie"; fi

config=$(get /api/config)
expect "config reports Windows" "$(json platform <<< "$config")" "windows"
expect "config reports this build's version" "$(json version <<< "$config")" "$expected_version"
if [[ "$config" == *"vdisplayStatus"* ]]; then pass "virtual display status is reported"; else fail "virtual display status is reported"; fi

# Pages
for page in "" pin apps config troubleshooting password setup; do
  body=$(get "/$page")
  if [[ "$body" == *"<title>TunaStream</title>"* && "$body" == *'id="app"'* ]]; then pass "page /$page loads"; else fail "page /$page loads (${#body} bytes)"; fi
done

# Branding files the pages and the tray rely on
for file in images/tunastream.ico images/logo-tunastream-45.png assets/css/tunastream.css; do
  expect "file /$file is served" "$(curl -sk -o /dev/null -w "%{http_code}" --max-time 20 -b "$jar" "$base/$file")" "200"
done

# TunaStream additions
expect "rumble test answers, with nobody streaming" "$(post /api/rumble-test '{}' | json clients)" "0"
expect "app list is served" "$(get /api/apps)" "Desktop"
status=$(get /api/status)
expect "status answers" "$(json status <<< "$status")" "True"
expect "status lists no controllers with nobody streaming" "$(python -c "import json,sys; print(len(json.load(sys.stdin)['controllers']))" <<< "$status" 2>/dev/null)" "0"

expect "setup is not yet done on a fresh install" "$(get /api/config | json setup_complete)" "None"
expect "settings can be saved" "$(post /api/config '{"end_session_on_disconnect":"enabled","virtual_display_primary":"disabled","headless_mode":"disabled"}' | json status)" "True"
config=$(get /api/config)
expect "saved setting is read back: end session on disconnect" "$(json end_session_on_disconnect <<< "$config")" "enabled"
expect "saved setting is read back: virtual display primary" "$(json virtual_display_primary <<< "$config")" "disabled"
expect "setup can be marked done" "$(post /api/config '{"setup_complete":"enabled"}' | json status)" "True"
expect "setup done is read back" "$(get /api/config | json setup_complete)" "enabled"

expect "end session answers with nothing running" "$(post /api/apps/close '{}' | json status)" "True"

# The updater: this build is newer than any published release, so a real check against GitHub must
# find the latest release, read it and decide there is nothing to install.
expect "updater is idle at start" "$(get /api/update | json stage)" "idle"
expect "updater starts a check" "$(post /api/update '{}' | json started)" "True"
stage=checking
for _ in $(seq 1 30); do
  stage=$(get /api/update | json stage)
  [ "$stage" = checking ] || break
  sleep 2
done
expect "updater finds no newer release than this build" "$stage $(get /api/update | json error)" "up_to_date"

# The program must still be alive after all that
if tasklist //FI "IMAGENAME eq tunastream.exe" 2>/dev/null | grep -qi "tunastream.exe"; then pass "program is still running"; else fail "program is still running"; fi

echo "--- log lines at warning level or above (expected on a machine with no GPU or drivers) ---"
log=$(find "$app_dir" -name "tunastream.log" | head -n 1)
[ -n "$log" ] && grep -E "Warning:|Error:|Fatal:" "$log" | cut -c1-220 | head -n 25

taskkill //F //IM tunastream.exe > /dev/null 2>&1

echo "RESULT: $failures failure(s)"
[ "$failures" = "0" ]
