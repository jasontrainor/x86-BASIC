#!/bin/bash
# Fuzz the BASICs with libFuzzer (clang), AddressSanitizer and UBSan:
# random lines typed in, and random files for them to LOAD, MERGE, RUN
# and EXEC, looking for crashes, memory errors and undefined behaviour.
#
#   tests/fuzz.sh [MACHINE ...]     spectrum a2fp a2int a1basic c64 (all by default)
#   FUZZ_TIME=60                    seconds for each machine
#   FUZZ_JOBS=1                     fuzzers run at once for each
#
# Each machine's fuzzer, corpus and findings are in tests/build/fuzz/NAME/;
# a finding (crash-..., leak-..., timeout-...) is reproduced by running
#   FUZZ_DISK=tests/build/fuzz/disk.img tests/build/fuzz/NAME/fuzz FILE
# An input is a file, a 0 byte, then the keys typed (see tests/host_stub.c);
# tests/fuzz/show.py prints one readably. Seeds are made from the
# listings in tests/listings and docs/screenshots.
set -u
cd "$(dirname "$0")/.."
command -v clang > /dev/null || { echo "fuzz: needs clang (with libFuzzer)" >&2; exit 1; }
machines=("$@")
[ ${#machines[@]} -gt 0 ] || machines=(spectrum a2fp a2int a1basic c64)
time=${FUZZ_TIME:-60}
jobs=${FUZZ_JOBS:-1}
out=tests/build/fuzz
mkdir -p "$out"
python3 tests/make_disk.py "$out/disk.img" || exit 1
export FUZZ_DISK=$PWD/$out/disk.img   # (each fuzzer runs in its own folder)
# (so that libFuzzer sees undefined behaviour as a crash, and keeps the input)
export UBSAN_OPTIONS=${UBSAN_OPTIONS:-abort_on_error=1:print_stacktrace=1}
status=0

for m in "${machines[@]}"; do
    dir=$out/$m
    mkdir -p "$dir/corpus"
    inc="-Icore -Imachines/$m" src="machines/$m/*.c"
    for u in $(cat "machines/$m/uses" 2>/dev/null); do inc+=" -Imachines/$u"; src+=" machines/$u/*.c"; done
    echo "== $m: building"
    # shellcheck disable=SC2086
    clang -g -O1 -fno-builtin -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
        -Wall -Wno-unused-function -Wno-unknown-attributes $inc -DHOST_TEST -DFUZZ -o "$dir/fuzz" tests/host_stub.c $src \
        core/storage.c core/fat.c core/string.c core/num.c core/font.c core/keymap.c || exit 1
    python3 tests/fuzz/seeds.py "$m" "$dir/corpus" "$dir/dict" || exit 1
    folder=$(echo "$m" | tr a-z A-Z)
    echo "== $m: fuzzing for ${time}s"
    par=()
    [ "$jobs" -gt 1 ] && par=(-jobs="$jobs" -workers="$jobs")   # (each then logs to fuzz-N.log)
    # (Its output goes to log.txt; a few lines of it are shown.)
    (cd "$dir" && FUZZ_FOLDER=$folder ./fuzz -max_total_time="$time" "${par[@]}" \
        -dict=dict -max_len=1024 -timeout=10 -rss_limit_mb=4096 -print_final_stats=1 corpus \
        > log.txt 2>&1)
    ran=$?
    grep -E "^#[0-9]+.*DONE|ERROR|SUMMARY|runtime error|stat::(number_of_executed_units|average_exec_per_sec)" \
        "$dir/log.txt" | tail -6
    found=$(ls "$dir"/crash-* "$dir"/leak-* "$dir"/timeout-* 2> /dev/null)
    if [ -n "$found" ]; then
        echo "== $m: FOUND PROBLEMS:"; echo "$found"
        status=1
    elif [ $ran -ne 0 ]; then
        echo "== $m: the fuzzer failed (exit $ran): see $dir/log.txt"; tail -3 "$dir/log.txt"
        status=1
    else
        echo "== $m: nothing found"
    fi
done
exit $status
