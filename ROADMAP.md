# Roadmap

What TunaStream is working toward, roughly in order. Nothing here is a promise or a date.

## Now

Everything here is built and passes the automated checks, but has not yet been confirmed on real hardware.

- **Reliable controller rumble.** Rumble reaches the client within about 20 ms instead of up to 150 ms, a
  reconnected controller no longer has its first rumble dropped, and the Troubleshooting page has a rumble test
  that tells a game problem from a client problem.
- **Stopping and starting.** Streams run on a virtual display that is the main display only while someone is
  streaming. Closing the client hands the main display back to the PC; reconnecting takes it again. A dropped
  connection keeps the app running for two minutes instead of quitting it.
- **Easier setup and a better web UI.** New theme, status home page with first-device steps and an End session
  button, navigation and settings in side menus, settings written in plain words with technical options folded
  under *Advanced settings* or *For experts*, a single Devices page for pairing and permissions with one-click
  presets, and an app editor that folds rarely used options away.
- **Updates from inside the app.** The home page offers *Update now* when a new release is out; TunaStream
  downloads the installer, checks it against GitHub's published checksum and installs it silently.
- **Installs next to another streaming host**, on its own port.
- **Releases you can download without a GitHub account**, published from every passing build of `master`, with a
  branded installer.
- **See who is playing.** The home page lists each streaming device and what its controllers are presented to
  games as.
- **Translations** of TunaStream's own screens in German, Spanish, French, Italian, Brazilian Portuguese, Russian,
  Japanese, Chinese and Korean. Settings reworded in plain English show in English in other languages until they
  are translated again.

## Next

- **Clear fallback when the virtual display fails.** Started: TunaStream retries once, falls back to the PC's own
  screen, and the home page says so. Still to do: reconnecting to a driver that hangs after sleep without a
  restart.
- **HDR on the wrong display.** Reports of HDR being applied to the physical display or not detected.
- **More documentation.** A troubleshooting guide for rumble, the connection and stop/start exists; the virtual
  display and permissions still need their own pages.

## TunaReceive: our own client

A TunaStream build of Moonlight for Android and Android TV (starting with NVIDIA Shield), sideloaded as an APK
from this repository's releases. Moonlight for Android is GPL-3.0, like TunaStream, so it can be forked the same
way. What it would add over stock Moonlight:

- **Controller rumble that works over Bluetooth and with 2.4G adapters**, including 8BitDo controllers
  ([moonlight-android#1442](https://github.com/moonlight-stream/moonlight-android/issues/1442)).
- **Smooth-video defaults** that match TunaStream's frame-rate matching, and the stream health readout on screen.
- **TunaStream actions from the couch**: end the session, play it on the PC, repair the virtual screen.
- Pairing without typing a PIN, by scanning a code shown on the TunaStream home page.
- **Send files from the PC to the device**: a *Send to device* button on the TunaStream home page, delivered
  over the existing paired connection, with no extra app or login. Until then, LocalSend does the job.

Built and published by GitHub Actions like the host. Starts once the host has settled on real hardware.

## Later (needs testing on real hardware for each change)

- Startup and service reliability on Windows 11 24H2 and later.
- Multiple virtual displays and multiple instances: cleaner management and primary-display handling.
- Frame pacing and micro-stutter with virtual displays.
- Gradual cleanup of the capture, display and input layers as each area is touched.

- **Two people using one PC independently.** Today a stream shares the mouse, keyboard and window focus with
  whoever is at the PC. Fully separate use means a separate Windows session for each person, which is what Duo
  does by patching Remote Desktop. That is a large project with licensing questions, not a setting.

## Outside this repository

- **SudoVDA driver fixes** (sleep/wake hangs, HAGS regressions, teardown crashes). The driver is a separate
  project and Windows only loads signed drivers. TunaStream can detect and recover from driver failures, but
  cannot ship a modified driver without getting it signed.
- **Extra audio codecs (AC-3, E-AC-3) and new video codecs (such as Pyrowave).** These need support in the client
  app as well as the host.
