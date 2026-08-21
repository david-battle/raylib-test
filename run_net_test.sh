#!/usr/bin/env bash
# Build (if needed) and run net_test with the VM's current external IP,
# which changes on every start since it's an ephemeral address.
set -euo pipefail

PROJECT=plasma-sol-276402
ZONE=us-west1-a
INSTANCE=udp-test
PORT=7777

cd "$(dirname "$0")"

if [[ ! -x net_test ]]; then
    gcc -I ~/raylib/src main.c hide_cursor_x11.c -o net_test \
        ~/raylib/src/libraylib.a -lm -lpthread -ldl -lX11
fi

IP=$(gcloud compute instances list \
    --project="$PROJECT" --filter="name=$INSTANCE" \
    --format="get(networkInterfaces[0].accessConfigs[0].natIP)")

if [[ -z "$IP" ]]; then
    echo "Instance $INSTANCE has no external IP (is it running?)" >&2
    exit 1
fi

echo "Echo server: $IP:$PORT"
exec ./net_test "$IP"
