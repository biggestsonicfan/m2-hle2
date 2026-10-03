# m2-hle2

A **Sega Model 2 arcade emulator**. It plays *Sonic the Fighters* (1996), online too, in a
browser, on a PC, on a handheld or in RetroArch. *Fighting Vipers* boots on it as well, and so
does homebrew written for the board.

- Everything it can do: [docs/FEATURES.md](docs/FEATURES.md)
- How it works, for developers: [docs/TECHNICAL.md](docs/TECHNICAL.md)
- Every other document: [docs/README.md](docs/README.md)

## Play

**In a browser:** open **[play.sonicthefighte.rs](https://play.sonicthefighte.rs)** and give it
your ROM set. There is nothing to install. Keyboard, gamepad and touch all work, and **Online
Battle** plays other people from the page.

**On a computer or a handheld:** download the newest build from the
[`canary` release](https://github.com/biggestsonicfan/m2-hle2/releases/tag/canary). It is
rebuilt from every change.

| Download | For |
|---|---|
| `m2hle-windows-x64.zip` | Windows (64-bit) |
| `m2hle-linux-x64.zip` | Linux (64-bit) |
| `m2hle-libretro-*.zip` | the RetroArch core: Windows, Linux, macOS, Android ([how to install](packaging/libretro/INSTALL.md)) |
| `m2hle-rocknix-arm64.zip` | ROCKNIX handhelds such as the Anbernic RG ARC-S ([how to install](packaging/rocknix/README.md)) |

On Windows or Linux, unzip it and run

```
m2hle --rom sfight.zip --run
```

or start `m2hle` and use **File → Load ROMs...**.

## ROMs

You need your own dump of the arcade board. None is included, and none will be accepted into
this repository.

For *Sonic the Fighters* that is MAME's `sfight` set. Keep `schamp.zip`, the set it borrows
files from, in the same folder as `sfight.zip`, or use a merged `sfight.zip` that holds both.
The browser takes one merged `sfight.zip` or `schamp.zip`.

## Controls

The keyboard defaults are MAME's:

| | Player 1 | Player 2 |
|---|---|---|
| Move | arrow keys | I J K L |
| Punch | Z | Delete |
| Kick | X | End |
| Barrier | C | Page Down |
| Start | 1 | 2 |
| Coin | 5 | 6 |

F2 is Service and F3 is Test (the operator menu). On the desktop F9 pauses. The browser and
RetroArch take gamepads and let you rebind every button.

*Sonic the Fighters* starts as the **Console** version, the way Sega's own ports play it: free
play, and the hidden fighters at character select (press Start on Amy for **Honey**, on Sonic
for **Metal Sonic**, on Bean for **Robotnik**). `--profile sfight` runs the arcade board exactly
as it shipped. [FEATURES.md](docs/FEATURES.md) has the rest.

## Online

Matches are played peer to peer, with matchmaking through [RPCN](https://github.com/RipleyTom/rpcn).
Sign in with Twitch or an RPCN account, then host a room or join one:

- **Browser:** the **Online Battle** button.
- **Desktop:** **Netplay → Netplay window**.
- **RetroArch:** set the core's *Online play* option to RPCN, then **Online Battle** in the
  game's main menu.
- **ROCKNIX handheld:** turn *online play* on in the game's options.

Rooms hold up to eight players: two fight, the rest watch and wait their turn, and the winner
stays on. On the official RPCN server you can also play the PS3 release running in RPCS3.

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
