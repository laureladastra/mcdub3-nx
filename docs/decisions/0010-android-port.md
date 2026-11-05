# 10. An Android port, first on the Switch under Android

Date: 2026-11-04

## Context

Both games run on the Switch (Horizon, libnx), Linux, macOS and Windows.
Android is the next host: the translated game is plain C (no JIT), already
built for 64-bit ARM, and the runtime's host layer is SDL2, which runs on
Android. The renderer (`xboxrecomp/src/nv2a_gl/`) is written for desktop
OpenGL 3.3 core; most Android GPUs offer OpenGL ES 3.2 only.

The baseline test device is a Switch running Android 15 (switchroot): the
same Tegra X1 as the Horizon build, 4 GB of RAM, with NVIDIA's own Android
driver, which offers OpenGL ES 3.2 and Vulkan (and, unlike phones, desktop
OpenGL through EGL).

## Decision

- **Android is a host of its own** (`ANDROID`): an `android/` Gradle
  project on SDL2's Android template, the game built with the NDK as
  `libmain.so`, landscape and full screen. One APK per game, as one NRO
  per game on the Switch. The DUB Edition first.
- **Sideloaded APKs only**, never the Play Store. The user supplies the
  disc: the extracted files in the app's folder (`Android/data/<app>/files/
  game-<edition>/`), the settings file and logs beside them, as on the
  Switch's SD card.
- **Renderer: OpenGL ES 3.2 from the start**, as a path of its own in the
  same renderer, chosen when the context is GLES: shader versions and
  precision, BGRA uploads, S3TC through the existing CPU decoder where the
  driver lacks it, `EXT_buffer_storage`, `EXT_depth_clamp`; ES 3.1 where
  3.2 is refused (`#version 310 es`, border clamp as edge clamp without
  the extension; the Android emulator on a Mac offers 3.1 through ANGLE,
  which also counts `gl_VertexID` as an attribute, so such programs link
  without per-draw matrices). It is what
  almost every Android GPU offers (Adreno, Mali, Xclipse, PowerVR); the
  baseline device offers it too, so phones are covered by what is tested
  there. Desktop OpenGL over EGL (NVIDIA Tegra only) is not pursued; Mesa's
  Zink is kept in mind should a GLES path fall short. Neither changes the
  Switch or desktop builds.
- **Controls**: game controllers through SDL (the existing
  `xinput_device.c` path; Joy-Cons and Bluetooth pads); a keyboard as on
  the desktop (`input/sdl_keyboard.c`), its keys handed to SDL as keys by
  `MainActivity` (SDL takes a keyboard that also reports a d-pad, as the
  emulator's and many real ones do, for a joystick: the arrows became a
  d-pad), and a stick-less "joystick" is not opened as a pad; Back is the
  Xbox's Back, not "leave". An on-screen pad (`input/touch_pad.c`) drawn
  by the renderer over the picture feeds the emulated pad the same way:
  a stick with the d-pad as a ring round it (up / down also gas and
  brake), A/B/X/Y with White and Black on a diagonal beside them as on
  the original Xbox controller, Back and Start at the bottom; one
  translucent grey, its labels in a stroke font of its own; hidden
  while a controller is connected or after a key, back at a touch --
  or always, or never: the start screen's Settings (Joy-Cons on the
  Switch's rails always count as connected), handed to the game as
  `RECOMP_TOUCH_PAD`; shown anyway, it adds to the controller's input;
  `RECOMP_TOUCH_PAD` (0 off, 1 always, also on the desktop with the
  mouse), `RECOMP_TOUCH_PAD_ALPHA`, `RECOMP_TOUCH_PAD_SCALE`.
- **The game folder is chosen on a start screen** (`LauncherActivity`,
  before SDL): the first time an onboarding (storage access, then the
  folder through Android's folder picker); afterwards the list of folders
  and Play, with Add folder / Remove under Settings, the choice
  remembered. Any edition's disc (game-usa/, game-eur/, ...): the game's
  boot check decides. Found by themselves: game*/ in the app's folder and
  in `mcdub3-nx/` and the Switch build's `switch/mcdub3-nx/` on internal
  storage and every SD card (one card for both under switchroot). Reading
  outside the app's folder takes "All files access" (sideloaded only, so
  allowed). `mcdub3-nx/mcdub3_env.txt` holds settings; `MC3_GAME_DIR`
  there is the fallback when the game is started without the screen.
- **Icon**: an adaptive launcher icon from `APP_ICON` (tools/app_icon.py
  android), made at build time; SDL's without artwork.
- The NDK and SDK live in `work-<game>/tools/` like the other toolchains
  (nothing installed outside the repository without asking).
- **Builds**: `make android` (a debug APK) and `make android-release`
  (`<app>-<version>.apk`, e.g. `mcdub3-1.0.0.apk`, into
  `work-<game>/android/`; tools/android_build.sh). The version is
  APP_VERSION, the version code derived from it (1.2.3 -> 10203). A
  release is signed with our own key, `keys/android-release.jks` and its
  `.properties` (made the first time; personal, ignored by git like the
  console keys): every update must carry the same key or Android refuses
  it, so it is kept safe. The native code is optimised in every variant
  (the debug variant built the runtime at -O0), the lifted code at -O2
  as on the Switch.
- **Version and CI**: the app's own `ANDROID_VERSION` (0.1.0, a beta),
  apart from the games'. `android-build.yml` builds a beta on every push
  to `main` that changes `android/` and a release by hand, on the
  self-hosted runner with the disc; the runner signs with a copy of the
  release key in `MC3_ANDROID_KEYS` (its own folder, never GitHub). The
  Switch release publishes the APK beside the NROs when asked
  ("android"; release builds only) (CONTRIBUTING.md, Releases (Android)).

## Consequences

- The Switch under Android is the first test device; with GLES from the
  start, phones need no second renderer path.
- A third packaging path to keep in step (NRO, desktop apps, APK); CI
  builds APKs only once the port boots.
- The game's data stays outside the APK, as on every other host.
