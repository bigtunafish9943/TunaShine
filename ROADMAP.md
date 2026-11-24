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

[TunaReceive](https://github.com/bigtunafish9943/TunaReceive) is TunaStream's app for Android TV (starting with the
NVIDIA Shield), phones and tablets, built on Artemis and Moonlight for Android. It is out, and updates itself.

Done: setup on first start, a short settings page, updates from inside the app, going straight to the PC's games
(waking it if needed), a connection test that fits the picture quality to the Wi-Fi, the quick menu on a held Home
button, plain-words stream stats, and TunaStream's look.

Still to come:

- **Rumble over Bluetooth for 8BitDo controllers**, and their 2.4G adapter on a TV box
  ([moonlight-android#1442](https://github.com/moonlight-stream/moonlight-android/issues/1442)).
- **TunaStream actions from the couch**: play it on the PC, repair the virtual screen.
- Pairing without typing a PIN, by scanning a code shown on the TunaStream home page.
- **Send files from the PC to the device**: a *Send to device* button on the TunaStream home page, delivered
  over the existing paired connection, with no extra app or login. Until then, LocalSend does the job.

## Later (needs testing on real hardware for each change)

- Startup and service reliability on Windows 11 24H2 and later.
- Multiple virtual displays and multiple instances: cleaner management and primary-display handling.
- Frame pacing and micro-stutter with virtual displays.
- Gradual cleanup of the capture, display and input layers as each area is touched.

- **Playing away from home over Tailscale.** A TV at someone else's house can already reach the PC when both run
  Tailscale and the PC is added by its Tailscale address. To make that easy: show the PC's Tailscale address on the
  home page, treat it as a trusted address for pairing, pick a picture that fits the home's upload speed, and say
  when the connection is being relayed instead of going direct.
- **Picture size that adjusts itself.** The bitrate and packet-loss protection already follow the connection; the
  picture size (4K down to 1440p and back) could follow it too.
- **Two people using one PC independently.** Today a stream shares the mouse, keyboard and window focus with
  whoever is at the PC. Fully separate use means a separate Windows session for each person, which is what Duo
  does by patching Remote Desktop. That is a large project with licensing questions, not a setting.

## Outside this repository

- **SudoVDA driver fixes** (sleep/wake hangs, HAGS regressions, teardown crashes). The driver is a separate
  project and Windows only loads signed drivers. TunaStream can detect and recover from driver failures, but
  cannot ship a modified driver without getting it signed.
- **Extra audio codecs (AC-3, E-AC-3) and new video codecs (such as Pyrowave).** These need support in the client
  app as well as the host.
