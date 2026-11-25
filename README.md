<p align="center">
  <img src="docs/images/tunastream-logo.png" width="720" alt="TunaStream: self-hosted game streaming">
</p>

<p align="center">
  Play your PC's games on your phone, tablet, handheld or TV.<br>
  A self-hosted streaming host for Windows. Play with <a href="https://github.com/bigtunafish9943/TunaReceive">TunaReceive</a> on Android TV and phones, or any <a href="https://moonlight-stream.org">Moonlight</a> client.
</p>

<p align="center">
  <img src="docs/images/tunastream-home.png" width="820" alt="The TunaStream home page, showing what is streaming, paired devices and virtual display status">
</p>

TunaStream runs on your gaming PC and streams it to another device over your network. It is built to be easy to
set up, pleasant to use, and smooth on ordinary home Wi-Fi.

> [!NOTE]
> This is TunaStream 1.0, the first complete version. It is played on every day with one setup (an RTX 5080 PC streaming over Wi-Fi to an NVIDIA
> Shield TV Pro running [TunaReceive](https://github.com/bigtunafish9943/TunaReceive)), and every release passes
> automated checks, but it has had little testing on other hardware. Expect rough edges.

## Smooth on Wi-Fi

Most streaming hosts send at one fixed quality and hope the network keeps up. TunaStream and TunaReceive watch
the stream together and adjust it while you play, so a busy evening on the Wi-Fi costs a little sharpness
instead of a frozen picture.

- **It eases off before anything is lost.** TunaReceive notices when frames start arriving late, which happens
  well before packets go missing, and the bitrate steps down straight away. When the connection clears, it
  climbs back up.
- **The picture size follows, without a freeze.** When the bitrate has dropped too far for a sharp 4K picture,
  the stream moves to 1440p or 1080p and the TV scales it to fill the screen. The new encoder is prepared in the
  background and swapped in between two frames, so the change is a slight softening rather than a pause. It
  goes back to full size when there is room again.
- **Error correction that fits the connection.** Extra recovery data lets the TV repair lost packets without
  asking for them again. On a clean connection TunaStream sends less of it (as little as half your setting),
  leaving more of the bitrate for the picture; at the first lost video it sends more, and holds that level for longer each time
  the connection proves rough.
- **Split-frame encoding on high-end Nvidia cards.** Cards with two or more NVENC engines (high-end RTX 40 and
  50 series) encode each 4K frame in two halves at the same time, which cuts the time the PC spends on every
  frame. It is on by default and turns itself off on cards that can't do it. *Settings > NVIDIA*.
- **No Wi-Fi scans mid-stream.** Windows looks for other Wi-Fi networks every minute or so, and on a PC that is
  itself on Wi-Fi each scan is a short lag spike. TunaStream pauses those scans while someone is streaming. The
  home page also says when the PC is on Wi-Fi and how strong its signal is, since a cable is the biggest single
  improvement most setups can make.
- **TV game mode.** TunaReceive asks the TV to skip its picture processing (HDMI ALLM), which on many TVs takes
  tens of milliseconds off every frame.
- **Sound that doesn't drop out.** TunaReceive starts with the smallest sound buffer and gives it a little more
  room each time a packet arrives late, so a clean connection keeps the lowest delay and an uneven one stops
  going silent.
- **You can see all of it.** The home page shows each device that is playing: frame rate, bitrate, error
  correction, picture size and whether frames are being split, next to the TV's own view of the stream (frames
  arriving late, decode time, delay). The log keeps a line a minute for every stream.

## Rumble that works

Rumble goes game → virtual Xbox 360 controller on the PC → TunaStream → network → TunaReceive → your controller,
and every step was tightened:

- **Fast.** TunaStream sends rumble to the client within about 20 ms, where it used to take up to 150 ms, and a
  controller that reconnects no longer loses its first rumble.
- **The right controller for the game.** Controllers with motion sensors but no touchpad, such as 8BitDo and
  Switch Pro controllers, are presented to games as an Xbox 360 controller, which nearly every PC game rumbles.
- **8BitDo Ultimate 2 through its 2.4G adapter, on a TV box.** Android can read this controller's buttons but has
  no way to rumble it. During a stream TunaReceive takes the adapter over and drives it directly, so rumble,
  motion and the battery level all work, and the controller still drives the menus. Rumble is refreshed several
  times a second, so a long rumble never fades out early.
- **Controllers in Xbox mode** get the start-up message Windows' own driver sends; some adapters, 8BitDo's among
  them, send no input until they receive it.
- **A rumble test.** *Rumble now* on the Troubleshooting page buzzes the controller directly, which tells a game
  problem from a client problem in one press. TunaReceive's *Check my controller* shows what the controller is
  and whether it can rumble.

## From the couch, and from your phone

- **Messages and sounds on the TV.** Type a message on the home page, or tap a ready-made one ("Dinner's
  ready!"), and it pops up over the game within a few seconds. Six built-in sounds (doorbell, dinner bell, air
  horn...) play on the TV too, and you can add your own.
- **Screenshots and instant replay.** On the TV, View + R1 saves a screenshot (on the TV and in your
  Pictures\TunaStream folder), and View + L1 saves the last 30 seconds as a clip in Videos\TunaStream, exactly as
  the stream looked. *Make this week's highlights* joins the end of each replay into one clip. The buttons are
  yours to change in TunaReceive.
- **A Live page for your phone.** Scan the QR code on the home page. While someone plays: what's playing with its
  cover, and the same stream details the TV's overlay shows (picture size and codec, frame rate set, now and on
  average, delay, how steady it is, bitrate, and the TV's and the PC's network), plus controller batteries, play
  time left, the message and sound buttons and End session. While nobody plays: the last seven days, play time
  today, each TV and how it's set up, this PC, milestones and captures. Add it to your home screen to open it
  like an app; *Full page* opens everything else.
