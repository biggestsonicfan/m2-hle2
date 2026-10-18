# m2-hle2

A **Sega Model 2 arcade emulator**. It plays *Sonic the Fighters* (1996), online too, in a
browser, on a PC, on a handheld or in RetroArch. *Sega Rally Championship*, *Virtual-On*,
*Dynamite Cop*, *Fighting Vipers*, *The House of the Dead*, *Daytona USA* and *Dead or Alive*
run on it as well, and so does homebrew written for the board.

- Everything it can do: [docs/FEATURES.md](docs/FEATURES.md)
- How it works, for developers: [docs/TECHNICAL.md](docs/TECHNICAL.md)
- Every other document: [docs/README.md](docs/README.md)

## Play

**In a browser:** open **[arcade.sonicthefighte.rs](https://arcade.sonicthefighte.rs)**, pick a game and
give it your ROM set. There is nothing to install. Keyboard, gamepad and touch all work, and **Online
Battle** plays other people from the page.

**On a computer or a handheld:** download the newest build from the
[`canary` release](https://github.com/biggestsonicfan/m2-hle2/releases/tag/canary). It is
rebuilt from every change.

| Download | For |
|---|---|
| `m2hle-windows-x64.zip` | Windows (64-bit) |
| `m2hle-linux-x64.zip` | Linux (64-bit) |
| `m2hle-libretro-*.zip` | the RetroArch core: Windows, Linux, macOS, Android ([how to install](packaging/libretro/INSTALL.md)) |
| `m2hle-libretro-linux-arm64.zip` | the RetroArch core for ROCKNIX handhelds such as the Anbernic RG ARC-S, with its installer ([how to install](packaging/libretro/INSTALL.md)) |
| `m2hle-web.zip` | the website, to host yourself |

On Windows or Linux, unzip it and run

```
m2hle --rom sfight.zip --run
```

or start `m2hle` and use **File → Load ROMs...**.

## ROMs

You need your own dump of the arcade board. None is included, and none will be accepted into
this repository.

Each game takes MAME's set of the same name. For *Sonic the Fighters* that is `sfight`: keep
`schamp.zip`, the set it borrows files from, in the same folder as `sfight.zip`, or use a merged
`sfight.zip` that holds both. The same goes for any clone set and its parent (`vonj.zip` with
`von.zip`), and *Daytona USA* needs the I/O board's `model1io.zip` beside `daytona.zip`. In the
browser, pick the zips together.

## Controls

The keyboard defaults are based on the Model 2 Emulator's (m2emulator), with changes. With
*Sonic the Fighters*' buttons:

| | Player 1 | Player 2 |
|---|---|---|
| Move | arrow keys | I J K L |
| Punch | Z | Delete |
| Kick | X | End |
| Barrier | C | Page Down |
| Start | 1 | 2 |
| Coin | 5 | 6 |

On the desktop F2 is Service, F3 is Test (the operator menu) and F9 pauses. The browser and
RetroArch take gamepads and let you rebind every button.

*Sonic the Fighters* starts as the **Console** version, the way Sega's own ports play it: free
play, and the hidden fighters at character select (press Start on Amy for **Honey**, on Sonic
for **Metal Sonic**, on Bean for **Robotnik**). `--profile sfight` runs the arcade board exactly
as it shipped. [FEATURES.md](docs/FEATURES.md) has the other games' controls (light guns, twin
sticks, wheels) and the rest.

## Online

Matches are played peer to peer, with matchmaking through [RPCN](https://github.com/RipleyTom/rpcn).
Sign in with Twitch or an RPCN account, then host a room or join one:

- **Browser:** the **Online Battle** button.
- **Desktop:** **Netplay → Netplay window**.
- **RetroArch** (handhelds too): set the core's *Online play* option to RPCN, then **Online
  Battle** in the game's main menu.

Rooms hold up to eight players: two fight, the rest watch and wait their turn, and the winner
stays on. On the official RPCN server you can also play the PS3 release running in RPCS3.

The browser, desktop and RetroArch builds can also play with rollback, through
[GGPO](https://github.com/pond3r/ggpo) and its own lobby: the browser's **GGPO lobby** button,
the desktop's `--ggpo-*` options, or the RetroArch core's *Online play* option.
[FEATURES.md](docs/FEATURES.md#online-play) has the details.

## Building it yourself

```
git clone --recurse-submodules https://github.com/biggestsonicfan/m2-hle2
python -m pip install ply==3.11                  # dear_bindings generates the ImGui C bindings
cmake -S m2-hle2 -B m2-hle2/build
cmake --build m2-hle2/build --config Release -j
```

The other frontends (browser, handheld, RetroArch core) and the tests are in
[docs/TECHNICAL.md](docs/TECHNICAL.md).

## Licence

The dependencies under [vendor/](vendor/) keep their own licences. No licence has been chosen
for the first-party code yet.
