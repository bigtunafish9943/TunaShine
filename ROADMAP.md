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
  button, settings in a side menu, a single Devices page for pairing and permissions with one-click presets, and
  an app editor that folds rarely used options away.
- **Installs next to another streaming host**, on its own port.
- **Releases you can download without a GitHub account**, published from every passing build of `master`, with a
  branded installer.
- **See who is playing.** The home page lists each streaming device and what its controllers are presented to
  games as.
- **Translations** of TunaStream's own screens in German, Spanish, French, Italian, Brazilian Portuguese, Russian,
  Japanese, Chinese and Korean.

## Next

- **Clear fallback when the virtual display fails.** Started: TunaStream retries once, falls back to the PC's own
  screen, and the home page says so. Still to do: reconnecting to a driver that hangs after sleep without a
  restart.
- **HDR on the wrong display.** Reports of HDR being applied to the physical display or not detected.
- **More documentation.** A troubleshooting guide for rumble, the connection and stop/start exists; the virtual
  display and permissions still need their own pages.

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