- **The TV's settings, from the PC.** Under **Devices > TV settings**, change TunaReceive's settings on a TV
  (picture, sound, controller shortcuts and more), each explained in full. The TV picks the changes up within
  15 seconds while its games are on screen.
- **Now playing on the tray icon.** Hover over TunaStream's icon by the clock to see who is playing what.
- **Shared PC mode** (on by default). Keep using the PC while someone streams: your monitor stays the main
  screen, only the game's windows go to the TV, only the game's sound goes to the stream, your mouse can't
  wander onto the TV's screen, and when you leave your mouse and keyboard alone the game gets focus back. Games
  set to *Windowed* lose their border and fill the TV. Or make the TV's screen the main one, so games open there
  by themselves, and your own new windows (Windows' own included) are moved back to your screens.
  *Settings > Shared PC*.
- **Watch in a browser.** *Watch* on the home page shows the game being streamed, with its sound, on any laptop
  or phone on your network, view only and a moment behind the TV. *Share a watch link* gives someone else a link
  that works once, within a day, and opens nothing but the picture. The device that opens it keeps watching
  with its own key, which changes every day and stops working after a week unused. For someone who comes back often, a
  *watch password* doesn't change: typed on the Watch page, it lets that device watch the same way.
- **Share my screen.** On the Watch page, choose to share one of the PC's own screens for an hour or a few, so someone can
  watch you play at the desk on a tablet or phone. The home page and the tray say while it's on, a TV's stream takes
  over while it plays, and it turns itself off when the time is up.
- **Do not disturb during streams** (on by default). Windows' notifications wait in the notification centre
  instead of popping up over the game.
- **Privacy mode** (off unless turned on, with Shared PC off). The PC's own monitors go dark while a TV streams,
  so nobody in the room sees what's being played, and come back as they were when the stream ends or pauses.
  If TunaStream stops unexpectedly, they come back the next time it starts.

## Everything else

- **Streams on its own virtual monitor.** Every stream gets a virtual display sized to the device you are playing
  on, with HDR. No spare monitor or dummy plug, and your real monitor keeps its own picture.
- **Gives your screen back when you stop.** The virtual display is the main display only while someone is
  streaming. Close the client and your monitor is the main display again, even if the game is left running.
- **Survives a dropped connection.** A Wi-Fi hiccup does not quit your game: it keeps running for two minutes so
  you can reconnect, and TunaReceive reconnects by itself.
- **A clear web interface.** A home page that shows what is streaming, who is playing and with which
  controllers, which devices are paired and whether the virtual display is working, with an End session button.
  Settings in plain words, with the ones most people change on the *Everyday* tab and the technical ones tucked
  away. Pairing and permissions together on one Devices page. Tick *Keep me signed in* to stay signed in for 15
  days, through restarts and updates; *Apply* brings the page back by itself once TunaStream has restarted.
- **Access tokens** for scripts and home automation (*Change Password > Access tokens*): each can only do what
  you tick (see what's playing, watch, send messages, end sessions, start games, read logs) and can be removed
  any time.
- **AI assistants.** `tunastream.exe --mcp` makes TunaStream a set of MCP tools for Claude Desktop, Claude Code
  and other assistants: what's playing, the paired devices and the games, starting a game, ending a session,
  more play time, a message on the TV, the play recap, and the PC's and TVs' logs. It uses an access token, so
  an assistant can do only what the token allows; making a token shows the settings to paste in.
- **Your last seven days.** The home page shows play time for each day, the most-played games and how smooth the
  streams were, with milestones as they're reached. The record stays on the PC.
- **A reminder when a game runs slow.** When a game itself draws fewer than 59 frames a second, the home page
  says so and names the fix in the game's settings, and keeps a reminder until it runs at full rate.
- **Your games, kept in sync** (off unless turned on, on the Apps page). Steam games you've played lately and games
  newly installed from Epic, GOG, EA, Ubisoft, Amazon and the Microsoft Store are added by themselves, with
  covers, and taken off again when they're uninstalled or, for Steam, not played for a while. Apps you added
  yourself are never touched, and a synced game you delete stays deleted.
- **The game's frame rate matches the stream** (on by default). With RivaTuner Statistics Server installed the
  cap is exact, 59.94 included; otherwise NVIDIA's frame cap is used. V-Sync is turned off in games while
  streaming on NVIDIA cards, since the TV paces the picture. Both go back as they were when the stream ends.
- **Game covers from Steam.** Steam games added as apps get their cover on the client's game tiles: a custom cover
  you set in your Steam library first, then the one Steam shows, fetched by itself.
- **Controller batteries** on the home page next to each controller, in red when one is low.
- **Nine colour themes**: Deep Sea, Coral Reef, Kelp Forest, Midnight, Golden Hour, Halloween, Autumn, Winter
  and Summer, in light and dark. *Seasonal themes* wears the last four in their season. TunaReceive wears the
  PC's theme by default, and so do TunaStream's own Desktop, Steam and Virtual Desktop tiles. *Settings >
  General*.
- **Updates itself.** When a new version is out, a banner offers *Update now*; TunaStream downloads it, checks it
  against GitHub's published checksum and installs it, keeping your settings and paired devices. Afterwards the
  home page shows what's new since the version you last saw.
- **Troubleshooting that helps.** The log, ready to save and attach to a bug report; logs sent from TVs
  (TunaReceive's *PC tools > Send logs to PC*); a rumble test; and a Wi-Fi check that lists the networks nearby
  and says which channel is quietest.
- **A test checklist** (*/checklist*, linked from Troubleshooting) of features to try on your own PC, TV and
  controllers, with how to tell each one works. Mark each *Works* or *Problem*; the marks are kept on the PC.
- **English, Spanish, French and German**, under *Settings > General > Language*.
- **Backup and restore.** One file with your settings, apps, sounds and play history, and your pairings if you
  tick them. TunaReceive can keep its own backup on the PC and get it back on a new TV.
- **Permissions for each device**, with one-click presets such as *Controller only*.
- **Send files to the TV** from the home page; they arrive in TunaReceive's Downloads.
- **Hardware encoding** on AMD, Intel and Nvidia GPUs, with a software fallback.
- Clipboard sync, input-only mode, and commands that run when a device connects or disconnects.

<p align="center">
  <img src="docs/images/tunastream-settings.png" width="820" alt="The settings page with its section menu on the left">
</p>

## Install

1. **Download the installer.** Open [Releases](https://github.com/bigtunafish9943/TunaStream/releases/latest)
   and download `TunaStream-<version>-setup.exe`.
2. **Run it.** Windows will warn about an unknown publisher because the installer is not
   code-signed; choose **More info**, then **Run anyway**. The installer also sets up the virtual display and
   virtual gamepad drivers.
3. **Open TunaStream** from the Start menu or its tray icon, and create the username and password you will use
   for the web interface. The home page then shows the steps to pair your first device.

After that, you don't need to come back here: new versions show up on the home page with an *Update now* button.

Already running another streaming host such as Sunshine? You can keep it. The installer notices and gives
TunaStream its own port (48989) and a name ending in `-TunaStream`, so both can run and show up separately in your
client.

## Pair a device

1. Install a client on the device you want to play on. On Android TV, phones and tablets, use
   [TunaReceive](https://github.com/bigtunafish9943/TunaReceive/releases/latest): everything under *Smooth on
   Wi-Fi* and *Rumble that works* needs it. On other devices, [Moonlight](https://moonlight-stream.org) works.
2. Add your PC in the client. It shows a PIN.
3. In TunaStream, open **Devices**, enter that PIN and give the device a name.
4. Pick **Desktop** or a game in the client.

The first device you pair gets every permission. Devices paired later can only view streams and list apps until you
grant more, so if a new device says `Permission Denied`, or its mouse and keyboard do nothing, edit it under
**Devices** and turn on *Launch Apps*, *Mouse Input* or *Keyboard Input*.

## Good to know

- **Using the PC while it streams.** Turn on *Shared PC* under **Settings > Shared PC** (see above). Without it,
  the stream's virtual display is an extra monitor on the same Windows desktop,
  so your real monitor stays usable. While someone is streaming, the virtual display is the main display, so
  games open on the stream. So do new windows; notifications are kept off it by *Do not disturb during
  streams* (on by default). Windows still has one mouse, one keyboard and one focused window for both of you, so clicking a window
  on your monitor takes focus away from the game. It works best when the person streaming plays with a
  controller: under **Devices**, edit their device and choose *Controller only*, and under
  **Settings > Screen and sound** turn on *Keep my mouse off the stream's screen*.
- **A game keeps running after you close the client.** That is the default, so you can pick up where you left
  off. Turn on *Quit the game when the last device disconnects* under **Settings > General**, or *Terminate on
  Pause* for a single app, to quit it instead.
- **Running next to another streaming host.** TunaStream appears in your client as a separate PC and needs its
  own pairing. If the client doesn't find it, add it by hand using the PC's IP address and the port, such as
  `192.168.1.20:48989`. Stream from one host at a time.
- **Other virtual display drivers.** Remove them before using TunaStream; two at once cause conflicts.
- **Laptops with two GPUs.** Under **Settings > Screen and sound > Advanced settings**, set *Graphics card* to
  the discrete GPU.
- **Windows.Graphics.Capture** (*Settings > Video encoding > Screen capture method*) can be smoother with frame
  generation and windowed games. TunaStream captures through a helper in your signed-in session, and uses
  Desktop Duplication on the lock screen, during UAC prompts and when nobody is signed in. *Automatic* uses
  Desktop Duplication.
- **HDR** needs Windows 11 23H2 or later, and how good it looks depends mostly on the device you stream to.

Something not working? See [troubleshooting](docs/troubleshooting.md) for rumble, connection and stop/start
problems.

## Requirements

| | Minimum |
|---|---|
| OS | Windows 10 or later |
| GPU | AMD VCE 1.0+, Intel with QuickSync, or an NVENC-capable Nvidia card |
| CPU | AMD Ryzen 3 or Intel Core i3 |
| RAM | 4 GB |
| Network | 5 GHz Wi-Fi (802.11ac). Wired Ethernet is recommended for 4K |

## Where it's going

See the [roadmap](ROADMAP.md). Found a bug or have an idea?
[Open an issue](https://github.com/bigtunafish9943/TunaStream/issues).

## Building

Every push builds the Windows installer with GitHub Actions and runs a smoke test against it, and every build of
the default branch (`TunaStreamMain`) is published as a release; see
[`build-windows.yml`](.github/workflows/build-windows.yml). To build on your own PC, follow
[docs/building.md](docs/building.md).

## License and credits

TunaStream is free software under the [GPL-3.0](LICENSE). It started from these open-source projects, and
owes them a great deal:

- [Apollo](https://github.com/ClassicOldSong/Apollo) by ClassicOldSong, which TunaStream began as a copy of:
  the virtual display support, per-device permissions and much more come from it.
- [Sunshine](https://github.com/LizardByte/Sunshine) by LizardByte, the streaming host Apollo is built on.
- [SudoVDA](https://github.com/SudoMaker/SudoVDA) by SudoMaker, the virtual display driver TunaStream installs.
- [Moonlight](https://moonlight-stream.org), whose open-source clients and work on the GameStream protocol
  make streaming like this possible. TunaStream works with Moonlight on any device.
- [Artemis](https://github.com/ClassicOldSong/moonlight-android) by ClassicOldSong, the Moonlight-based Android
  client that [TunaReceive](https://github.com/bigtunafish9943/TunaReceive), TunaStream's own client, began as.

Their authors and licenses are listed in [NOTICE](NOTICE).
