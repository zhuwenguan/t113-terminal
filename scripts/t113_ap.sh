#!/bin/sh
set -eu

# The base T113 image starts wlan0 as a station from S39wifi/S40network.
# Reconfigure it here so every application start (including the SysV boot
# entry) ends in the same local telemetry AP mode.
T113_WIFI_IFACE=${T113_WIFI_IFACE:-wlan0}
T113_AP_ADDRESS=${T113_AP_ADDRESS:-192.168.5.1/24}
T113_HOSTAPD_CONF=${T113_HOSTAPD_CONF:-/etc/hostapd.conf}
T113_DNSMASQ_CONF=${T113_DNSMASQ_CONF:-/etc/dnsmasq.conf}
T113_UDHCPC_PIDFILE=${T113_UDHCPC_PIDFILE:-/var/run/udhcpc.wlan0.pid}
T113_AP_IP=${T113_AP_ADDRESS%/*}

log() {
    printf '%s\n' "t113_ap: $*"
}

pid_is_number() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

stop_station_dhcp() {
    if [ -r "$T113_UDHCPC_PIDFILE" ]; then
        pid=$(sed -n '1p' "$T113_UDHCPC_PIDFILE")
        if pid_is_number "$pid"; then
            kill "$pid" 2>/dev/null || true
        fi
    fi
    rm -f "$T113_UDHCPC_PIDFILE"
}

stop_station_wpa() {
    # The stock image does not create a wpa_supplicant pid file.  Match only
    # the wlan0 command line so a second radio is not disturbed.
    for pid in $(ps 2>/dev/null | awk '$0 ~ /wpa_supplicant/ && $0 ~ /-i[ =]wlan0([ ]|$)/ {print $1}'); do
        if pid_is_number "$pid"; then
            kill "$pid" 2>/dev/null || true
        fi
    done
}

ap_is_ready() {
    iw dev "$T113_WIFI_IFACE" info 2>/dev/null |
        grep -q 'type AP' || return 1
    ip -4 addr show dev "$T113_WIFI_IFACE" 2>/dev/null |
        grep -q "inet ${T113_AP_IP}/" || return 1
    ps 2>/dev/null | grep -q '[h]ostapd' || return 1
    ps 2>/dev/null | grep -q '[d]nsmasq' || return 1
    return 0
}

start_ap() {
    [ -d "/sys/class/net/$T113_WIFI_IFACE" ] || {
        log "interface $T113_WIFI_IFACE is missing"
        return 1
    }
    [ -r "$T113_HOSTAPD_CONF" ] || {
        log "hostapd config is missing: $T113_HOSTAPD_CONF"
        return 1
    }
    [ -r "$T113_DNSMASQ_CONF" ] || {
        log "dnsmasq config is missing: $T113_DNSMASQ_CONF"
        return 1
    }

    if ap_is_ready; then
        log "AP already ready: iface=$T113_WIFI_IFACE address=$T113_AP_ADDRESS"
        return 0
    fi

    stop_station_dhcp
    stop_station_wpa
    killall hostapd 2>/dev/null || true
    killall dnsmasq 2>/dev/null || true

    ip link set "$T113_WIFI_IFACE" down
    iw dev "$T113_WIFI_IFACE" set type __ap
    ip addr flush dev "$T113_WIFI_IFACE"
    ip addr add "$T113_AP_ADDRESS" dev "$T113_WIFI_IFACE"
    ip link set "$T113_WIFI_IFACE" up

    attempt=1
    while [ "$attempt" -le 3 ]; do
        hostapd -B "$T113_HOSTAPD_CONF" || true
        sleep 1
        if iw dev "$T113_WIFI_IFACE" info 2>/dev/null |
               grep -q 'type AP'; then
            break
        fi
        killall hostapd 2>/dev/null || true
        ip link set "$T113_WIFI_IFACE" down
        iw dev "$T113_WIFI_IFACE" set type __ap
        ip link set "$T113_WIFI_IFACE" up
        attempt=$((attempt + 1))
    done
    if ! iw dev "$T113_WIFI_IFACE" info 2>/dev/null | grep -q 'type AP'; then
        log "hostapd could not enable AP on $T113_WIFI_IFACE"
        return 1
    fi
    dnsmasq -C "$T113_DNSMASQ_CONF"
    log "AP ready: iface=$T113_WIFI_IFACE address=$T113_AP_ADDRESS"
}

stop_ap() {
    killall hostapd 2>/dev/null || true
    killall dnsmasq 2>/dev/null || true
    ip link set "$T113_WIFI_IFACE" down 2>/dev/null || true
}

status_ap() {
    iw dev "$T113_WIFI_IFACE" info 2>/dev/null || true
    ip -4 addr show dev "$T113_WIFI_IFACE" 2>/dev/null || true
    ps 2>/dev/null | grep -E '(hostapd|dnsmasq)' | grep -v grep || true
}

case "${1:-start}" in
    start) start_ap ;;
    stop) stop_ap ;;
    status) status_ap ;;
    *)
        echo "Usage: $0 {start|stop|status}" >&2
        exit 1
        ;;
esac
