# Installing the m2-hle core in RetroArch

This guide covers two setups:

- [**RetroArch**](#part-1-retroarch) on Windows, Linux, macOS or Android.
- [**ROCKNIX**](#part-2-rocknix) handhelds, where the core gets its own emulator entry for
  Sega Model 2 in EmulationStation.

m2-hle emulates the Sega Model 2 arcade board. The game it runs today is **Sonic The Fighters**.

## What you need

- **RetroArch.** A recent version: this guide was tested with 1.22.2.
- **The ROM sets, from your own copy of the game.** Both files go in the same folder, and they're
  never included with the emulator:
  - `sfight.zip`: Sonic The Fighters, the MAME set.
  - `schamp.zip`: Sonic Championship, the parent set. `sfight` is a clone of it and needs files
    from it.
- **A graphics driver with OpenGL.** GL 4.1 on a computer, GLES 3 on Android and handhelds. Most
  devices from the last ten years have it.

## Downloading the core

Take the zip for your platform from the canary release:

`https://github.com/biggestsonicfan/m2-hle2/releases/download/canary/<zip name>`

| Zip | For |
|---|---|
| `m2hle-libretro-windows-x64.zip` | RetroArch on Windows |
| `m2hle-libretro-linux-x64.zip` | RetroArch on desktop Linux |
| `m2hle-libretro-macos-universal.zip` | RetroArch on macOS (Apple Silicon and Intel) |
| `m2hle-libretro-android-arm64.zip` | RetroArch on Android |
| `m2hle-libretro-linux-arm64.zip` | ARM Linux handhelds: ROCKNIX, Knulli, muOS, ArkOS and others |

Each zip holds the core, `m2hle_libretro.info` (so RetroArch knows the core's name and what it
needs), the README and this guide. The ARM Linux zip also has `install-rocknix.sh`.

---

## Part 1: RetroArch

### Install

**Windows, Linux and macOS**

1. In RetroArch, open **Settings > Directory**. Note two folders: **Cores** and
   **Core Info**.
2. Copy the core into the Cores folder:
   - Windows: `m2hle_libretro.dll`
   - Linux: `m2hle_libretro.so`
   - macOS: `m2hle_libretro.dylib`
3. Copy `m2hle_libretro.info` into the Core Info folder.
4. Restart RetroArch.
5. **macOS only:** the core isn't signed. If macOS blocks it, allow it in
   **System Settings > Privacy & Security** and start RetroArch again.

**Android**

1. Copy `m2hle_libretro_android.so` somewhere on the device, for example `Download`.
2. In RetroArch: **Load Core > Install or Restore a Core**, then pick the file.

### Set the video driver

The core draws with OpenGL. If RetroArch is set to Vulkan or Direct3D, it normally switches to
its GL driver for this core by itself. If you get an error or a black screen instead, set
**Settings > Drivers > Video** to `glcore` (or `gl`) and restart RetroArch.

### Start the game

1. **Load Content** and pick `sfight.zip`. Keep `schamp.zip` in the same folder.
2. If RetroArch asks which core to use, choose **Sega - Model 2 (m2-hle)**.

The first start takes a few seconds while the game loads its textures.

### Controls

| RetroPad | Cabinet |
|---|---|
| D-pad or left stick | Joystick |
| B | Button 1 |
| A | Button 2 |
| Y | Button 3 |
| X | Button 4 |
| Start | Start |
| Select | Insert coin |
| L3 | Test (operator menu) |
| R3 | Service |

Controller port 1 is player 1 and port 2 is player 2. Remap buttons in
**Quick Menu > Controls** as with any other core.

To play, press **Select** to insert a coin, then **Start**.

### Core options

Open **Quick Menu > Core Options** while the game runs.

| Option | Choices | What it does | Takes effect |
|---|---|---|---|
| Internal resolution | Native, Double, Triple, Quadruple, Full screen | The size the game is drawn at. Native is the arcade's own 496x384. Full screen draws at your window's or screen's size. | at once |
| Draw rate | 60, 30 | 30 draws every second frame: half the graphics work, cooler on a handheld. The game still runs at 60. | at once |
| Heat guard | off, 80, 85, 90 °C | Above that temperature, draws every second frame until the device has cooled 5 degrees. Linux and handhelds only. | at once |
| Sound board | enabled, disabled | Disabled is silent and uses less power. | next time you load the game |
| Online play | RetroArch, RPCN | Which netplay to use (see below). | next time you load the game |
| RPCN sign-in | Signed out, Sign in using Twitch, RPCN account | Only shown with Online play set to RPCN. | at once |
| Input delay (frames) | 1 to 8 | For matches you host. Higher hides more network lag. | the next match you host |

Savestates, rewind and run-ahead don't work with this core. RetroArch says the core doesn't
support save states if you try.

### Online play

Two ways to play online. Both start both players' games from power-on at the same moment, then
keep them in step.

**RetroArch netplay** (the default)

Use RetroArch's own **Netplay** menu:

- **Host:** load the game, then **Netplay > Host**. Set a password in RetroArch's netplay settings
  if you want one.
- **Join:** find the game in RetroArch's lobby, or connect to the host's address.

The host is player 1 and the first player to join is player 2. Both games restart when player 2
joins. Both players need **the same build** of the core; RetroArch refuses a mismatch.

**RPCN** (plays against the m2-hle desktop emulator and play.sonicthefighte.rs)

1. Set **Online play** to **RPCN**, then close and reload the game.
2. Set **RPCN sign-in**:
   - **Sign in using Twitch:** a notification shows a twitch.tv/activate address and a code.
     Open the address on any phone or computer and enter the code. Later launches sign in by
     themselves.
   - **RPCN account:** cores can't show a text box, so the account is entered as a cheat code.
     In **Quick Menu > Cheats**, add a new cheat, set its code to
     `rpcn:NAME:PASSWORD:TOKEN`, and apply the changes. TOKEN is the one RPCN e-mailed you when
     you registered; leave `:TOKEN` off if your server doesn't use one. The password can't
     contain `:`.
3. Press **L1 and R1 together** in game to open the lobby. From there you can host a room, join
   one, and start the match. The d-pad moves, the bottom face button picks, and the right face
   button closes.

Keep in mind:

- **One Twitch sign-in per account.** RPCN keeps a single Twitch login per account, so signing in
  with Twitch here signs out any other copy of m2-hle using that account with Twitch.
- **Cheats are saved in plain text.** RetroArch writes cheat codes to a text file, so the password
  from an RPCN-account cheat ends up there. Use a password you don't use anywhere else.
- **No RPCN on Android.** Its sign-in needs the system's OpenSSL, which Android doesn't have.
  RetroArch netplay works there.

---

## Part 2: ROCKNIX

The setup gives the core its own entry under **Sega Model 2** in EmulationStation. You can pick it
per game, or make it the default for the whole system. The other Model 2 emulators stay
available.

### Before you start

- **The zip:** `m2hle-libretro-linux-arm64.zip` (not the Android one).
- **ssh:** enable it on the device (**Network settings > Enable SSH** on ROCKNIX). You'll need the
  device's IP address and root password.
- **The ROMs:** copy `sfight.zip` and `schamp.zip` into `/storage/roms/segamodel2/`. Over the
  network, that's the `roms` share's `segamodel2` folder.

### Quick install

From a computer:

```
scp m2hle-libretro-linux-arm64.zip root@<device-ip>:/storage/
ssh root@<device-ip>
cd /storage && mkdir -p m2hle-libretro && cd m2hle-libretro
unzip -o ../m2hle-libretro-linux-arm64.zip
bash install-rocknix.sh --make-default
```

- **Leave off `--make-default`** to only add the entry; then pick it per game (see below).
- **Updating later:** unpack the new zip and run the script again. The core is replaced (the
  previous one is kept as a backup) and nothing else changes.
- **After installing without `--make-default`:** restart EmulationStation (**Quit > Restart
  EmulationStation**, or reboot) so it reads its new settings. With `--make-default` the script
  restarts it for you.

The script does exactly the steps in the next section.

### What the installer changes

Do these by hand if you'd rather not run a script, or use them to check what it did. Each file
edited gets a timestamped `.bak-` copy first.

**1. Install the core.** Copy the core and its info file into `/tmp/cores`:

```
install -m 755 m2hle_libretro.so   /tmp/cores/
install -m 644 m2hle_libretro.info /tmp/cores/
```

`/tmp/cores` is where ROCKNIX's RetroArch looks for cores. It's an overlay: whatever you add is
really stored in `/storage/cores`, so it survives reboots and ROCKNIX updates. Don't write into
`/usr/lib/libretro`, which is read-only.

**2. Add the emulator entry.** In `/storage/.emulationstation/es_systems.cfg`, find the system
whose `<name>` is `segamodel2`, and add this block right after its `<emulators>` line:

```xml
			<emulator name="retroarch">
				<cores>
					<core>m2hle</core>
				</cores>
			</emulator>
```

The core's name, `m2hle`, is its file name without `_libretro.so`.

**3. Allow netplay from EmulationStation.** In `/storage/.emulationstation/es_features.cfg`, find
`<emulator name="retroarch" ...>` and add this right after the `<cores>` line that follows it:

```xml
      <core name="m2hle" features="netplay" />
```

This lets EmulationStation offer RetroArch's netplay for the core. Leave out `rewind` and
`autosave`: they need savestates, which this core doesn't have.

**4. Keep the core's files out of the ROM folder.** ROCKNIX's RetroArch saves into the ROM folder,
which is usually shared on your network, and the core keeps its RPCN login with its saves.
Create `/storage/.config/retroarch/config/m2-hle/m2-hle.cfg` containing:

```
savefiles_in_content_dir = "false"
```

The core's files then go to `/storage/.config/retroarch/saves/`, which only root can read. This
applies to this core only.

**5. Make it Model 2's default (optional; `--make-default` does this).** The Model 2 settings are
in `/storage/.config/system/configs/system.cfg`. There are two catches, and each one silently
undoes the change:

- **EmulationStation rewrites the file when it exits.** So stop it before editing, and start it
  again afterwards.
- **ROCKNIX restores the file from `system.cfg.backup` after an unclean shutdown.** That means a
  crash, a freeze or pulling the battery. So refresh the backup after editing, or the first crash
  quietly puts the old emulator back.

```
systemctl stop essway
sed -i 's/^segamodel2\.emulator=.*/segamodel2.emulator=retroarch/; s/^segamodel2\.core=.*/segamodel2.core=m2hle/' \
    /storage/.config/system/configs/system.cfg
grep -q '^segamodel2\.emulator=' /storage/.config/system/configs/system.cfg || echo 'segamodel2.emulator=retroarch' >> /storage/.config/system/configs/system.cfg
grep -q '^segamodel2\.core=' /storage/.config/system/configs/system.cfg || echo 'segamodel2.core=m2hle' >> /storage/.config/system/configs/system.cfg
/usr/bin/chksysconfig backup
sync
systemctl start essway
```

The same thing is available from EmulationStation's menus, which avoid both catches. See below.

### Choosing the emulator in EmulationStation

**For the whole system:** EmulationStation's main menu > **Game settings** > **Per-system advanced
configuration** > **Sega Model 2** > **Emulator: RetroArch, Core: m2hle**.

**For one game:** on the game in the list, open its options (the button EmulationStation shows
for options on the help bar), then **Advanced game options** > **Emulator: RetroArch,
Core: m2hle**.

Menu names can differ slightly between ROCKNIX versions.

### Recommended settings for a handheld

**In EmulationStation:** turn on **Integer scale** for Sega Model 2, under per-system advanced
configuration or in `system.cfg` as `segamodel2.integerscale=1`. The picture is then shown pixel
for pixel instead of stretched.

**In RetroArch's core options** (**Quick Menu > Core Options**):

- **Internal resolution:** **Native** to go with integer scale, or **Full screen** to fill the
  screen at the core's own resolution. Full screen draws about 1.5 times as many pixels as
  Native on a 640x480 screen, so it runs warmer.
- **Heat guard:** **85 °C** (the handheld build's default). An RG ARC-S reached 83 °C in two
  minutes with the sound board on and began to throttle; the guard eases the load before it gets
  hotter.
- **Sound board:** your choice. Off saves power and heat. An online match turns it on
  automatically, because both players' games have to run the same hardware.
- **Draw rate:** **30** if the device still runs hot.

**Per-game options:** RetroArch can keep separate core options per game, in
`/storage/.config/retroarch/config/m2-hle/sfight.opt`. If a setting doesn't seem to stick, check
there as well as `m2-hle.opt` beside it.

### Online play on ROCKNIX

- **RetroArch netplay:** start a match from EmulationStation's netplay settings or RetroArch's
  Netplay menu, as with any other core (step 3 above is what enables it). The other player needs
  the same build of the core.
- **RPCN:** follow the RPCN steps in Part 1. The easiest sign-in on a handheld is
  **Sign in using Twitch**: the code appears on screen and you approve it on your phone. The
  login is saved in `/storage/.config/retroarch/saves/m2hle-rpcn.cfg`.
  - Give the handheld **its own RPCN account.** An account can only be signed in once, so if your
    computer or a bot is already signed in with it, the handheld can't be.
  - If an RPCN sign-in already exists on this handheld in `/storage/.config/m2hle2/netplay.cfg`
    (from the older standalone build), the core copies it once the first time you use RPCN.
- **Input logs:** every RPCN match saves an input log next to the login file, named
  `netplay-<date>-s<session>-p<player>.inputs`. If two players saw different games, that file is
  what finds out why.

### Troubleshooting

| What you see | What to do |
|---|---|
| The game still starts in the old emulator | Restart EmulationStation. It only reads its settings at startup. |
| It went back to the old emulator after a freeze or crash | ROCKNIX restored `system.cfg` from its backup. Set the emulator again and run `/usr/bin/chksysconfig backup` (or use the EmulationStation menus). |
| Black screen, but the picture shows behind RetroArch's menu | Update the core. Early test builds left GL state behind that RetroArch then drew with. |
| **RetroArch / m2hle** isn't in the emulator list | Check step 2, then restart EmulationStation. |
| "Could not load" or the core is missing | Check that `/storage/cores/m2hle_libretro.so` exists, and that you installed the ARM Linux zip, not Android's. |
| The device gets very hot | Keep Heat guard on, set Draw rate to 30, or turn the sound board off. |
| Something else | `/var/log/exec.log` holds the log of the last game launched. It's cleared on reboot, so copy it off before restarting. |

### Going back

To return Model 2 to its previous emulator:

1. Set it back in EmulationStation's per-system configuration, or edit `segamodel2.emulator` and
   `segamodel2.core` in `system.cfg` as in step 5, with EmulationStation stopped.
2. Then run `/usr/bin/chksysconfig backup`.

To remove the core completely, also:

1. Delete `/storage/cores/m2hle_libretro.so` and `/storage/cores/m2hle_libretro.info`.
2. Remove the two entries from steps 2 and 3, or restore the `.bak-` copies the installer made.
3. Restart EmulationStation.

### Files on ROCKNIX

| File | What it is |
|---|---|
| `/storage/cores/m2hle_libretro.so` | the core (installed through `/tmp/cores`) |
| `/storage/cores/m2hle_libretro.info` | the core's description for RetroArch |
| `/storage/.emulationstation/es_systems.cfg` | holds the Sega Model 2 emulator entry (step 2) |
| `/storage/.emulationstation/es_features.cfg` | lets ES offer netplay for the core (step 3) |
| `/storage/.config/system/configs/system.cfg` | EmulationStation's per-system settings (step 5) |
| `/storage/.config/retroarch/config/m2-hle/m2-hle.cfg` | keeps the core's saves out of the ROM folder (step 4) |
| `/storage/.config/retroarch/config/m2-hle/m2-hle.opt` | the core options |
| `/storage/.config/retroarch/config/m2-hle/sfight.opt` | the core options for Sonic The Fighters only, if set per game |
| `/storage/.config/retroarch/saves/m2hle-rpcn.cfg` | your RPCN login, private to root |
| `/storage/roms/segamodel2/` | `sfight.zip` and `schamp.zip` |
