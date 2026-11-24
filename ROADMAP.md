# Roadmap

What TunaShine is working toward, roughly in order. Nothing here is a promise or a date.

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
- **Installs next to Sunshine or Apollo**, on its own port.

## Next

- **Clear fallback when the virtual display fails**: visible error in the web UI, useful logs, and falling back
  to a physical display instead of a black stream.
- **HDR on the wrong display.** Reports of HDR being applied to the physical display or not detected.
- **Our own documentation**, especially for the virtual display, permissions and troubleshooting.

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
  project and Windows only loads signed drivers. TunaShine can detect and recover from driver failures, but
  cannot ship a modified driver without getting it signed.
- **Extra audio codecs (AC-3, E-AC-3) and new video codecs (such as Pyrowave).** These need support in the client
  app as well as the host.
