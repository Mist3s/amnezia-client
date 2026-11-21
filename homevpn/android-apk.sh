#!/bin/bash
# Signed HomeVPN APK. Usage: homevpn/android-apk.sh <min sdk: 28|30> <abi> <build number>
# The version comes from homevpn/version.sh.
# Needs HOMEVPN_KEYSTORE (PKCS12 file kept outside the repository) and HOMEVPN_KEYSTORE_PASS_FILE.
set -e

MIN_SDK="${1:?min sdk: 28 or 30}"
ABI="${2:?abi: arm64-v8a or armeabi-v7a}"
BUILD_NUMBER="${3:?build number}"
: "${HOMEVPN_KEYSTORE:?path to the signing keystore}"
: "${HOMEVPN_KEYSTORE_PASS_FILE:?path to the file with the keystore password}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$("$ROOT/homevpn/version.sh" "$BUILD_NUMBER")"

export APP_ANDROID_MIN_SDK="$MIN_SDK"
if [ "$MIN_SDK" -ge 30 ]; then
    : "${QT_VERSION:=6.10.3}"
    MAX_SDK=null
    export APP_ANDROID_VERSION_CODE_OFFSET=$(( BUILD_NUMBER * 2 + 1 ))
else
    : "${QT_VERSION:=6.10.1}"
    MAX_SDK=29
    export APP_ANDROID_MAX_SDK="$MAX_SDK" APP_ANDROID_VERSION_CODE_OFFSET=$(( BUILD_NUMBER * 2 ))
fi
export QT_VERSION
export ANDROID_APPLICATION_ID="dev.homevpn.app"
export EXTRA_CMAKE_ARGS="-DCLIENT_APPLICATION_NAME=HomeVPN -DCLIENT_ANDROID_APP_NAME=ЧихНет -DCLIENT_ANDROID_RES_OVERLAY_DIR=$ROOT/homevpn/android-res -DCLIENT_ANDROID_PACKAGE=$ANDROID_APPLICATION_ID -DAMNEZIAVPN_VERSION=$VERSION"
export QT_ANDROID_KEYSTORE_PATH="$HOMEVPN_KEYSTORE"
export QT_ANDROID_KEYSTORE_ALIAS="${HOMEVPN_KEYSTORE_ALIAS:-homevpn}"
export QT_ANDROID_KEYSTORE_STORE_PASS="$(cat "$HOMEVPN_KEYSTORE_PASS_FILE")"
export JOBS="${JOBS:-6}"

BUILD_DIR="${BUILD_DIR:-$ROOT/deploy/build-homevpn/android-$ABI-api$MIN_SDK}"
OUT_DIR="${OUT_DIR:-$ROOT/deploy/build-homevpn/out}"
APK_NAME="HomeVPN_${VERSION}_${ABI}_api${MIN_SDK}.apk"

echo "HomeVPN $VERSION, $ABI, min sdk $MIN_SDK, Qt $QT_VERSION"
cd "$ROOT"
nice -n 19 deploy/build.sh -t android --sign --abi "$ABI" -b "$BUILD_DIR"

mkdir -p "$OUT_DIR"
cp "$BUILD_DIR/client/android-build/build/outputs/apk/android-build-release-signed.apk" "$OUT_DIR/$APK_NAME"
printf '{"abi": "%s", "min_sdk": %s, "max_sdk": %s}\n' "$ABI" "$MIN_SDK" "$MAX_SDK" > "$OUT_DIR/$APK_NAME.json"
echo "$OUT_DIR/$APK_NAME"
