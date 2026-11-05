#!/usr/bin/env bash
# The APK (ADR 0010): tools/android_build.sh [debug|release]
#
#   debug     work-<game>/android/<app>-<version>-debug.apk, signed with
#             Gradle's debug key (this machine's)
#   release   work-<game>/android/<app>-<version>.apk (mcdub3-1.0.0.apk),
#             signed with the release key in keys/ -- made here the first
#             time (keys/android-release.jks + .properties; personal, ignored
#             by git). Keep a copy: an update signed with another key will
#             not install over the old one.
#
# The version is APP_VERSION in config/<game>/app.env. The Android SDK, NDK
# and SDL2 source live in work-dub/tools/ (android-sdk/, SDL2-2.32.10/),
# shared by both games.
set -euo pipefail
. "$(dirname "$0")/env.sh"
mc3_app_env

kind=${1:-release}
case "$kind" in debug|release) ;; *) mc3_fail "usage: $0 [debug|release]" ;; esac

tools="$MC3_REPO/work-dub/tools"
export ANDROID_HOME="${ANDROID_HOME:-$tools/android-sdk}"
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$tools/gradle-home}"
[ -d "$ANDROID_HOME/ndk" ] || mc3_fail "no Android SDK/NDK in $ANDROID_HOME (sdkmanager: \"platforms;android-35\" \"build-tools;35.0.0\" \"ndk;30.0.16248370\" \"cmake;3.31.6\" platform-tools)"
[ -d "$tools/SDL2-2.32.10" ] || mc3_fail "no SDL2 source in $tools/SDL2-2.32.10 (libsdl.org, SDL2-2.32.10.tar.gz)"

app=$([ "$MC3_GAME" = remix ] && echo mcdub3remix || echo mcdub3)
version="${APP_VERSION:?no APP_VERSION in config/$MC3_GAME/app.env}"

if [ "$kind" = release ]; then
    props="$MC3_REPO/keys/android-release.properties"
    jks="$MC3_REPO/keys/android-release.jks"
    if [ ! -f "$props" ]; then
        [ ! -f "$jks" ] || mc3_fail "$jks exists without its .properties: restore it (the passwords are there)"
        mkdir -p "$MC3_REPO/keys"
        pass=$(openssl rand -hex 16)
        keytool -genkeypair -keystore "$jks" -storetype PKCS12 -alias mcdub3 \
            -keyalg RSA -keysize 4096 -validity 36500 -storepass "$pass" -keypass "$pass" \
            -dname "CN=${APP_AUTHOR:-mcdub3-nx}" >/dev/null 2>&1 \
            || mc3_fail "keytool could not make $jks"
        printf 'storeFile=keys/android-release.jks\nstorePassword=%s\nkeyAlias=mcdub3\nkeyPassword=%s\n' \
            "$pass" "$pass" > "$props"
        chmod 600 "$jks" "$props"
        echo "  release key made: keys/android-release.jks (+ .properties). Keep a copy:"
        echo "  updates signed with another key will not install over this one."
    fi
fi

task=$([ "$kind" = release ] && echo assembleRelease || echo assembleDebug)
(cd "$MC3_REPO/android" && ./gradlew --console=plain "$task" -PMC3_GAME="$MC3_GAME")

name="$app-$version$([ "$kind" = release ] || echo -debug).apk"
src="$MC3_REPO/android/app/build/outputs/apk/$kind/$name"
[ -f "$src" ] || mc3_fail "no $src after the build"
mkdir -p "$MC3_WORK/android"
cp "$src" "$MC3_WORK/android/$name"
echo "  APK: ${MC3_WORK#$MC3_REPO/}/android/$name ($(du -h "$src" | cut -f1 | tr -d ' '), version $version)"
