<p align="center">
  <img src="apollo.svg" width="128" height="128" alt="TunaShine icon">
</p>

# TunaShine

TunaShine is a self-hosted game and desktop streaming host for Windows. You install it on your gaming PC and
play from a phone, tablet, handheld, TV or another computer using a
[Moonlight](https://moonlight-stream.org)-compatible client.

It is a fork of [Apollo](https://github.com/ClassicOldSong/Apollo), which is itself a fork of
[Sunshine](https://github.com/LizardByte/Sunshine).

> [!NOTE]
> TunaShine is at an early stage and its builds are untested. So far it differs from the Apollo version it was
> forked from in name, icon and two small rumble changes. The goals below describe where it is heading, not what
> is finished; see the [roadmap](ROADMAP.md).

## Goals

- **Easier setup.** One installer that leaves you ready to pair, with fewer manual steps.
- **A better interface.** A redesigned web UI for pairing, apps and settings.
- **Reliable controller rumble**, including when the client is on Wi-Fi.

## What you get from Apollo

- Built-in virtual display with HDR support that matches each client's resolution and frame rate automatically.
  No monitor or dummy plug is needed.
- Hardware encoding on AMD, Intel and Nvidia GPUs, with a software fallback.
- Per-client permissions.
- Clipboard sync.
- Commands that run when a client connects or disconnects.
- Input-only mode.

## Download

There are no tagged releases yet. Every change is built automatically, and the Windows installer for each build
is attached to its run on the [Actions](https://github.com/bigtunafish9943/TunaShine/actions) page, under
**Artifacts** (you need to be signed in to GitHub to download it).

These builds are untested development builds.

## Clients

- [Moonlight](https://moonlight-stream.org) on Windows, macOS, Linux, Android, iOS and more.
- [Artemis](https://github.com/ClassicOldSong/moonlight-android) on Android, which adds client-side control of the
  virtual display.

## Permissions

The first client you pair gets full permissions. Clients paired after that can only view streams and list apps
until you grant more. If a new client shows `Permission Denied` when launching an app, or its mouse and keyboard
do nothing, open that client in the web UI and grant `Launch Apps`, `Mouse Input` or `Keyboard Input`.

## Virtual display

TunaShine uses the SudoVDA driver. A virtual display is created when a stream starts and removed when the app
quits. Each client gets a fixed display identity, so Windows remembers its display settings.

> [!WARNING]
> Remove any other virtual display driver before using this one. Running two causes conflicts.

On a laptop with two GPUs, set `Adapter Name` to the discrete GPU and enable `Headless mode` in the
`Audio/Video` tab, then restart.

## HDR

HDR needs Windows 11 23H2 or later. How good it looks depends mostly on the client device.

## Requirements

| Component | Minimum |
|-----------|---------|
| OS        | Windows 10 or later |
| GPU       | AMD VCE 1.0+, Intel with QuickSync, or an NVENC-capable Nvidia card |
| CPU       | AMD Ryzen 3 or Intel Core i3 |
| RAM       | 4 GB |
| Network   | 5 GHz Wi-Fi (802.11ac); wired Ethernet recommended for 4K |

## Building

Pushing to this repository builds the Windows installer with GitHub Actions; see
[`.github/workflows/build-windows.yml`](.github/workflows/build-windows.yml). To build locally, follow
[docs/building.md](docs/building.md).

## Credits

TunaShine exists because of the work of others:

- [Apollo](https://github.com/ClassicOldSong/Apollo) by ClassicOldSong
- [Sunshine](https://github.com/LizardByte/Sunshine) by LizardByte and its contributors
- [Moonlight](https://github.com/moonlight-stream) and its contributors
- [SudoVDA](https://github.com/SudoMaker/SudoVDA) and [ViGEmBus](https://github.com/nefarius/ViGEmBus)

## License

[GPL-3.0](LICENSE), the same license as Apollo and Sunshine.
