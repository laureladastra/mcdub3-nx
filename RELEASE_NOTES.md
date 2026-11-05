# Release notes

## v1.1.0

**Headlight beams in both games, and the first Android beta.** At night your car finally lights up the road ahead and we're releasing the Android version for further testing.

### Features

- **Headlight beams.** Turning your lights on at night now throws a beam of light onto the road ahead, as on the original Xbox: it brightens the street, cars and walls it reaches and stops where it meets them ([#10](https://github.com/laureladastra/mcdub3-nx/issues/10), thanks for the report). Before, the lights worked in the game but no beam was drawn. Other emulators (i.e. xemu → https://github.com/xemu-project/xemu/issues/996) show a solid white shape instead. The game builds the beam in a small hidden picture using two tricks of the Xbox's graphics chip that the port did not reproduce yet: it reuses a picture as a depth map, and it counts the beam's layers with a special blending mode that works with negative numbers. Both are emulated now. Tested in the DUB Edition and the Remix. The WHITE button flashes your high beams while you hold it. The main headlights are on by themselves at night.
- **Android beta (app version 0.1.0).** Both games can be installed as an Android app: `mcdub3-0.1.0.apk` (DUB Edition) and `mcdub3remix-0.1.0.apk` (Remix). Like on the Switch you bring your own copy of the game.

## v1.0.0

**Version 1.0 for both games on the Nintendo Switch.** This release removes the last annoyances: races that felt slow when the frame rate dipped, random zoom-in or out during loading screens, and vertical lines during upscaling.

### Features

- **Races keep their real speed.** The original game moves the race forward by a fixed step every frame, which was fine on the Xbox, where it runs on a fixed 30 FPS. On the Switch, in rain, with nitrous or in the busiest streets, the frame rate can dip and the whole race then went into slow motion. The race now proceeds by the time that really passed. Racing in the rain and with nitrous feels much steadier.
- **No more random zooms during loading screens.** Menus and loading screens are drawn for 4:3 TVs, with black strips at the sides, and the port enlarges them to fill your 16:9 screen. When something briefly reached those strips (the circle animation on loading screens for example), the picture behind it jumped in size for a brief moment. The size stays put now.
- **No more vertical white lines at the edges at speed.** The motion blur when you drive fast could pull in thin white lines along the left, right and bottom edges of the picture. Fixed in both games.

### Other changes

- **Mac and Linux:** while driving, the arrow keys only work the gas and brake, so the DUB Edition's camera no longer moved in and out when you accelerated or braked with the keyboard. This experience is stable now.
- **Mac and Linux:** with a UI scale of 2 or more (`RECOMP_GL_UI_SCALE`), faint regular stripes over the race screen are gone.

### Tips

- **Remix:** a picture size of 1 (`RECOMP_GL_SCALE=1` in `mcdub3remix_env.txt`) gives the smoothest races. The DUB Edition handles 1.5.
- For normal play, leave `MC3_PERF` out of your settings file: it is a diagnostic and costs some speed.

### ⚠️ Known issues

- **Remix:** in the busiest parts of the city at full speed with nitrous, the frame rate can still drop for a few seconds. The race no longer slows down, but the picture gets choppier.

## v0.5.0

**A big speed update for both games on the Switch.** Races, especially in the rain, now run close to the game's own top speed of 30 FPS most of the time. Freezes during races are minimised.

### Features

- Measured in rain races on the Switch (at the default 1x picture size):

  |                                    | v0.4.0                 | v0.5.0   |
  | ---------------------------------- | ---------------------- | -------- |
  | Midnight Club 3: DUB Edition       | 23 frames per second   | **28.5** |
  | Midnight Club 3: DUB Edition Remix | 20.5 frames per second | **27.5** |

  The game itself never runs faster than 30, as on the original Xbox, so most of the time you are now at or near full speed.
- **No more half-second freezes in races.** The game stopped for about half a second now and then during a race, most often right after the start.
- **Sharper look for less effort (Remix).** The Remix's glow and blur effects are drawn at normal size even when you raise the picture size, which cost a lot of speed for something that is blurred anyway.

### Other changes

- Each game now has its own tuned graphics settings, so improving one game does not change the other.
- Saves can be copied between the Switch and a computer for the Remix too (`make save-sync-down GAME=remix`).
- Fewer files are written to the SD card at every start (some leftover diagnostic files are no longer created; you can delete any `progcache-slow` files you find).
- For developers: a much more detailed performance report with `MC3_PERF=2` (time per part of a frame, on the processor and the graphics chip, and why draws could not be combined).

### Tips

- **Remix:** keep the picture size at 1 (`RECOMP_GL_SCALE=1` in `mcdub3remix_env.txt`). Higher values still cost the Remix more than the DUB Edition.
- After updating, the first race or two may stutter briefly while the game prepares its graphics again. This settles once the file `progcache.bin` has been rebuilt; don't delete it afterwards.

### ⚠️ Known issues

- **Remix:** in the busiest parts of the city at high speed, the Remix's wide speed camera shows far more buildings than usual and the game can drop to around 12 frames per second for a few seconds; your car then feels slower too. This is the next thing being worked on.
- Right after a race starts, the game can still pause for about half a second while it loads the race's textures.

## v0.4.0

**This is the first release of Midnight Club 3: DUB Edition Remix**. Runs from boot to the finish line: intro movies, menus, garage, career and races, with music and sound. It ships next to the DUB Edition as its own app, `mcdub3remix-nx-eur.nro`.

### Features

- **Midnight Club 3: DUB Edition Remix.** A new app for this game: `mcdub3remix-nx-eur.nro` on the Switch (in `sdmc:/switch/mcdub3remix-nx/`, disc in `game-eur/` or `game/`), and a Mac app built with `make mac-app GAME=remix`.
- **Remix races play as they should.** From the countdown on, the camera no longer turns into a spinning top view, you can drive, and the pause and options menus appear centred with their list shown once.
- **More of the game converted correctly.** Floating-point comparisons now give the same answers as the Xbox's processor, also when a value is not a number. The Remix's code relies on this throughout (it was compiled for the Xbox's SSE unit, the DUB Edition mostly for the older x87 one).
- **Keyboard driving on Mac and Linux.** While you drive, the arrow keys now only press the gas and the brake. In the Remix the up arrow also pulled the race camera in and up; in menus the arrows still move through the lists.

### Other changes

- NSP forwarders are no longer attached to releases: they need your console's keys, so they are built on your own computer (`make nsp`, or `make nsp GAME=remix`; see CONTRIBUTING.md). The NRO is all you need with hbmenu.
- On the Switch, a run with `MC3_PERF=2` in `mcdub3_env.txt` keeps writing its log while the game hangs, so a hang can be diagnosed from the log.
- For developers: the app's identity moved to `config/<game>/app.env`; the repository's history now holds this project's commits only (the toolkit is vendored as a snapshot).

## v0.3.0

### Features

- **Smooth loading screens.** The circles on the career loading screen now turn smoothly and close, on every platform. The game copies each frame into the next one with the Xbox GPU's 2D engine and only draws what changed.
- **Faster menus on the Switch.** Moving through the main menu and its city flythrough went from about 20 to 27-30 frames per second. A background thread of the game checked a one-second timer thousands of times a second and kept interrupting the main thread. It now checks every few milliseconds.
- **More of the game lifted correctly.** Functions that end in a switch table were sometimes cut into pieces when the game's code was converted. They are now kept whole.

### Other changes

- On the Switch, the previous run's log is kept as `mcdub3_log_prev.txt` next
  to `mcdub3_log.txt`, so a hang can still be reported after the game was
  started again.
- We're preparing to port Midnight Club 3: Dub Edition Remix and integrate its code into this repo. Tools and scripts have been updated and the repo has been reorganised to support parallel builds for these different games.
- Generated files now live in `work-dub/` instead of `work/`. The first
  `make` after updating moves the folder for you; run `make lift` again
  after updating, as the converted game code changed.
- `make mac-app` takes the disc folder as `DISC=` (it was `GAME=`).

### ⚠️ Known issues

- Driving on the Switch runs at 18-25 frames per second.
- A race can occasionally hang (the screen stops, the game does not crash).
  If it happens, leave it for about ten seconds before quitting and share
  `mcdub3_log.txt` (or `mcdub3_log_prev.txt` if you started the game again).
- Right after a race starts, the game can pause for about a second.
- With fast storage the career loading screen can end before its closing
  animation has played; this is how the game reacts to a load that finishes
  very quickly.
  
## v0.2.0

### Features

- **Support for European (PAL) disc.** Updated the [guided installation page](https://laureladastra.github.io/mcdub3-homepage/) and build artifacts (`mcdub3-nx-eur.nro`) to build for this edition.
- **Beta release for Windows.** Updated section in our [README](https://github.com/laureladastra/mcdub3-nx) contains the build and installation guide.
- **Readable tutorial video text**: the text within tutorial videos is no longer scrambled at higher game resolutions.
- **Saves**: profiles from an original Xbox (a memory unit dump such as a `.gsx` from a save site) can be imported with `make import-save`, and profiles can be copied between the Switch and a computer over FTP with `make save-sync-down` / `make save-sync-up` (see the README's Saves section).

### ⚠️ Known issues

- Menus and the city flythroughs run below full speed on the Switch.
- The circles on the career loading screen sometimes stutter.
  complete; this is being investigated.
- Occasional hangs on career loading screens are being worked on.


## v0.1.0

This is a beta release of the game to provide early access to the community. Expect some crashes.

### Features

- Intro movies, menus, garage, career and races, with music and sound.
- Saves: create, load and delete profiles.
- Widescreen, higher resolutions and picture settings
  (`mcdub3_env.txt`; see the README's Settings section).

### ⚠️ Known issues

- Menus and the city flythroughs run below full speed on the Switch.
- Occasional hangs on career loading screens and in races are being worked
  on.