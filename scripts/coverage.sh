#!/bin/sh
# Line coverage of src/ from every test suite together (smoke, unit_widgets, unit_fileops,
# unit_core with the CLI runs of the app itself, unit_update), all headless. Builds an instrumented
# Debug build in build-cov (clang), runs the suites, merges the profiles and prints the report;
# `scripts/coverage.sh show src/Sidebar.cpp` prints that file's lines with their counts instead.
# macOS (xcrun llvm-*) and Linux (llvm-profdata / llvm-cov on the PATH).
set -e
cd "$(dirname "$0")/.."
if command -v xcrun >/dev/null 2>&1; then
    PROFDATA="xcrun llvm-profdata"; COV="xcrun llvm-cov"
else
    PROFDATA=llvm-profdata; COV=llvm-cov
fi
B=build-cov
if [ ! -f $B/CMakeCache.txt ]; then
    PREFIX=""
    command -v brew >/dev/null 2>&1 && PREFIX="-DCMAKE_PREFIX_PATH=$(brew --prefix qt)"
    cmake -S . -B $B -G Ninja -DCMAKE_BUILD_TYPE=Debug $PREFIX \
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_C_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
        -DCMAKE_CXX_FLAGS="-fprofile-instr-generate -fcoverage-mapping" \
        -DCMAKE_EXE_LINKER_FLAGS="-fprofile-instr-generate"
fi
APP=$B/Gifiles.app/Contents/MacOS/Gifiles
[ -f "$APP" ] || APP=$B/Gifiles
SUITES="smoke unit_widgets unit_fileops unit_core unit_update"
OBJECTS="$APP"
for t in $SUITES; do OBJECTS="$OBJECTS -object $B/gifiles_$t"; done
IGNORE='(third_party|tests|_autogen|/opt/|/usr/)'

if [ "$1" = show ]; then
    shift
    $COV show $OBJECTS -instr-profile=$B/all.profdata "$@"
    exit 0
fi

cmake --build $B
rm -rf $B/prof && mkdir -p $B/prof
status=0
for t in $SUITES; do
    LLVM_PROFILE_FILE="$PWD/$B/prof/$t-%p.profraw" ./$B/gifiles_$t >$B/prof/$t.log 2>&1 || {
        status=1
        echo "$t failed:"; grep -E '^(FAIL|   Loc)' $B/prof/$t.log || tail -20 $B/prof/$t.log
    }
    grep -E '^Totals' $B/prof/$t.log | sed "s/^/$t: /"
done
$PROFDATA merge -sparse $B/prof/*.profraw -o $B/all.profdata
$COV report $OBJECTS -instr-profile=$B/all.profdata -ignore-filename-regex="$IGNORE"
exit $status
