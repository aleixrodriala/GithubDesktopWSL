# GitHub Desktop WSL

Think of it like the [VS Code Remote WSL extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-wsl) but for GitHub Desktop. A fork of [GitHub Desktop](https://github.com/desktop/desktop) that makes WSL repositories work properly — **16-50x faster git operations**, working SSH keys, and no more CRLF issues.

**[Download the latest release](https://github.com/aleixrodriala/GithubDesktopWSL/releases/latest)** — installs side-by-side with official GitHub Desktop

## The problem

Official GitHub Desktop can't handle repos inside WSL. When you open a `\\wsl.localhost\...` path:

- **Git commands are unusably slow** — Desktop runs Windows `git.exe`, which accesses WSL files through the [9P protocol](https://learn.microsoft.com/en-us/windows/wsl/filesystems). Every file stat, read, and open is a round-trip across the VM boundary.
- **SSH keys don't work** — Desktop injects a Windows-only `SSH_ASKPASS` binary that breaks SSH inside WSL.
- **File operations fail** — Checking merge/rebase state, reading diffs, writing `.gitignore` — all go through 9P and are either slow or broken.
- **Deleting repos fails** — Windows Recycle Bin doesn't support WSL UNC paths.

## The solution

This fork runs a lightweight **daemon inside WSL** that executes git and file operations natively. Desktop talks to it over TCP instead of going through 9P.

```
Official Desktop (slow path):
  Desktop → git.exe → Windows kernel → 9P → VM boundary → WSL → ext4
  (every file operation is a round-trip)

This fork (fast path):
  Desktop → TCP → daemon → git (native) → ext4
  (one round-trip per command, git has direct disk access)
```

The daemon is bundled inside the installer. When you open a WSL repo, it's deployed and started automatically. There's nothing to configure.

## Why a daemon and not a git shim?

The simpler approach is to replace Desktop's bundled `git.exe` with a shim that calls `wsl.exe -e git`. Projects like [wsl-git-bridge](https://github.com/MiloudiMohamed/wsl-git-bridge) and my own earlier [wsl-git-shim](https://github.com/aleixrodriala/wsl-git-shim) do this. It works, but has limitations:

**Spawn overhead** — Every command pays ~92ms to launch `wsl.exe`, even a trivial `git rev-parse`. The daemon is a persistent process, so it has ~2ms overhead (TCP round-trip + framing). Over a typical Desktop refresh cycle (10-20 commands), that's ~1-2 seconds vs ~20-40ms.

**File operations aren't covered** — Desktop doesn't just run git. It also reads diffs, checks merge/rebase state, reads and writes `.gitignore`, checks submodule paths — 8 different files do direct filesystem operations. A git-only shim leaves all of these going through 9P. The daemon handles them natively.

**Breaks on Desktop updates** — Desktop ships its own bundled `git.exe` inside each version's Squirrel package. Every update creates a new `app-<version>` directory with a fresh `git.exe`, so a replaced shim gets wiped. This fork is a separate app — updates come from its own release channel.

| | Daemon (this fork) | Git shim |
|---|---|---|
| Per-command overhead | ~2ms | ~92ms |
| File operations (diffs, merge state, .gitignore) | Native | Still 9P |
| Survives Desktop updates | Yes | No |
| Install | Run installer | Replace git.exe manually |

## Performance

Three ways to run git on a WSL repo, benchmarked across repo sizes:

### Spawn overhead

Before any git work happens, each approach has a fixed cost:

| Approach | Overhead |
|----------|----------|
| Native git (daemon) | 1 ms |
| git.exe (9P) | 46 ms |
| wsl.exe -e (shim) | 94 ms |

### Git operations by repo size

<table>
<tr><th>Operation</th><th colspan="3">Small (21 files)</th><th colspan="3">Medium (334 files)</th><th colspan="3">Large (2,373 files)</th></tr>
<tr><th></th><th>Daemon</th><th>git.exe (9P)</th><th>Shim</th><th>Daemon</th><th>git.exe (9P)</th><th>Shim</th><th>Daemon</th><th>git.exe (9P)</th><th>Shim</th></tr>
<tr><td>git status</td><td>2 ms</td><td>50 ms (25x)</td><td>93 ms (46x)</td><td>2 ms</td><td>51 ms (25x)</td><td>93 ms (46x)</td><td>3 ms</td><td>50 ms (16x)</td><td>92 ms (30x)</td></tr>
<tr><td>git log</td><td>2 ms</td><td>50 ms (25x)</td><td>92 ms (46x)</td><td>2 ms</td><td>51 ms (25x)</td><td>91 ms (45x)</td><td>2 ms</td><td>49 ms (24x)</td><td>93 ms (46x)</td></tr>
<tr><td>git diff</td><td>2 ms</td><td>49 ms (24x)</td><td>90 ms (45x)</td><td>2 ms</td><td>48 ms (24x)</td><td>91 ms (45x)</td><td>2 ms</td><td>50 ms (25x)</td><td>92 ms (46x)</td></tr>
<tr><td>git branch</td><td>1 ms</td><td>50 ms (50x)</td><td>93 ms (93x)</td><td>1 ms</td><td>49 ms (49x)</td><td>92 ms (92x)</td><td>2 ms</td><td>49 ms (24x)</td><td>90 ms (45x)</td></tr>
<tr><td>git rev-parse</td><td>1 ms</td><td>51 ms (51x)</td><td>92 ms (92x)</td><td>1 ms</td><td>49 ms (49x)</td><td>92 ms (92x)</td><td>1 ms</td><td>49 ms (49x)</td><td>92 ms (92x)</td></tr>
<tr><td>git for-each-ref</td><td>1 ms</td><td>51 ms (51x)</td><td>96 ms (96x)</td><td>1 ms</td><td>51 ms (51x)</td><td>92 ms (92x)</td><td>2 ms</td><td>50 ms (25x)</td><td>91 ms (45x)</td></tr>
</table>

> Benchmarks: 11 iterations, median, WSL2 on Windows 11 (kernel 6.6.87.2). [Full benchmark script and results](benchmark/).

### Real Desktop workflows

These are the actual sequences Desktop runs during common operations:

| Workflow | This fork | Official Desktop | Speedup |
|----------|----------:|-----------------:|--------:|
| Open / switch to a repo | 16 ms | 173 ms | **11x** |
| Populate branch list + dates | 6 ms | 88 ms | **14x** |
| View last commit diff | 4 ms | 44 ms | **10x** |
| Pre-fetch checks | 3 ms | 82 ms | **26x** |

> The "open repo" workflow includes `git status`, `for-each-ref`, 4 `pathExists` checks, `readFile`, and `git log`.

### Why the difference is so large

`git.exe` running on a WSL repo has a ~50ms floor for *any* operation — that's the cost of accessing files through 9P across the VM boundary. A `wsl.exe -e git` shim avoids 9P but pays ~92ms to spawn `wsl.exe` per command — actually slower than 9P for typical Desktop operations. The daemon has ~2ms overhead (TCP round-trip + message framing). The actual git work is the same; the difference is entirely in how commands are dispatched and files are accessed.

A typical Desktop refresh runs 10-20 commands. With git.exe that's 500-1000ms of pure overhead. With a shim it's 920-1840ms. With the daemon it's 20-40ms.

## Install

1. Download **[GitHubDesktopWSLSetup-x64.exe](https://github.com/aleixrodriala/GithubDesktopWSL/releases/latest)**
2. Run the installer
3. Open any WSL repository

That's it. The app:
- Detects WSL paths automatically (`\\wsl.localhost\...` or `\\wsl$\...`)
- Deploys the daemon to `~/.local/bin/` in your WSL distro on first use
- Starts the daemon via `--daemonize` (forks to background)
- Reconnects automatically if the daemon crashes
- Falls through to normal Desktop behavior for Windows repos

> SmartScreen may warn on first install (the fork is unsigned). Click "More info" then "Run anyway".

## How it works

### Components

**`wsl-git-daemon`** (C, ~550 lines, zero dependencies)
A persistent daemon that runs inside WSL. Listens on TCP `127.0.0.1` on a random port. Handles:
- Git commands — forks git with proper pipes for stdout/stderr/stdin
- File operations — read, write, stat, pathExists, unlink via direct syscalls
- Security — token-based auth, localhost-only connections

**`wsl.ts`** (TypeScript, ~420 lines)
The Desktop-side client. Responsibilities:
- **Path detection** — identifies `\\wsl.localhost\...` and `\\wsl$\...` paths, extracts distro name
- **Lifecycle management** — deploy binary, start/restart daemon, health check
- **Protocol client** — binary length-prefixed frames over TCP
- **Drop-in wrappers** — `wslReadFile()`, `wslPathExists()`, etc. that route to daemon for WSL paths and fall through to native `fs` for Windows paths

**`core.ts` patch** (1 check)
All git commands in Desktop flow through `git()` in `core.ts`. A single `if (isWSLPath(path))` routes WSL repos through the daemon while leaving Windows repos completely untouched.

### Wire protocol

```
Frame: [type: 1 byte][length: 4 bytes big-endian][payload: N bytes]

Client → Daemon:
  INIT  (0x01)  JSON { token, cmd, args, cwd, stdin, path }
  STDIN (0x02)  Raw bytes (for writeFile content, empty = EOF)

Daemon → Client:
  STDOUT      (0x03)  Raw bytes (git output / file content)
  STDERR      (0x04)  Raw bytes
  EXIT        (0x05)  4-byte exit code
  ERROR       (0x06)  UTF-8 error message
  STAT_RESULT (0x07)  JSON { exists, size, isDir }
```

### Daemon lifecycle

```
User opens WSL repo
  → Desktop detects \\wsl.localhost\Ubuntu\...
  → Extracts distro name ("Ubuntu")
  → Tries to connect to daemon (reads /tmp/wsl-git-daemon.info via UNC path)
  → If not running:
      → Deploys binary: wsl.exe -d Ubuntu -e sh -c "cp ... ~/.local/bin/ && chmod 755 ..."
      → Starts daemon: wsl.exe -d Ubuntu -e sh -c "~/.local/bin/wsl-git-daemon --daemonize"
      → Daemon forks, writes info file, parent exits, wsl.exe returns
      → Desktop reads info file, connects to daemon
  → All git/file operations go through daemon
  → If connection fails mid-session → auto-restart
```

## Staying current with upstream

This project doesn't diverge from upstream GitHub Desktop. It maintains a set of **patch files** (in `patches/`) that are applied on top of each upstream release:

1. CI checks for new upstream `release-*` tags every 6 hours
2. Clones `desktop/desktop` at the new release tag
3. Applies `patches/*.patch` via `git apply`
4. Builds the daemon + app, publishes a new release
5. If patches fail to apply → opens a GitHub Issue for manual resolution

The patches are designed to be minimal and conflict-resistant: one new file (`wsl.ts`), one new directory (`wsl-daemon/`), and small surgical changes to existing files (mostly import swaps). All patches are plain text and easy to review.

## Files changed

```
NEW   wsl-daemon/daemon.c                  Persistent daemon
NEW   wsl-daemon/Makefile                  Build script
NEW   app/src/lib/wsl.ts                   Client, lifecycle, wrappers
PATCH app/src/lib/git/core.ts              Git routing (1 if-block)
PATCH app/src/main-process/main.ts         WSL delete handler
PATCH app/src/models/repository.ts         isWSL getter
PATCH app/src/lib/git/diff.ts              Import swap: wslReadFile
PATCH app/src/lib/git/rebase.ts            Import swap: wslReadFile, wslPathExists
PATCH app/src/lib/git/cherry-pick.ts       Import swap: wslReadFile, wslPathExists
PATCH app/src/lib/git/merge.ts             Import swap: wslPathExists
PATCH app/src/lib/git/description.ts       Import swap: wslReadFile, wslWriteFile
PATCH app/src/lib/git/gitignore.ts         WSL-aware read/write/unlink
PATCH app/src/lib/git/submodule.ts         Import swap: wslPathExists
PATCH app/src/lib/stores/app-store.ts      Import swap: wslPathExists
PATCH app/package.json                     Branding: "GitHub Desktop WSL"
PATCH script/dist-info.ts                  Update URL, app ID
PATCH script/build.ts                      Bundle daemon binary
PATCH script/package.ts                    Skip code signing
NEW   .github/workflows/sync-upstream.yml  Auto-sync with upstream
NEW   .github/workflows/build-release.yml  Build and publish releases
```

Most patches are one-line import changes — swapping `readFile` from `fs/promises` with `wslReadFile` from `wsl.ts`. These are unlikely to conflict with upstream changes.

## Building from source

```bash
# 1. Build daemon (in WSL)
cd wsl-daemon && make

# 2. Install dependencies (on Windows)
yarn install

# 3. Build
yarn build:dev     # development
yarn build:prod    # production

# 4. Package installer (production only)
SKIP_CODE_SIGNING=1 yarn package
```

## License

[MIT](LICENSE)

## Credits

Based on [GitHub Desktop](https://github.com/desktop/desktop) by GitHub, Inc.

WSL support by [@aleixrodriala](https://github.com/aleixrodriala).
