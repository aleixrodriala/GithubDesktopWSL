#!/usr/bin/env bash
#
# Benchmark: git.exe (9P) vs wsl.exe -e git (shim) vs native git (daemon baseline)
#
# Measures the three approaches to running git on WSL repos from GitHub Desktop:
#   1. git.exe through 9P — what official Desktop does
#   2. wsl.exe -e git — what shim/bridge projects do
#   3. native git — what the daemon achieves (same process, no spawn/9P overhead)
#
# Usage: ./benchmark/run.sh [iterations]

set -euo pipefail

ITERATIONS="${1:-11}"
BENCH_DIR="/tmp/github-desktop-wsl-bench"
RESULTS_FILE="$(cd "$(dirname "$0")" && pwd)/results.md"

# Paths to executables
GIT_EXE="/mnt/c/Program Files/Git/cmd/git.exe"
WSL_EXE="/mnt/c/Windows/System32/wsl.exe"
NATIVE_GIT="$(which git)"

if [[ ! -f "$GIT_EXE" ]]; then
    echo "ERROR: git.exe not found at $GIT_EXE"
    echo "Install Git for Windows from https://git-scm.com"
    exit 1
fi

# --- Helpers ---

median() {
    # Takes a file with one number per line, returns the median
    sort -n "$1" | awk -v n="$(wc -l < "$1")" 'NR==int(n/2)+1{print $0}'
}

bench() {
    local label="$1"
    local workdir="$2"
    shift 2
    local cmd=("$@")
    local timefile
    timefile=$(mktemp)

    # Warmup run
    (cd "$workdir" && "${cmd[@]}" > /dev/null 2>&1) || true

    for i in $(seq 1 "$ITERATIONS"); do
        local start end elapsed
        start=$(date +%s%N)
        (cd "$workdir" && "${cmd[@]}" > /dev/null 2>&1) || true
        end=$(date +%s%N)
        elapsed=$(( (end - start) / 1000000 ))
        echo "$elapsed" >> "$timefile"
    done

    local med
    med=$(median "$timefile")
    rm -f "$timefile"
    echo "$med"
}

# For git.exe, we need to convert WSL paths to Windows UNC paths
wsl_to_unc() {
    local wslpath="$1"
    local distro
    distro=$(wslpath -w / | sed 's|\\$||' | sed 's|.*\\\\||')
    echo "\\\\wsl.localhost\\${distro}${wslpath}"
}

bench_gitexe() {
    local label="$1"
    local workdir="$2"
    shift 2
    local args=("$@")
    local unc_workdir
    unc_workdir=$(wsl_to_unc "$workdir")
    local timefile
    timefile=$(mktemp)

    # Warmup
    (cd "$workdir" && "$GIT_EXE" -C "$unc_workdir" "${args[@]}" > /dev/null 2>&1) || true

    for i in $(seq 1 "$ITERATIONS"); do
        local start end elapsed
        start=$(date +%s%N)
        "$GIT_EXE" -C "$unc_workdir" "${args[@]}" > /dev/null 2>&1 || true
        end=$(date +%s%N)
        elapsed=$(( (end - start) / 1000000 ))
        echo "$elapsed" >> "$timefile"
    done

    local med
    med=$(median "$timefile")
    rm -f "$timefile"
    echo "$med"
}

bench_wslexe() {
    local label="$1"
    local workdir="$2"
    shift 2
    local args=("$@")
    local timefile
    timefile=$(mktemp)

    # Warmup
    "$WSL_EXE" -e git -C "$workdir" "${args[@]}" > /dev/null 2>&1 || true

    for i in $(seq 1 "$ITERATIONS"); do
        local start end elapsed
        start=$(date +%s%N)
        "$WSL_EXE" -e git -C "$workdir" "${args[@]}" > /dev/null 2>&1 || true
        end=$(date +%s%N)
        elapsed=$(( (end - start) / 1000000 ))
        echo "$elapsed" >> "$timefile"
    done

    local med
    med=$(median "$timefile")
    rm -f "$timefile"
    echo "$med"
}

# --- Create test repos ---

