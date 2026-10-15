#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "${BASH_SOURCE[0]}")"
umask 077
export TZ=Asia/Shanghai
if [[ ! -x ./AccountQuery ]]; then
    printf '%s\n' 'Run bash build.sh first.' >&2
    exit 2
fi
exec flock -n -E 73 .query.lock ./AccountQuery "${1:-config.ini}"
