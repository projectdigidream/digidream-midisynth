#!/usr/bin/env bash
#
# Build the DigidreamSynth Android app (Capacitor).
#
#   ./scripts/package_android.sh               debug APK
#   ./scripts/package_android.sh --release      unsigned release APK
#   ./scripts/package_android.sh --stage-only   just refresh mobile/www/
#
# Same static payload as the web and desktop builds -- Capacitor serves it
# from https://localhost inside the WebView, so AudioWorklet and WASM get the
# secure context they need.
#
set -euo pipefail
cd "$(dirname "$0")/.."

MODE=${1:-debug}

if [[ ! -f tool/dist/midisynth.mjs ]]; then
  echo "tool/dist/midisynth.mjs missing -- run ./scripts/build_wasm.sh first" >&2
  exit 1
fi

echo "staging static payload -> mobile/www/"
./scripts/package_web.sh mobile/www > /dev/null
rm -f mobile/www.tar.gz mobile/www/serve.js
# Capacitor always loads index.html from the web root; hand it straight to
# the DAW. A redirect, not a copy -- daw.html's URLs are relative to tool/.
cat > mobile/www/index.html <<'HTML'
<!DOCTYPE html>
<meta charset="utf-8">
<meta http-equiv="refresh" content="0; url=tool/daw.html">
<link rel="icon" href="data:,">
<title>DigidreamSynth</title>
<body style="margin:0;background:#14161a"></body>
HTML
du -sh mobile/www | sed 's/^/  /'

[[ "$MODE" == "--stage-only" ]] && { echo "staged."; exit 0; }

cd mobile
[[ -d node_modules ]] || npm install --no-fund --no-audit
if [[ ! -d android ]]; then
  npx cap add android
  # android/ is generated, so the launcher icons have to be too.
  npx @capacitor/assets generate --android \
      --iconBackgroundColor '#14161a' --splashBackgroundColor '#14161a'
fi
npx cap sync android
# The version from VERSION (android/ is generated, so it is set on every build):
# versionName as written, versionCode 0.2.0 -> 200, 1.2.3 -> 10203.
VER="$(cat ../VERSION)"
CODE="$(echo "$VER" | awk -F. '{print $1*10000 + $2*100 + $3}')"
sed -i.bak -e "s/versionCode [0-9]*/versionCode $CODE/" -e "s/versionName \"[^\"]*\"/versionName \"$VER\"/" android/app/build.gradle && rm -f android/app/build.gradle.bak

: "${ANDROID_HOME:=$HOME/Library/Android/sdk}"
export ANDROID_HOME
[[ -d "$ANDROID_HOME" ]] || { echo "Android SDK not found at $ANDROID_HOME (set ANDROID_HOME)" >&2; exit 1; }

# Capacitor 7's Gradle plugins need JDK 21; Android Studio ships one, so
# borrow it rather than making the machine's default java match.
jdk_major() { "$1" -version 2>&1 | head -1 | sed -E 's/.*"([0-9]+).*/\1/'; }
if [[ $(jdk_major "${JAVA_HOME:+$JAVA_HOME/bin/}java") -lt 21 ]]; then
  JBR="/Applications/Android Studio.app/Contents/jbr/Contents/Home"
  [[ -x "$JBR/bin/java" ]] || { echo "need JDK 21 (brew install openjdk@21) or Android Studio" >&2; exit 1; }
  export JAVA_HOME="$JBR"
  echo "using JDK 21 from Android Studio"
fi

cd android
if [[ "$MODE" == "--release" ]]; then
  ./gradlew assembleRelease
  echo "APK: mobile/android/app/build/outputs/apk/release/"
else
  ./gradlew assembleDebug
  echo "APK: mobile/android/app/build/outputs/apk/debug/app-debug.apk"
fi