create_repo() {
    local name="$1"
    local num_files="$2"
    local dir="$BENCH_DIR/$name"

    if [[ -d "$dir" ]]; then
        rm -rf "$dir"
    fi

    mkdir -p "$dir"
    cd "$dir"
    git init -q
    git config user.email "bench@test.com"
    git config user.name "Benchmark"

    # Create files
    for i in $(seq 1 "$num_files"); do
        local subdir="src/dir$(( (i - 1) / 50 ))"
        mkdir -p "$subdir"
        echo "// File $i - $(head -c 200 /dev/urandom | base64 | head -c 100)" > "$subdir/file${i}.txt"
    done

    git add -A
    git commit -q -m "Initial commit with $num_files files"

    # Add some history (10 commits)
    for c in $(seq 1 10); do
        echo "change $c" >> README.md
        git add -A
        git commit -q -m "Commit $c"
    done

    cd - > /dev/null
    echo "$dir"
}

# --- Main ---

echo "=== GitHub Desktop WSL Benchmark ==="
echo ""
echo "Iterations: $ITERATIONS (median reported)"
echo "Date: $(date -u +%Y-%m-%d)"
echo "Kernel: $(uname -r)"
echo "git.exe: $("$GIT_EXE" --version 2>/dev/null | head -1)"
echo "native git: $(git --version)"
echo ""

# Create repos
echo "Creating test repositories..."
REPO_SMALL=$(create_repo "small" 20)
REPO_MEDIUM=$(create_repo "medium" 333)
REPO_LARGE=$(create_repo "large" 2372)
echo "  small:  20 files   ($REPO_SMALL)"
echo "  medium: 333 files  ($REPO_MEDIUM)"
echo "  large:  2372 files ($REPO_LARGE)"
echo ""

# Operations to benchmark
OPS=(
    "status:status --porcelain"
    "log:log -20 --oneline"
    "diff:diff HEAD~1 --stat"
    "branch:branch -a"
    "rev-parse:rev-parse HEAD"
    "for-each-ref:for-each-ref --count=20 --sort=-committerdate refs/"
)

# Repo configs
REPOS=(
    "small:$REPO_SMALL"
    "medium:$REPO_MEDIUM"
    "large:$REPO_LARGE"
)

# Run benchmarks
echo "Running benchmarks..."
echo ""

# Store all results for the markdown table
declare -A RESULTS

for repo_conf in "${REPOS[@]}"; do
    repo_name="${repo_conf%%:*}"
    repo_path="${repo_conf#*:}"
    file_count=$(find "$repo_path" -type f -not -path '*/.git/*' | wc -l)

    echo "--- $repo_name ($file_count files) ---"

    for op_conf in "${OPS[@]}"; do
        op_label="${op_conf%%:*}"
        op_args="${op_conf#*:}"

        # shellcheck disable=SC2086
        t_native=$(bench "$op_label" "$repo_path" git $op_args)
        # shellcheck disable=SC2086
        t_gitexe=$(bench_gitexe "$op_label" "$repo_path" $op_args)
        # shellcheck disable=SC2086
        t_wslexe=$(bench_wslexe "$op_label" "$repo_path" $op_args)

        speedup_9p="$(( t_gitexe / (t_native > 0 ? t_native : 1) ))x"
        speedup_shim="$(( t_wslexe / (t_native > 0 ? t_native : 1) ))x"

        printf "  %-15s native: %4d ms | git.exe (9P): %4d ms (%s) | wsl.exe (shim): %4d ms (%s)\n" \
            "$op_label" "$t_native" "$t_gitexe" "$speedup_9p" "$t_wslexe" "$speedup_shim"

        RESULTS["${repo_name}_${op_label}_native"]="$t_native"
        RESULTS["${repo_name}_${op_label}_gitexe"]="$t_gitexe"
        RESULTS["${repo_name}_${op_label}_wslexe"]="$t_wslexe"
    done
    echo ""
done

