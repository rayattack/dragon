#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

# Python is opt-in: it runs ~100-1000× slower than the compiled languages and
# would dominate suite runtime (e.g. mandelbrot ~0.5s compiled vs ~44s Python).
# Pass --python (or -p) to include it; by default the suite runs only the
# compiled "big boys" (C, C++, Rust, Go, Java, Dragon).
RUN_PYTHON=0
for arg in "$@"; do
    case "$arg" in
        --python|-p) RUN_PYTHON=1 ;;
    esac
done

DRAGON=../build/dragon
RESULTS=""
DIVIDER="────────────────────────────────────────────────────────────"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
BOLD='\033[1m'
RESET='\033[0m'

time_cmd() {
    local label="$1"
    shift
    # Run 3 times, take the best (lowest) real time
    local best=""
    for run in 1 2 3; do
        local start end elapsed
        start=$(date +%s%N)
        output=$("$@" 2>&1)
        end=$(date +%s%N)
        elapsed=$(( (end - start) ))
        if [ -z "$best" ] || [ "$elapsed" -lt "$best" ]; then
            best=$elapsed
        fi
    done
    local ms=$((best / 1000000))
    local sec=$((ms / 1000))
    local frac=$((ms % 1000))
    printf "  ${CYAN}%-12s${RESET} %3d.%03ds  │ %s\n" "$label" "$sec" "$frac" "$output"
    RESULTS="${RESULTS}${label},${ms}\n"
}

compile_and_bench() {
    local name="$1"
    local dir="$2"

    echo ""
    echo -e "${BOLD}${YELLOW}[$name]${RESET}"
    echo "$DIVIDER"
    RESULTS=""

    # C
    gcc -O2 -o "$dir/bench_c" "$dir/"*.c 2>/dev/null && \
        time_cmd "C" "$dir/bench_c" || echo "  C: compile failed"

    # C++
    g++ -O2 -o "$dir/bench_cpp" "$dir/"*.cpp 2>/dev/null && \
        time_cmd "C++" "$dir/bench_cpp" || echo "  C++: compile failed"

    # Rust
    rustc -O -o "$dir/bench_rs" "$dir/"*.rs 2>/dev/null && \
        time_cmd "Rust" "$dir/bench_rs" || echo "  Rust: compile failed"

    # Java
    javac "$dir/"*.java -d "$dir" 2>/dev/null && {
        local classname
        classname=$(basename "$dir/"*.java .java)
        time_cmd "Java" java -cp "$dir" "$classname"
    } || echo "  Java: compile failed"

    # Go
    if ls "$dir/"*.go 1>/dev/null 2>&1; then
        go build -o "$dir/bench_go" "$dir/"*.go 2>/dev/null && \
            time_cmd "Go" "$dir/bench_go" || echo "  Go: compile failed"
    fi

    # Python (opt-in via --python / -p)
    if [ "$RUN_PYTHON" = "1" ] && ls "$dir/"*.py 1>/dev/null 2>&1; then
        time_cmd "Python" python3 "$dir/"*.py || echo "  Python: run failed"
    fi

    # Dragon (with -O2 to match other compilers)
    $DRAGON build -O2 "$dir/"*.dr -o "$dir/bench_dr" 2>/dev/null && \
        time_cmd "Dragon" "$dir/bench_dr" || echo "  Dragon: compile failed"

    echo "$DIVIDER"
}

echo ""
echo -e "${BOLD}${GREEN}══════════════════════════════════════════════════════════════${RESET}"
if [ "$RUN_PYTHON" = "1" ]; then
    echo -e "${BOLD}${GREEN}  DRAGON vs C vs C++ vs Rust vs Go vs Java vs Python${RESET}"
else
    echo -e "${BOLD}${GREEN}  DRAGON vs C vs C++ vs Rust vs Go vs Java   (pass --python to add Python)${RESET}"
fi
echo -e "${BOLD}${GREEN}  Best of 3 runs │ All compiled with optimizations${RESET}"
echo -e "${BOLD}${GREEN}══════════════════════════════════════════════════════════════${RESET}"

compile_and_bench "Fibonacci (recursive, n=42)" "fib"
compile_and_bench "Sieve of Eratosthenes (n=1,000,000)" "sieve"
compile_and_bench "String Concatenation (10,000 iterations)" "strings"
compile_and_bench "Object Creation (1,000,000 instances)" "objects"
compile_and_bench "Binary Trees (alloc/GC churn, depth 14)" "binary-trees"
compile_and_bench "Mandelbrot (float, 1600x1600, 100 iter)" "mandelbrot"
compile_and_bench "Parallel Sum (fork-join, 8 workers x 30M)" "parallel"
compile_and_bench "Dictionary / Hashmap (string keys, 3M ops)" "dicts"

echo ""
echo -e "${BOLD}${GREEN}Done.${RESET}"

# Cleanup compiled artifacts across all benchmark dirs.
rm -f ./*/bench_*
rm -f ./*/*.class
