#!/bin/sh

# T113 board-side video lifecycle/performance test helper.  This script does
# not alter the application data path; it only launches the fixed binary,
# samples system state, and enforces teardown baselines.

set -u

duration=${1:?duration seconds required}
name=${2:?session name required}
sample_interval=${3:-1}
force_stop_ms=${4:-0}

binary=${T113_TEST_BINARY:-/mnt/UDISK/t113_terminal_800x600_772ec72}
inspector=${T113_TEST_INSPECTOR:-/mnt/UDISK/mjpeg_mp4_inspect}
root=${T113_TEST_ROOT:-/mnt/usb/sda/video_overnight_20260826}
run_dir=$root/$name
video_path=$run_dir/video.mp4
app_pid=
generator_pids=

fixed_environment=$root/fixed.env
if [ ! -r "$fixed_environment" ]; then
    echo "missing fixed test environment: $fixed_environment" >&2
    exit 2
fi
. "$fixed_environment"
: "${T113_EXPECT_BOOT_ID:?missing T113_EXPECT_BOOT_ID}"
: "${T113_EXPECT_BINARY_SHA256:?missing T113_EXPECT_BINARY_SHA256}"
: "${T113_EXPECT_KERNEL_RELEASE:?missing T113_EXPECT_KERNEL_RELEASE}"

baseline_dma_count=${T113_BASELINE_DMA_COUNT:-1}
baseline_dma_bytes=${T113_BASELINE_DMA_BYTES:-4915200}
baseline_ion_count=${T113_BASELINE_ION_COUNT:-1}
baseline_ion_bytes=${T113_BASELINE_ION_BYTES:-4915200}
expect_mp4_valid=${T113_EXPECT_MP4_VALID:-1}
expect_app_rc=${T113_EXPECT_APP_RC:-0}
expect_recording_failure=${T113_EXPECT_RECORDING_FAILURE:-0}
expected_camera_mode=${T113_EXPECT_CAMERA_MODE:-Video: V4L2 MJPEG 800x600 @ 30 FPS started, buffers=3}

mkdir -p "$run_dir"
rm -f "$video_path" "$video_path.idx" "$run_dir/app.log" \
      "$run_dir/resources.tsv" "$run_dir/thread_ticks.tsv" \
      "$run_dir/cangen.log" "$run_dir/inspector.log" \
      "$run_dir/result.env"

clock_value()
{
    clock=$1
    column=$2
    awk -v clock="$clock" -v column="$column" \
        '$1 == clock { print $column; exit }' \
        /sys/kernel/debug/clk/clk_summary
}

dma_value()
{
    column=$1
    awk -v column="$column" \
        '/Total [0-9]+ objects/ { print $column; exit }' \
        /sys/kernel/debug/dma_buf/bufinfo
}

ion_value()
{
    field=$1
    total=0
    for heap in /sys/kernel/debug/ion/*; do
        if [ -r "$heap/$field" ]; then
            value=$(cat "$heap/$field")
            total=$((total + value))
        fi
    done
    printf '%s\n' "$total"
}

log_value()
{
    line=$1
    key=$2
    printf '%s\n' "$line" | sed -n "s/.*$key=\\([^ ]*\\).*/\\1/p"
}

process_exited()
{
    pid=$1
    seconds=$2
    waited=0
    while [ -r "/proc/$pid/stat" ] && \
          [ "$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)" != Z ] && \
          [ "$waited" -lt "$seconds" ]; do
        sleep 1
        waited=$((waited + 1))
    done
    [ ! -r "/proc/$pid/stat" ] || \
        [ "$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null)" = Z ]
}

process_value()
{
    pid=$1
    key=$2
    if [ -n "$pid" ] && [ -r "/proc/$pid/status" ]; then
        awk -v key="$key" '$1 == key":" { print $2; exit }' \
            "/proc/$pid/status"
    else
        printf '0\n'
    fi
}

