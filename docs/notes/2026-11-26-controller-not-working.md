# Handoff: controller does nothing on the TV (2026-11-26)

From the Claude session working on TunaStream and TunaReceive, for the Claude working on
TunaHome. The user asked for this note so you can look at the controller problem as well.

## What the user sees

- Controller: **8BitDo (Ultimate 2 Wireless) through its 2.4G USB dongle**, plugged into the TV box.
- Starting a game from TunaReceive: Steam Big Picture opens on the PC with the mouse pointer in
  the middle, and **the controller controls nothing** (Steam sees no controller).
- **The controller also doesn't move TunaReceive's own menus** (games list). So the TV isn't
  getting input from it at all, before any stream.
- It **worked last night** (evening of 2026-11-25, US time).
- Not yet known: whether it moves TunaHome's home screen, whether the dongle's light is solid,
  whether the controller's mode switch is on 2.4G. The user has been asked; no answer yet.

## What changed on the TV since it last worked

**TunaHome** (all on `main`, pushed by me with the user's permission; please pull before working):

| Commit | Change | Touches input? |
|---|---|---|
| `32ba7ff` | Icon packs match more apps; Watch Next follows the TV's list | no |
| `5484b62` | Icons keep their shape | no |
| `9576386` | Update check hourly | no |
| `846e6d7` | Sounds: `BaseActivity.dispatchKeyEvent` now does D-pad focus moves itself (to skip Android's click); theme `android:soundEffectsEnabled=false` | **TunaHome's own screens only**; worth a second look |
| `560922f` | Who's in on the top bar | no |
| `d420211` | `HomeActivity` gets a MAIN + HOME + DEFAULT intent filter (so Launcher Manager lists it); the user may have set TunaHome as the default home since | possibly, indirectly |
| `b3068b7` | Open apps (Home twice): `HomeButtonService` calls `OpenApps.note()` on window changes, adds a default Home×2 shortcut; `KILL_BACKGROUND_PROCESSES` | no key handling changed |

`HomeButtonService.onKeyEvent` itself is unchanged: it only takes Home and Back and returns
false for everything else. The earlier patch (0001, key filtering off while
`com.limelight.Game` / `WatchActivity` is in front) is in.

**TunaReceive** 1.0.99 to 1.0.103: Watch sync, one-press PC wake (PcView), Continue/Last played,
and, after the problem was reported, `672b8d3`: `MenuControllerBridge` (new) binds
`UsbDriverService` whenever a TunaReceive screen other than `Game` is in front, claims the 8BitDo
dongle (`setClaimForRumble(true)`) and injects D-pad / A (DPAD_CENTER) / B (BACK) key events
into TunaReceive's own window with `Instrumentation.sendKeySync`. Before that, TunaReceive
only claimed the dongle during a stream (`Game`), and in menus the controller went through
Android. No input-path commits in TunaReceive between the last working evening and the report.

**TunaStream** 1.0.210 / 1.0.212: Watch page, window sharing, self-check. No input changes.

## What's been ruled out / suspected

- Not the PC: the controller doesn't work in TunaReceive's menus, before a stream.
- Most likely between the controller and the dongle (mode switch moved to Bluetooth, lost
  pairing), but please check TunaHome's side: anything that could hold or swallow a USB HID
  device or gamepad input system-wide (accessibility key filtering `canRequestFilterKeyEvents`,
  an overlay or window holding focus, being the default HOME now).
- Useful test on the TV: does the controller move **TunaHome**? Does `getevent -l` or Android's
  Settings > Remotes & Accessories see the dongle? Does it work with TunaHome's Home button
  setting (accessibility service) switched off?

## Please

- Pull TunaHome `main` first (`b3068b7` or later): my commits are there.
- If you change anything in TunaReceive's USB driver or `MenuControllerBridge`, that's in the
  TunaReceive repo (`app/src/main/java/com/limelight/binding/input/driver/`).
