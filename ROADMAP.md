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
- **A stream that fits itself to the connection.** With TunaReceive, the bitrate steps down when frames start
  arriving slowly (before any are lost) and back up when the connection clears; the picture size follows once the
  bitrate is too thin for it (4K to 1440p and back); and packet-loss protection uses less on a clean connection
  and more on a rough one. Windows doesn't scan for other Wi-Fi networks mid-stream, and the home page says when
  the PC itself is on Wi-Fi.
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
button, plain-words stream stats, and TunaStream's look. Also:

- **Rumble for the 8BitDo Ultimate 2 through its 2.4G adapter** on a TV box, with menus driven by that controller.
- **TunaStream actions from the couch**: repair the virtual screen; files sent with *Send to device* arrive in
  Downloads.
- **A stream that fits itself to the connection**: picture quality and size adjust while playing, the TV is asked
  for its game mode, and a dropped stream reconnects by itself.
- **How the last stream went**: a plain report after each stream, and the TV's view of the stream shown on
  TunaStream's home page next to the PC's.

Still to come:

- **Rumble for 8BitDo controllers over Bluetooth** on Android, which the controller doesn't offer in that mode
  ([moonlight-android#1442](https://github.com/moonlight-stream/moonlight-android/issues/1442)).
- Pairing without typing a PIN, by scanning a code shown on the TunaStream home page.
- Translations of TunaReceive's own screens.

## Later (needs testing on real hardware for each change)

- Startup and service reliability on Windows 11 24H2 and later.
- Multiple virtual displays and multiple instances: cleaner management and primary-display handling.
- Frame pacing and micro-stutter with virtual displays.
- Gradual cleanup of the capture, display and input layers as each area is touched.

- **Playing away from home over Tailscale.** A TV at someone else's house can already reach the PC when both run
  Tailscale and the PC is added by its Tailscale address. To make that easy: show the PC's Tailscale address on the
  home page, treat it as a trusted address for pairing, pick a picture that fits the home's upload speed, and say
  when the connection is being relayed instead of going direct.
- **Two people using one PC independently.** Today a stream shares the mouse, keyboard and window focus with
  whoever is at the PC. Fully separate use means a separate Windows session for each person, which is what Duo
  does by patching Remote Desktop. That is a large project with licensing questions, not a setting.

## Outside this repository

- **SudoVDA driver fixes** (sleep/wake hangs, HAGS regressions, teardown crashes). The driver is a separate
  project and Windows only loads signed drivers. TunaStream can detect and recover from driver failures, but
  cannot ship a modified driver without getting it signed.
- **Extra audio codecs (AC-3, E-AC-3) and new video codecs (such as Pyrowave).** These need support in the client
  app as well as the host.
