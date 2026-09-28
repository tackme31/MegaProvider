# MegaProvider

A PowerShell provider that exposes [MEGA](https://mega.io/) cloud storage as a `mega:` drive.
Browse it with `Set-Location` and `Get-ChildItem`, and change it with the standard cmdlets — `Rename-Item`,
`Move-Item`, `Copy-Item`, `Remove-Item`, `New-Item` — including in pipelines, so bulk jobs are one line:

```powershell
Get-ChildItem mega:\photos -Recurse -Filter *.jpg `
    | Rename-Item -NewName { $_.Name -replace '^IMG_', 'trip_' }
```

MegaProvider talks to MEGA through the official [MEGA C++ SDK](https://github.com/meganz/sdk), which
runs in a small background process (`megaprovider-host`) so that the file list is loaded once, not
on every command.

**Note:** This is a personal project, published in case it's useful to someone else, and it's still
before 1.0, so anything below may change between versions without a compatibility layer — property
names, cmdlets and parameters included. It isn't affiliated with MEGA Limited and comes with no
warranty (see [LICENSE](LICENSE)). It changes your real cloud storage, so use it at your own risk.

## Requirements

- Windows x64, or Linux x64 with glibc 2.35 or later (Ubuntu 22.04 or later, for example)
- PowerShell 7.4 or later (not Windows PowerShell 5.1)

macOS is not supported.

## Installation

### Windows

1. Download `MegaProvider-<version>-win-x64.zip` from the
   [Releases](https://github.com/tackme31/MegaProvider/releases) page.
2. Unpack it into your module folder and unblock the files:

   ```powershell
   $version = '<version>'   # e.g. 0.1.0
   $dest = "$HOME\Documents\PowerShell\Modules\MegaProvider\$version"
   Expand-Archive "MegaProvider-$version-win-x64.zip" $dest
   Get-ChildItem $dest | Unblock-File
   ```

### Linux

1. Download `MegaProvider-<version>-linux-x64.tar.gz` from the
   [Releases](https://github.com/tackme31/MegaProvider/releases) page.
2. Unpack it into your module folder (in `pwsh`):

   ```powershell
   $version = '<version>'   # e.g. 0.2.0
   $dest = "$HOME/.local/share/powershell/Modules/MegaProvider/$version"
   New-Item -ItemType Directory -Force $dest | Out-Null
   tar -xzf "MegaProvider-$version-linux-x64.tar.gz" -C $dest
   ```

### Then

Import the module and log in:

```powershell
Import-Module MegaProvider
Connect-MegaAccount (Get-Credential)
Set-Location mega:\
```

The login is remembered: later sessions only need `Import-Module MegaProvider`
(add it to your `$PROFILE` to skip even that).

To uninstall, run `Disconnect-MegaAccount`, stop the background process
(`Get-Process megaprovider-host | Stop-Process`), and delete the module folder and the data folder
(`%LOCALAPPDATA%\MegaProvider` on Windows, `~/.local/share/MegaProvider` on Linux).

## Commands

### Account

| Command | What it does |
|---|---|
| `Connect-MegaAccount [-Credential] <PSCredential> [-AuthCode <code>]` | Logs in and remembers the session. For accounts with two-factor authentication, give the authenticator code with `-AuthCode`, or you are asked for it. Connecting again replaces the account (one at a time). |
| `Get-MegaAccount` | Shows the account you are connected to. |
| `Disconnect-MegaAccount` | Logs out (the session is invalidated on MEGA's side too) and forgets it. |

The session is stored in `session.dat` in the data folder. On Windows it is encrypted for your Windows
user (DPAPI); on Linux it is a file only you can read, in a folder only you can open (as MEGAcmd does it).
Your password is never stored.

### Standard cmdlets on `mega:`

| Cmdlet | Behaviour |
|---|---|
| `Set-Location` (`cd`), `Get-Item`, `Test-Path`, `Resolve-Path`, tab completion | As on a local drive. If no name matches exactly, a case-insensitive match is used (MEGA names are case-sensitive). |
| `Get-ChildItem` (`dir`, `gci`) | Supports `-Recurse`, `-Filter`, `-Name`, `-File`, `-Directory`. |
| `Rename-Item` | Refuses a name that another item in the same folder already has. |
| `Move-Item` | The destination must be an existing folder. Refuses if it already holds an item of the same name. |
| `Copy-Item` | Within `mega:` only. Refuses if the destination already holds the name. A folder needs `-Recurse` and is copied with everything in it. |
| `Remove-Item` | **Moves the item to the Rubbish Bin.** MegaProvider never deletes permanently; use `Restore-MegaItem` to undo. |
| `New-Item -ItemType Directory` | Creates a folder. |

`-WhatIf` and `-Confirm` work on every command that changes something.

On Linux, `ls` is the system command, not an alias of `Get-ChildItem` (PowerShell keeps `ls`, `cp`, `mv`,
`rm` and `cat` for the system there), so it does not see `mega:`. Use `dir` or `gci`. Paths are shown with
`/` on Linux; both `mega:\photos` and `mega:/photos` work on either system.

### Transfers and the Rubbish Bin

| Command | What it does |
|---|---|
| `Send-MegaItem [-Path] <local> [-Destination] <mega:\folder>` | Uploads files or folders. Uploading a file whose name already exists adds a new version of it. |
| `Receive-MegaItem [-Path] <mega:\...> [[-Destination] <local folder>]` | Downloads files or folders (to the current directory by default). |
| `Get-MegaRubbishItem [[-Name] <wildcard>]` | Lists what is in the Rubbish Bin. |
| `Restore-MegaItem [-Handle] <handle> [-Destination <mega:\folder>]` | Puts an item back where it was removed from, or into `-Destination`. |

Transfers show a progress bar and can be cancelled with Ctrl+C. `Copy-Item` cannot copy between `mega:`
and a local drive (PowerShell does not allow that across providers), which is what `Send-` and
`Receive-MegaItem` are for. They take pipeline input:

```powershell
Get-ChildItem ~/photos/*.jpg | Send-MegaItem -Destination mega:\photos
Get-ChildItem mega:\docs -File | Receive-MegaItem -Destination ~/backup
Get-MegaRubbishItem report* | Restore-MegaItem
```

## Cautions

### Bulk operations

MegaProvider makes it easy to change hundreds of items with one pipeline. MEGA limits how fast an
account may send requests, and there is no undo for renames and moves.

- **Preview first.** Run the pipeline with `-WhatIf` and check what it would do.
- **Keep runs to a sensible size.** Split very large jobs instead of changing thousands of items at once.
  Do not use MegaProvider to stress-test or benchmark MEGA's servers.
- Requests are sent one at a time. When MEGA refuses one with EAGAIN (rate limiting), MegaProvider
  waits and retries up to 4 times (0.5 to 4 seconds apart).
- If it is still refused, **the whole pipeline stops** at that item, with an error naming it: the items
  before it were done, it and the rest were not. Wait a few minutes, then run the pipeline again for the
  rest.
- While MEGA is refusing a whole batch of requests, the MEGA SDK keeps retrying on its own, so a command
  can seem to hang for a while without any message.

### Items with the same name

MEGA allows several items with the same name in one folder, so a path can match more than one item.
Listing and reading show the first match. Changing or transferring through such a path is refused with
an "ambiguous" error, so that a bulk job cannot hit the wrong item; rename one of them in another MEGA
client first. Tab completion does not work in such a folder (a PowerShell limitation).

### Other

- **Updating:** the background process stays up for up to an hour after the last command. Stop it
  (`Get-Process megaprovider-host | Stop-Process`) before installing a new version.
- This is an unofficial tool, not affiliated with or endorsed by MEGA.

## Building from source

Needs the .NET 8 SDK or later, PowerShell 7.4 or later, and a C++ toolchain:

- **Windows:** Visual Studio 2022 with the C++ workload (including the v142 toolset).
- **Linux:** GCC 11 or later and the tools vcpkg needs to build the SDK's dependencies. On Ubuntu:

  ```sh
  sudo apt install build-essential cmake ninja-build pkg-config autoconf autoconf-archive automake libtool curl zip unzip
  ```

```powershell
git clone --recurse-submodules https://github.com/tackme31/MegaProvider.git
cd MegaProvider
native/third_party/vcpkg/bootstrap-vcpkg.bat   # on Linux: native/third_party/vcpkg/bootstrap-vcpkg.sh
./scripts/dev.ps1          # builds everything and opens a pwsh with the module loaded
./scripts/test.ps1         # runs the tests that need no account
```

The first native build compiles the MEGA SDK and its dependencies through vcpkg and takes a while
(about half an hour on Linux). On Linux it compiles as many files at once as there are cores; if memory
runs short (under WSL, for example), set `$env:CMAKE_BUILD_PARALLEL_LEVEL = 8` before running `dev.ps1`.

## License

MegaProvider is licensed under the [MIT License](LICENSE).

Each release package also contains `THIRD-PARTY-NOTICES.txt` for the components linked into
`megaprovider-host`, including the MEGA C++ SDK (BSD 2-Clause), nlohmann/json (MIT) and the
libraries they depend on.

## Author

Takumi Yamada ([@tackme31](https://github.com/tackme31))
