#!/bin/sh
# Starts/reuses the default broker, then opens the first interactive client.
set -eu
cd -- "$(dirname -- "$0")"
if [ ! -x build/relay-chat ]; then ./build.sh; fi
server_pid=''
cleanup() {
    if [ -n "$server_pid" ]; then
        kill "$server_pid" 2>/dev/null || true
        wait "$server_pid" 2>/dev/null || true
    fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM
if ! ./chat check >/dev/null 2>&1; then
    umask 077
    mkdir -p var
    ./chat server >var/demo-server.log 2>&1 &
    server_pid=$!
    attempts=0
    until ./chat check >/dev/null 2>&1; do
        attempts=$((attempts + 1))
        if [ "$attempts" -ge 30 ] || ! kill -0 "$server_pid" 2>/dev/null; then
            cat var/demo-server.log >&2
            exit 1
        fi
        sleep 0.1
    done
fi
printf '\nOpen another Ubuntu terminal and run:\n  cd %s\n  ./chat --name Bob\n\n' "$PWD"
printf '%s\n' 'Starting Alice. /help lists commands; /quit ends this demo client.'
./chat --name "${1:-Alice}"
