#!/bin/bash

# Simple script to run CPU2006 with AddressSanitizer.
# Make sure to use spec version 1.2 (SPEC_CPU2006v1.2).
# Run this script like this:
# $./run_spec_clang_asan.sh TAG size benchmarks...
# TAG is any word. If you use different TAGS you can runs several builds in
# parallel.
# size can be test, train or ref. test is a small data set, train is medium,
# ref is large.
# To run all C tests use all_c, for C++ use all_cpp. To run integer tests
# use int, for floating point use fp.

name=$1
shift
size=$1
shift

me=$(basename $0)

usage() {
  echo >&2 "Usage: $me TAG size bmarks"
  exit 1
}

if test -z "$name"; then
  usage
fi

case "$size" in
  test|train|ref)
    ;;
  *)
    echo >&2 "$me: unexpected size: $size"
    usage
    ;;
esac

if [ ! -f ./shrc ]; then
  echo >&2 "$me: script must be run from SPEC2006 folder"
  exit 1
fi

ulimit -s 8092  # stack

SPEC_J=${SPEC_J:-20}
NUM_RUNS=${NUM_RUNS:-1}
CC=${CC}
BIT=${BIT:-64}
OPT_LEVEL=${OPT_LEVEL:-"-O2"}
F_ASAN=-fsanitize=address
CXX=${CXX}
rm -rf config/$name.*

if test -z "$FC"; then
  FC=echo
fi

COMMON_FLAGS="$F_ASAN -fsanitize-recover=address -m$BIT -g"
CC="$CC    -std=gnu89 $COMMON_FLAGS"
CXX="$CXX             $COMMON_FLAGS"
FC="$FC     $COMMON_FLAGS"

cat << EOF > config/$name.cfg
monitor_wrapper = $SPEC_WRAPPER  \$command
monitor_specrun_wrapper = $SPECRUN_WRAPPER  \$command
ignore_errors = yes
tune          = base
ext           = $name
output_format = asc, Screen
reportable    = 1
teeout        = yes
teerunout     = yes
strict_rundir_verify = 0
makeflags = -j$SPEC_J
default=default=default=default:
CC  = $CC
CXX = $CXX
EXTRA_LIBS = $EXTRA_LIBS
FC         = $FC
default=base=default=default:
COPTIMIZE   = $OPT_LEVEL
CXXOPTIMIZE = $OPT_LEVEL
default=base=default=default:
PORTABILITY = -DSPEC_CPU_LP64
400.perlbench=default=default=default:
CPORTABILITY= -DSPEC_CPU_LINUX_X64
462.libquantum=default=default=default:
CPORTABILITY= -DSPEC_CPU_LINUX
483.xalancbmk=default=default=default:
CXXPORTABILITY= -DSPEC_CPU_LINUX -include string.h
447.dealII=default=default=default:
CXXPORTABILITY= -include string.h -include stdlib.h -include cstddef
EOF

export ASAN_OPTIONS=halt_on_error=0:detect_leaks=0${ASAN_OPTIONS:+:$ASAN_OPTIONS}
. shrc
runspec -c $name -a run -I -l --size $size -n $NUM_RUNS "$@"