# GitHub Desktop WSL

Think of it like the [VS Code Remote WSL extension](https://marketplace.visualstudio.com/items?itemName=ms-vscode-remote.remote-wsl) but for GitHub Desktop. A fork of [GitHub Desktop](https://github.com/desktop/desktop) that makes WSL repositories work properly — **6-27x faster git operations**, working SSH keys, and no more CRLF issues.

**[Download the latest release](https://github.com/aleixrodriala/GithubDesktopWSL/releases/latest)**

<p align="center">
  <img src="banner.png" alt="GitHub Desktop WSL — 2ms git status, 16ms open repo, 20x faster" width="640">
</p>

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

## Performance

**6x-27x faster** across all git operations compared to official Desktop on WSL repos.

### Git operations by repo size

<table>
<tr><th>Operation</th><th colspan="2">Small (20 files)</th><th colspan="2">Medium (333 files)</th><th colspan="2">Large (2,372 files)</th></tr>
<tr><th></th><th>Daemon</th><th>git.exe</th><th>Daemon</th><th>git.exe</th><th>Daemon</th><th>git.exe</th></tr>
<tr><td>git status</td><td>2 ms</td><td>43 ms (20x)</td><td>3 ms</td><td>41 ms (15x)</td><td>7 ms</td><td>41 ms (6x)</td></tr>
<tr><td>git log -20</td><td>2 ms</td><td>41 ms (24x)</td><td>3 ms</td><td>41 ms (16x)</td><td>3 ms</td><td>41 ms (14x)</td></tr>
<tr><td>git diff HEAD~1</td><td>2 ms</td><td>41 ms (25x)</td><td>2 ms</td><td>41 ms (20x)</td><td>3 ms</td><td>43 ms (14x)</td></tr>
<tr><td>git branch -a</td><td>2 ms</td><td>44 ms (23x)</td><td>2 ms</td><td>40 ms (18x)</td><td>3 ms</td><td>51 ms (20x)</td></tr>
<tr><td>git rev-parse</td><td>2 ms</td><td>42 ms (27x)</td><td>2 ms</td><td>40 ms (26x)</td><td>2 ms</td><td>41 ms (27x)</td></tr>
<tr><td>for-each-ref</td><td>2 ms</td><td>42 ms (26x)</td><td>3 ms</td><td>40 ms (12x)</td><td>4 ms</td><td>41 ms (11x)</td></tr>
<tr><td><strong>Average speedup</strong></td><td colspan="2"><strong>24x</strong></td><td colspan="2"><strong>18x</strong></td><td colspan="2"><strong>16x</strong></td></tr>
</table>

### Real Desktop workflows

These are the actual sequences Desktop runs during common operations:

| Workflow | This fork | Official Desktop | Speedup |
|----------|----------:|-----------------:|--------:|
| Open / switch to a repo | 16 ms | 173 ms | **11x** |
| Populate branch list + dates | 6 ms | 88 ms | **14x** |
| View last commit diff | 4 ms | 44 ms | **10x** |
| Pre-fetch checks | 3 ms | 82 ms | **26x** |

> Benchmarks: 7 iterations, median, WSL2 on Windows 11. The "open repo" workflow includes `git status`, `for-each-ref`, 4 `pathExists` checks, `readFile`, and `git log`.

### Why the difference is so large

`git.exe` running on a WSL repo has a ~40ms floor for *any* operation — that's the cost of launching `git.exe` through 9P and resolving paths across the VM boundary. Our daemon has ~2ms overhead (TCP round-trip + message framing). The actual git work is the same; the difference is entirely in how files are accessed.

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

## Related projects

- **[wsl-git-shim](https://github.com/aleixrodriala/wsl-git-shim)** — A simpler, zero-fork approach: replaces Desktop's bundled `git.exe` with a shim that routes WSL paths to `wsl.exe -e git`. Works with official Desktop but slower (~40ms overhead per command from spawning `wsl.exe`) and doesn't support file operations.

## License

[MIT](LICENSE)

## Credits

Based on [GitHub Desktop](https://github.com/desktop/desktop) by GitHub, Inc.

WSL support by [@aleixrodriala](https://github.com/aleixrodriala).
