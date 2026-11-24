# TunaStream troubleshooting

Short answers for the three things that most often go wrong: controller rumble, the connection, and what happens
when a stream stops and starts. Every section tells you which line to look for in the log, which you can read on
the **Troubleshooting** page of the web interface.

## Controller rumble

Rumble travels: game → virtual gamepad on the PC → TunaStream → network → client app → your controller. Find out
which half is broken first.

1. Start a stream with the controller connected.
2. On the **Troubleshooting** page, press **Rumble now**.

**The controller buzzed.** TunaStream, the network and the client are fine. The game is not sending rumble to the
virtual gamepad.

- Set **Settings > Controls > Controller type the PC sees** to **Xbox 360** and reconnect the controller. Most PC games
  only rumble an Xbox controller. By default, controllers with a touchpad (PlayStation controllers) are emulated as
  a PlayStation 4 controller, which fewer games rumble. Controllers with only motion sensors, such as 8BitDo and
  Switch Pro controllers, stay Xbox 360.
- Check the game's own vibration setting.
- If the game runs through Steam, check Steam Input for that game: it can replace the gamepad the game sees.
- The log says which type was chosen, for example `Gamepad 0 will be Xbox 360 controller`.

**Nothing happened.** The game is not the problem. Look at the client side.

- Check that rumble is turned on in the client app's settings.
- Controllers connected to the client over Bluetooth only rumble if the device's operating system supports
  rumble for that controller. Try the same controller with a cable, or in another app on that device.
- Make sure **Settings > Controls > Rumble** is on.
- The log should show `Rumble test sent to 1 client(s)`. If it says no device is streaming, the stream was not
  running when you pressed the button.

### 8BitDo Ultimate 2

- **On an Android TV box (such as the NVIDIA Shield), connect it over Bluetooth in D mode.** Its 2.4G adapter is
  meant for Windows PCs: plugged into a TV box it is detected but doesn't control anything.
- Over Bluetooth, some Android TV boxes don't pass rumble to 8BitDo controllers. **Rumble now** on the
  Troubleshooting page shows whether yours does.
- **Pick "Xbox-style controller" in setup**, or set the controller type to **Xbox 360**. Its gyro used to make
  automatic mode choose a PlayStation 4 controller.
- **Back paddles and L4/R4** reach the PC only as the buttons they're mapped to. Map them to existing buttons in
  8BitDo Ultimate Software if you want them in games.
- **Update its firmware** with 8BitDo Ultimate Software; rumble and connection fixes arrive that way.

## The connection

**The client cannot find the PC.**

- Add it by hand using the PC's IP address.
- If TunaStream is installed next to another streaming host it uses port 48989, and the address needs it:
  `192.168.1.20:48989`. The home page says so when this applies.
- Both devices must be on the same network. Guest Wi-Fi networks usually block this.

**It connects, then stutters or drops.**

- Use 5 GHz Wi-Fi or a cable for the PC. Wired on the PC end makes the biggest difference.
- Lower the bitrate in the client before changing anything on the PC. In TunaReceive, *Fit picture quality to
  this connection* (in the PC's menu) measures the connection and suggests one.
- A stream that drops and comes back shows `CLIENT DISCONNECTED (connection lost)` in the log. One that the
  client ended on purpose shows `CLIENT DISCONNECTED`.

**The stream shows the wrong screen, or a blank desktop.**

- TunaStream streams a virtual monitor and makes it the main display while someone is streaming, so games open
  there. The log shows `Virtual Display created at` followed by `Virtual Display set as the primary display`.
- Programs that remember their monitor, like Steam's Big Picture, are moved onto it for three minutes after the
  app starts. The log shows `Moved a window onto the stream's screen` for each one.
- `Virtual Display creation failed` means the display driver did not respond. The home page shows *Driver
  problem*. Restart TunaStream from the Troubleshooting page; if it persists, reinstall TunaStream.
- To stream your real monitor instead, turn off **Give each stream its own screen** under
  **Settings > Screen and sound**.

## Stopping and starting

What TunaStream does depends on how the stream ended.

| What happened | What TunaStream does |
|---|---|
| You chose **Quit** in the client, or pressed **End session** on the home page | Quits the app, removes the virtual monitor, your monitor is the main display |
| You closed the client without quitting | Keeps the app running. Your monitor becomes the main display again. Reconnect to carry on where you left off |
| Same, with **End session when the last client disconnects** on | Quits the app and removes the virtual monitor straight away |
| The connection dropped (Wi-Fi, device went to sleep) with that setting on | Keeps the app running for two minutes so you can reconnect, then ends the session |

- **My monitor did not come back as the main display.** Look for `Primary display handed back to` in the log.
  If it is missing or says `Couldn't`, press Win+P and choose *PC screen only*, then tell us in an issue.
- **The client says an app is already running.** Reconnect to resume it, or press **End session** on the home
  page.
- **A game is still running after everyone left.** That is the default. Turn on **Quit the game when the last
  device disconnects** under **Settings > General**, or *Terminate on Pause* for one app.