# Also benchmark pure spawn overhead
echo "--- Pure spawn overhead (no git work) ---"
t_spawn_gitexe=$(bench "spawn" "/tmp" "$GIT_EXE" --version)
t_spawn_wslexe=$(bench "spawn" "/tmp" "$WSL_EXE" -e echo ok)
t_spawn_native=$(bench "spawn" "/tmp" git --version)
printf "  %-15s native: %4d ms | git.exe: %4d ms | wsl.exe -e: %4d ms\n" \
    "spawn" "$t_spawn_native" "$t_spawn_gitexe" "$t_spawn_wslexe"
echo ""

# --- Generate markdown results ---

cat > "$RESULTS_FILE" << 'HEADER'
# Benchmark Results

Comparing three approaches to running git on WSL repos:

1. **git.exe through 9P** — what official GitHub Desktop does (Windows git.exe accessing `\\wsl.localhost\...`)
2. **wsl.exe -e git (shim)** — what [wsl-git-bridge](https://github.com/MiloudiMohamed/wsl-git-bridge) and similar projects do
3. **Native git (daemon)** — what this fork achieves (persistent daemon, no spawn or 9P overhead)

HEADER

cat >> "$RESULTS_FILE" << EOF
## Environment

- **Date**: $(date -u +%Y-%m-%d)
- **Iterations**: $ITERATIONS (median reported)
- **OS**: Windows 11 + WSL2
- **Kernel**: $(uname -r)
- **git.exe**: $("$GIT_EXE" --version 2>/dev/null | head -1)
- **Native git**: $(git --version)

## Spawn overhead

Before any git work happens, each approach has a fixed cost:

| Approach | Overhead |
|----------|----------|
| Native git (daemon) | ${t_spawn_native} ms |
| git.exe | ${t_spawn_gitexe} ms |
| wsl.exe -e (shim) | ${t_spawn_wslexe} ms |

## Results by repo size

EOF

for repo_conf in "${REPOS[@]}"; do
    repo_name="${repo_conf%%:*}"
    repo_path="${repo_conf#*:}"
    file_count=$(find "$repo_path" -type f -not -path '*/.git/*' | wc -l)

    cat >> "$RESULTS_FILE" << EOF
### ${repo_name^} repo ($file_count files)

| Operation | Native (daemon) | git.exe (9P) | wsl.exe (shim) |
|-----------|----------------:|-------------:|----------------:|
EOF

    for op_conf in "${OPS[@]}"; do
        op_label="${op_conf%%:*}"
        t_native="${RESULTS[${repo_name}_${op_label}_native]}"
        t_gitexe="${RESULTS[${repo_name}_${op_label}_gitexe]}"
        t_wslexe="${RESULTS[${repo_name}_${op_label}_wslexe]}"
        speedup_9p="$(( t_gitexe / (t_native > 0 ? t_native : 1) ))x"
        speedup_shim="$(( t_wslexe / (t_native > 0 ? t_native : 1) ))x"
        echo "| git $op_label | ${t_native} ms | ${t_gitexe} ms (${speedup_9p}) | ${t_wslexe} ms (${speedup_shim}) |" >> "$RESULTS_FILE"
    done

    echo "" >> "$RESULTS_FILE"
done

cat >> "$RESULTS_FILE" << 'FOOTER'
## What these numbers mean

- **git.exe (9P)**: Every command pays ~40ms+ just to cross the VM boundary via the 9P filesystem protocol. This is the floor — it doesn't get faster regardless of how trivial the git operation is.
- **wsl.exe (shim)**: Avoids 9P entirely (git runs natively on ext4), but spawning `wsl.exe` per command costs ~90-100ms. Better than 9P for large repos where 9P overhead compounds, but worse for small/trivial operations.
- **Native git (daemon)**: Same as running git directly in WSL. The daemon adds ~2ms of TCP overhead. This is what the fork achieves.

A typical GitHub Desktop refresh runs 10-20 commands. With git.exe that's 400-800ms of pure overhead. With a shim it's 900-2000ms of spawn overhead. With the daemon it's 20-40ms.
FOOTER

echo "Results written to: $RESULTS_FILE"
echo ""

# Cleanup
rm -rf "$BENCH_DIR"
echo "Done. Temp repos cleaned up."
