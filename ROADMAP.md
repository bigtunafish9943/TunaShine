# Roadmap

What TunaShine is working toward, roughly in order. Nothing here is a promise or a date.

## Now

- **Reliable controller rumble.** Two host-side changes are in: rumble is sent to the client within about
  20 ms instead of up to 150 ms, and a reconnected controller no longer has its first rumble dropped. Whether
  this fixes rumble over Wi-Fi is not yet confirmed on real hardware.
- **Returning to the PC when the client disconnects.** Restoring the physical display, resolution and window
  positions when a stream ends is unreliable. Needs investigation in the display restore code.
- **Easier setup and a better web UI.** Done so far: new theme, status home page with first-device steps, settings
  in a side menu, a single Devices page for pairing and permissions. The app editor now shows the few fields most apps
  need and folds the rest away. Still to do: the Applications list itself.

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