snapshot()
{
    label=$1
    pid=${2:-}
    timestamp=$(date '+%Y-%m-%dT%H:%M:%S%z')
    uptime=$(cut -d' ' -f1 /proc/uptime)
    dma_count=$(dma_value 2)
    dma_bytes=$(dma_value 4)
    ion_count=$(ion_value num_of_buffers)
    ion_bytes=$(ion_value num_of_alloc_bytes)
    rss=$(process_value "$pid" VmRSS)
    vmsize=$(process_value "$pid" VmSize)
    threads=$(process_value "$pid" Threads)
    fds=0
    if [ -n "$pid" ] && [ -d "/proc/$pid/fd" ]; then
        fds=$(find "/proc/$pid/fd" -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)
    fi
    available=$(awk '/^MemAvailable:/ {print $2}' /proc/meminfo)
    slab=$(awk '/^Slab:/ {print $2}' /proc/meminfo)
    cma_free=$(awk '/^CmaFree:/ {print $2}' /proc/meminfo)
    load=$(cut -d' ' -f1-3 /proc/loadavg | tr ' ' '/')
    temperature=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0)
    frequency=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq \
                    2>/dev/null || echo 0)
    cpu_idle=$(awk '/^cpu / {print $5}' /proc/stat)

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$label" "$timestamp" "$uptime" "$pid" "$rss" "$vmsize" \
        "$threads" "$fds" "$dma_count" "$dma_bytes" "$ion_count" \
        "$ion_bytes" "$(clock_value g2d 2)" "$(clock_value g2d 3)" \
        "$(clock_value g2d 5)" "$(clock_value bus-g2d 2)" \
        "$(clock_value bus-g2d 3)" "$(clock_value mbus-g2d 2)" \
        "$(clock_value mbus-g2d 3)" "$available" "$slab" "$cma_free" \
        "$load" "$temperature" "$frequency/$cpu_idle" \
        >> "$run_dir/resources.tsv"

    awk -v label="$label" -v uptime="$uptime" \
        '/^cpu / {print label, uptime, $0}' /proc/stat \
        >> "$run_dir/thread_ticks.tsv"
    if [ -n "$pid" ] && [ -d "/proc/$pid/task" ]; then
        for task in /proc/$pid/task/*; do
            awk -v label="$label" -v uptime="$uptime" \
                '{print label, uptime, "task", $1, $2, $14, $15}' \
                "$task/stat" >> "$run_dir/thread_ticks.tsv" 2>/dev/null
        done
    fi
}

stop_generators()
{
    for generator in $generator_pids; do
        kill "$generator" 2>/dev/null || true
    done
    if [ -n "$generator_pids" ]; then
        wait $generator_pids 2>/dev/null || true
    fi
    generator_pids=
}

cleanup()
{
    if [ -n "$app_pid" ] && kill -0 "$app_pid" 2>/dev/null; then
        kill -INT "$app_pid" 2>/dev/null || true
        process_exited "$app_pid" 10 || {
            kill -TERM "$app_pid" 2>/dev/null || true
            process_exited "$app_pid" 5 || kill -KILL "$app_pid" 2>/dev/null || true
        }
        wait "$app_pid" 2>/dev/null || true
    fi
    stop_generators
}

trap cleanup EXIT INT TERM HUP

printf 'label\ttimestamp\tuptime\tpid\trss_kb\tvmsize_kb\tthreads\tfd\tdma_count\tdma_bytes\tion_count\tion_bytes\tg2d_enable\tg2d_prepare\tg2d_rate\tbus_enable\tbus_prepare\tmbus_enable\tmbus_prepare\tmem_available_kb\tslab_kb\tcma_free_kb\tload\ttemp_mc\tcpu_freq_khz_cpu_idle_ticks\n' \
    > "$run_dir/resources.tsv"
: > "$run_dir/thread_ticks.tsv"
: > "$run_dir/cangen.log"

current_boot_id=$(cat /proc/sys/kernel/random/boot_id)
current_binary_sha256=$(sha256sum "$binary" | cut -d' ' -f1)
current_kernel_release=$(uname -r)
if [ "$current_boot_id" != "$T113_EXPECT_BOOT_ID" ] || \
   [ "$current_binary_sha256" != "$T113_EXPECT_BINARY_SHA256" ] || \
   [ "$current_kernel_release" != "$T113_EXPECT_KERNEL_RELEASE" ]; then
    echo "fixed environment mismatch: boot=$current_boot_id binary=$current_binary_sha256 kernel=$current_kernel_release" >&2
    exit 3
fi

ip link set awlink0 down 2>/dev/null || true
ip link set awlink1 down 2>/dev/null || true
ip link set awlink0 type can bitrate 500000 loopback on
ip link set awlink1 type can bitrate 500000 loopback on
ip link set awlink0 up
ip link set awlink1 up

snapshot before

cangen awlink0 -g 20 -I 121 -L 8 -D 993A8A1300000000 >> "$run_dir/cangen.log" 2>&1 &
generator_pids="$generator_pids $!"
cangen awlink0 -g 50 -I 201 -L 8 -D A00F000000000000 >> "$run_dir/cangen.log" 2>&1 &
generator_pids="$generator_pids $!"
cangen awlink0 -g 50 -I 123 -L 8 -D 0000F20300000000 >> "$run_dir/cangen.log" 2>&1 &
generator_pids="$generator_pids $!"
cangen awlink1 -g 10 -I 321 -L 8 -D i >> "$run_dir/cangen.log" 2>&1 &
generator_pids="$generator_pids $!"

sleep 1
for generator in $generator_pids; do
    if ! kill -0 "$generator" 2>/dev/null; then
        echo "CAN load generator failed: pid=$generator" >&2
        exit 4
    fi
done

if [ -n "${T113_FAULT_WRITE_MODE:-}" ]; then
    export T113_FAULT_WRITE_PATH="$video_path"
fi

cd "$run_dir" || exit 1
"$binary" --video --camera /dev/video0 --video-record "$video_path" \
    --video-seconds "$duration" --video-compositor g2d \
    > "$run_dir/app.log" 2>&1 &
app_pid=$!

elapsed=0
active_seen=0
stop_timeout=0
while kill -0 "$app_pid" 2>/dev/null; do
    snapshot "run_$elapsed" "$app_pid"
    if [ "$(clock_value g2d 2)/$(clock_value g2d 3)" = 1/1 ]; then
        active_seen=1
        if [ "$force_stop_ms" -gt 0 ]; then
            sleep "0.$(printf '%03d' "$force_stop_ms")"
            kill -INT "$app_pid" 2>/dev/null || true
            break
        fi
    fi
    if [ "$force_stop_ms" -eq 0 ] && [ "$elapsed" -gt $((duration + 1)) ]; then
        kill -INT "$app_pid" 2>/dev/null || true
        break
    fi
    sleep "$sample_interval"
    elapsed=$((elapsed + sample_interval))
done

if kill -0 "$app_pid" 2>/dev/null && ! process_exited "$app_pid" 15; then
    stop_timeout=1
    kill -TERM "$app_pid" 2>/dev/null || true
    if ! process_exited "$app_pid" 5; then
        kill -KILL "$app_pid" 2>/dev/null || true
        process_exited "$app_pid" 5 || true
    fi
fi
wait "$app_pid"
app_rc=$?
app_pid=
stop_generators
sleep 1
snapshot after

inspect_rc=not_run
if [ -s "$video_path" ]; then
    "$inspector" "$video_path" > "$run_dir/inspector.log" 2>&1
    inspect_rc=$?
fi

after_dma_count=$(dma_value 2)
after_dma_bytes=$(dma_value 4)
after_ion_count=$(ion_value num_of_buffers)
after_ion_bytes=$(ion_value num_of_alloc_bytes)
after_g2d="$(clock_value g2d 2)/$(clock_value g2d 3)"
after_bus="$(clock_value bus-g2d 2)/$(clock_value bus-g2d 3)"
after_mbus="$(clock_value mbus-g2d 2)/$(clock_value mbus-g2d 3)"
after_boot_id=$(cat /proc/sys/kernel/random/boot_id)
after_binary_sha256=$(sha256sum "$binary" | cut -d' ' -f1)
after_kernel_release=$(uname -r)

statistics=$(grep 'Video statistics:' "$run_dir/app.log" | tail -n 1)
g2d_profile=$(grep 'Video G2D profile:' "$run_dir/app.log" | tail -n 1)
writer_profile=$(grep 'Video writer latency:' "$run_dir/app.log" | tail -n 1)
inspector_profile=$(grep '^file=' "$run_dir/inspector.log" 2>/dev/null | tail -n 1)
written=$(log_value "$statistics" written)
mp4_frames=$(log_value "$inspector_profile" frames)
mp4_duration=$(log_value "$inspector_profile" duration | sed 's/s$//')
mp4_pts=$(log_value "$inspector_profile" pts_monotonic)
recording_failed=$(log_value "$g2d_profile" recording_failed)
writer_failed=$(log_value "$writer_profile" failed)
can0_frames=$(sed -n 's/.*CAN0 (awlink0): rx_frames=\([0-9][0-9]*\).*/\1/p' "$run_dir/app.log" | tail -n 1)
can1_frames=$(sed -n 's/.*CAN1 (awlink1): rx_frames=\([0-9][0-9]*\).*/\1/p' "$run_dir/app.log" | tail -n 1)
camera_mode_seen=0
grep -Fq "$expected_camera_mode" "$run_dir/app.log" && camera_mode_seen=1
idx_absent=1
[ -e "$video_path.idx" ] && idx_absent=0

