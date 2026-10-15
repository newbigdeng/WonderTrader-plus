#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
sdk_lib='../runtime/wtpy/wrapper/linux/traders'
test -f sdk/include/ThostFtdcTraderApi.h
test -f "$sdk_lib/thosttraderapi_se.so"
g++ -std=c++11 -O2 -Wall -Wextra -pthread -isystem sdk/include AccountQuery.cpp \
    -L "$sdk_lib" '-Wl,-rpath,$ORIGIN/../runtime/wtpy/wrapper/linux/traders' \
    -l:thosttraderapi_se.so -o AccountQuery
printf '%s\n' 'Built AccountQuery with the existing SimNow CTP SDK.'
