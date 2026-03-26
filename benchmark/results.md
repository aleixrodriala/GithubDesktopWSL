# Benchmark Results

Comparing three approaches to running git on WSL repos:

1. **git.exe through 9P** — what official GitHub Desktop does (Windows git.exe accessing `\\wsl.localhost\...`)
2. **wsl.exe -e git (shim)** — what [wsl-git-bridge](https://github.com/MiloudiMohamed/wsl-git-bridge) and similar projects do
3. **Native git (daemon)** — what this fork achieves (persistent daemon, no spawn or 9P overhead)

## Environment

- **Date**: 2026-03-26
- **Iterations**: 11 (median reported)
- **OS**: Windows 11 + WSL2
- **Kernel**: 6.6.87.2-microsoft-standard-WSL2
- **git.exe**: git version 2.53.0.windows.2
- **Native git**: git version 2.43.0

## Spawn overhead

Before any git work happens, each approach has a fixed cost:

| Approach | Overhead |
|----------|----------|
| Native git (daemon) | 1 ms |
| git.exe | 46 ms |
| wsl.exe -e (shim) | 94 ms |

## Results by repo size

### Small repo (21 files)

| Operation | Native (daemon) | git.exe (9P) | wsl.exe (shim) |
|-----------|----------------:|-------------:|----------------:|
| git status | 2 ms | 50 ms (25x) | 93 ms (46x) |
| git log | 2 ms | 50 ms (25x) | 92 ms (46x) |
| git diff | 2 ms | 49 ms (24x) | 90 ms (45x) |
| git branch | 1 ms | 50 ms (50x) | 93 ms (93x) |
| git rev-parse | 1 ms | 51 ms (51x) | 92 ms (92x) |
| git for-each-ref | 1 ms | 51 ms (51x) | 96 ms (96x) |

### Medium repo (334 files)

| Operation | Native (daemon) | git.exe (9P) | wsl.exe (shim) |
|-----------|----------------:|-------------:|----------------:|
| git status | 2 ms | 51 ms (25x) | 93 ms (46x) |
| git log | 2 ms | 51 ms (25x) | 91 ms (45x) |
| git diff | 2 ms | 48 ms (24x) | 91 ms (45x) |
| git branch | 1 ms | 49 ms (49x) | 92 ms (92x) |
| git rev-parse | 1 ms | 49 ms (49x) | 92 ms (92x) |
| git for-each-ref | 1 ms | 51 ms (51x) | 92 ms (92x) |

### Large repo (2373 files)

| Operation | Native (daemon) | git.exe (9P) | wsl.exe (shim) |
|-----------|----------------:|-------------:|----------------:|
| git status | 3 ms | 50 ms (16x) | 92 ms (30x) |
| git log | 2 ms | 49 ms (24x) | 93 ms (46x) |
| git diff | 2 ms | 50 ms (25x) | 92 ms (46x) |
| git branch | 2 ms | 49 ms (24x) | 90 ms (45x) |
| git rev-parse | 1 ms | 49 ms (49x) | 92 ms (92x) |
| git for-each-ref | 2 ms | 50 ms (25x) | 91 ms (45x) |

## What these numbers mean

- **git.exe (9P)**: Every command pays ~40ms+ just to cross the VM boundary via the 9P filesystem protocol. This is the floor — it doesn't get faster regardless of how trivial the git operation is.
- **wsl.exe (shim)**: Avoids 9P entirely (git runs natively on ext4), but spawning `wsl.exe` per command costs ~90-100ms. Better than 9P for large repos where 9P overhead compounds, but worse for small/trivial operations.
- **Native git (daemon)**: Same as running git directly in WSL. The daemon adds ~2ms of TCP overhead. This is what the fork achieves.

A typical GitHub Desktop refresh runs 10-20 commands. With git.exe that's 400-800ms of pure overhead. With a shim it's 900-2000ms of spawn overhead. With the daemon it's 20-40ms.