{
    echo "APP_RC=$app_rc"
    echo "ACTIVE_SEEN=$active_seen"
    echo "STOP_TIMEOUT=$stop_timeout"
    echo "INSPECT_RC=$inspect_rc"
    echo "AFTER_G2D=$after_g2d"
    echo "AFTER_BUS_G2D=$after_bus"
    echo "AFTER_MBUS_G2D=$after_mbus"
    echo "AFTER_DMA_COUNT=$after_dma_count"
    echo "AFTER_DMA_BYTES=$after_dma_bytes"
    echo "AFTER_ION_COUNT=$after_ion_count"
    echo "AFTER_ION_BYTES=$after_ion_bytes"
    echo "AFTER_BOOT_ID=$after_boot_id"
    echo "AFTER_BINARY_SHA256=$after_binary_sha256"
    echo "AFTER_KERNEL_RELEASE=$after_kernel_release"
    echo "CAMERA_MODE_SEEN=$camera_mode_seen"
    echo "WRITTEN=$written"
    echo "MP4_FRAMES=$mp4_frames"
    echo "MP4_DURATION=$mp4_duration"
    echo "MP4_PTS_MONOTONIC=$mp4_pts"
    echo "RECORDING_FAILED=$recording_failed"
    echo "WRITER_FAILED=$writer_failed"
    echo "CAN0_FRAMES=$can0_frames"
    echo "CAN1_FRAMES=$can1_frames"
    echo "IDX_ABSENT=$idx_absent"
} > "$run_dir/result.env"

