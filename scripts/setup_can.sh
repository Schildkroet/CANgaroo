#!/bin/bash
# Setup every physical CAN interface for CAN FD (Classical CAN where an
# adapter has no FD). Virtual vcan interfaces are left alone, see setup_vcan.sh.
# Nominal bitrate 500 kbit/s, data bitrate 2 Mbit/s by default
# Requires: iproute2, a SocketCAN-capable adapter (e.g. gs_usb, peak_usb)
# Usage: ./setup_can.sh [bitrate] [dbitrate]   (default 500000 2000000)
#        Re-runs itself under sudo when not started as root.

set -e

# Configuring an interface with "ip link" needs CAP_NET_ADMIN, so rerun the
# whole script under sudo rather than failing halfway through the loop.
if [[ $EUID -ne 0 ]]; then
    if ! command -v sudo > /dev/null 2>&1; then
        echo "Error: root privileges are required and sudo was not found." >&2
        echo "       Run this script as root instead." >&2
        exit 1
    fi
    echo "Root privileges required, re-running under sudo..." >&2
    exec sudo -- "$(readlink -f "$0")" "$@"
fi

BITRATE="${1:-500000}"
DBITRATE="${2:-2000000}"

# All interfaces of link type "can" (gs_usb, peak_usb, slcan, ...); vcan has
# its own link type and is not listed.
mapfile -t DEVICES < <(ip -o link show type can | awk -F': ' '{ print $2 }')
if [[ ${#DEVICES[@]} -eq 0 ]]; then
    echo "Error: no CAN interface found (is the adapter plugged in?)" >&2
    exit 1
fi

# One adapter that cannot be configured must not stop the others.
FAILED=()
setup_device() {
    local dev="$1"

    # Bitrates can only be changed while the interface is down
    ip link set down "$dev" || return 1

    if ip link set "$dev" type can bitrate "$BITRATE" dbitrate "$DBITRATE" fd on 2> /dev/null; then
        echo "$dev: CAN FD, ${BITRATE} bit/s nominal, ${DBITRATE} bit/s data"
    else
        echo "Note: $dev does not support CAN FD, falling back to Classical CAN"
        ip link set "$dev" type can bitrate "$BITRATE" fd off 2> /dev/null \
            || ip link set "$dev" type can bitrate "$BITRATE" \
            || return 1
        echo "$dev: Classical CAN, ${BITRATE} bit/s"
    fi

    # Auto-recovery from bus-off; not every driver supports it (e.g. some USB adapters)
    ip link set "$dev" type can restart-ms 100 2> /dev/null \
        || echo "Note: $dev does not support restart from bus-off, skipping restart-ms"

    ip link set up "$dev"
}

for dev in "${DEVICES[@]}"; do
    if ! setup_device "$dev"; then
        echo "Error: could not configure $dev" >&2
        FAILED+=("$dev")
    fi
done

echo
if [[ ${#DEVICES[@]} -ge 2 ]]; then
    echo "Test: cansend ${DEVICES[0]} 123#DEADBEEF     ->  candump ${DEVICES[1]}"
    echo "      cansend ${DEVICES[0]} 123##1DEADBEEF   ->  candump ${DEVICES[1]}   (CAN FD, with BRS)"
else
    echo "Test: candump ${DEVICES[0]}   (needs a second node on the bus to see traffic)"
fi

if [[ ${#FAILED[@]} -gt 0 ]]; then
    echo "Failed: ${FAILED[*]}" >&2
    exit 1
fi