status=0
if [ "$app_rc" -ne "$expect_app_rc" ] || [ "$active_seen" -ne 1 ] || \
   [ "$stop_timeout" -ne 0 ] || [ "$camera_mode_seen" -ne 1 ] || \
   [ "$after_g2d" != 0/0 ] || [ "$after_bus" != 0/0 ] || \
   [ "$after_mbus" != 0/0 ] || \
   [ "$after_dma_count" -ne "$baseline_dma_count" ] || \
   [ "$after_dma_bytes" -ne "$baseline_dma_bytes" ] || \
   [ "$after_ion_count" -ne "$baseline_ion_count" ] || \
   [ "$after_ion_bytes" -ne "$baseline_ion_bytes" ] || \
   [ "$after_boot_id" != "$T113_EXPECT_BOOT_ID" ] || \
   [ "$after_binary_sha256" != "$T113_EXPECT_BINARY_SHA256" ] || \
   [ "$after_kernel_release" != "$T113_EXPECT_KERNEL_RELEASE" ] || \
   [ -z "$can0_frames" ] || [ "$can0_frames" -le 0 ] || \
   [ -z "$can1_frames" ] || [ "$can1_frames" -le 0 ]; then
    status=90
fi
if [ "$expect_recording_failure" -eq 1 ]; then
    if [ "$recording_failed" != yes ] && [ "$writer_failed" != yes ]; then
        status=92
    fi
elif [ "$force_stop_ms" -eq 0 ] && [ "$expect_mp4_valid" -eq 1 ]; then
    duration_ok=$(awk -v actual="$mp4_duration" -v requested="$duration" \
        'BEGIN { low=requested-1.5; if (low < 0) low=0; exit !(actual >= low && actual <= requested+1.5) }' \
        && echo 1 || echo 0)
    if [ "$inspect_rc" != 0 ] || [ -z "$written" ] || [ "$written" -le 0 ] || \
       [ "$mp4_frames" != "$written" ] || [ "$mp4_pts" != yes ] || \
       [ "$recording_failed" != no ] || [ "$writer_failed" != no ] || \
       [ "$idx_absent" -ne 1 ] || [ "$duration_ok" -ne 1 ]; then
        status=91
    fi
elif [ "$force_stop_ms" -gt 0 ] && [ -n "$written" ] && [ "$written" -gt 0 ]; then
    if [ "$inspect_rc" != 0 ] || [ "$mp4_frames" != "$written" ] || \
       [ "$mp4_pts" != yes ] || [ "$idx_absent" -ne 1 ]; then
        status=91
    fi
fi

echo "STATUS=$status" >> "$run_dir/result.env"
printf '%s status=%s rc=%s active=%s clocks=%s,%s,%s dma=%s/%s ion=%s/%s inspect=%s\n' \
    "$name" "$status" "$app_rc" "$active_seen" "$after_g2d" \
    "$after_bus" "$after_mbus" "$after_dma_count" "$after_dma_bytes" \
    "$after_ion_count" "$after_ion_bytes" "$inspect_rc"

trap - EXIT INT TERM
exit "$status"
