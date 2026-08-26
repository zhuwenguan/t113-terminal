#!/bin/sh

# Board-side orchestrator for the fixed 800x600 overnight validation.  Run one
# phase at a time so an interrupted SSH connection does not lose test state.

set -u

phase=${1:?phase required}
runner=${T113_TEST_RUNNER:-/mnt/UDISK/t113_video_session_test.sh}
root=${T113_TEST_ROOT:-/mnt/usb/sda/video_overnight_20260826}
summary=$root/sessions.tsv

mkdir -p "$root"
if [ ! -e "$summary" ]; then
    printf 'phase\tindex\tname\tduration_s\tstart\tstop\tapp_rc\tframes\tlost\tencoded\twritten\tapp_drop\tfps\tdecode_avg\tdecode_p95\tdecode_p99\tdecode_max\tyuv_avg\tyuv_p95\tyuv_p99\tyuv_max\tblend_avg\tblend_p95\tblend_p99\tblend_max\tencode_avg\tencode_p95\tencode_p99\tencode_max\tvideo_cpu\tqueue_avg\tqueue_peak\tqueue_full\tqueue_drop\twriter_avg\twriter_p95\twriter_p99\twriter_max\twriter_gt100\trss_max\tafter_g2d\tafter_bus\tafter_mbus\tafter_dma\tafter_ion\tinspect_rc\tmp4_frames\tmp4_duration\tmp4_fps\tpts\n' > "$summary"
fi

value()
{
    text=$1
    key=$2
    printf '%s\n' "$text" | sed -n "s/.*$key=\\([^ ]*\\).*/\\1/p"
}

append_result()
{
    group=$1
    index=$2
    name=$3
    duration=$4
    directory=$root/$name
    app_log=$directory/app.log
    result=$directory/result.env
    inspector=$directory/inspector.log
    statistics=$(grep 'Video statistics:' "$app_log" | tail -n 1)
    profile=$(grep 'Video profile:' "$app_log" | tail -n 1)
    g2d_profile=$(grep 'Video G2D profile:' "$app_log" | tail -n 1)
    queue=$(grep 'Video writer queue:' "$app_log" | tail -n 1)
    writer=$(grep 'Video writer latency:' "$app_log" | tail -n 1)
    decode=$(grep 'Video latency mjpeg_decode' "$app_log" | tail -n 1)
    yuv=$(grep 'Video latency yuv_to_argb' "$app_log" | tail -n 1)
    blend=$(grep 'Video latency g2d_blend' "$app_log" | tail -n 1)
    encode=$(grep 'Video latency jpeg_encode' "$app_log" | tail -n 1)
    mp4=$(grep '^file=' "$inspector" 2>/dev/null | tail -n 1)
    start=$(awk -F '\t' 'NR == 2 {print $2}' "$directory/resources.tsv")
    stop=$(awk -F '\t' 'END {print $2}' "$directory/resources.tsv")
    app_rc=$(sed -n 's/^APP_RC=//p' "$result")
    after_g2d=$(sed -n 's/^AFTER_G2D=//p' "$result")
    after_bus=$(sed -n 's/^AFTER_BUS_G2D=//p' "$result")
    after_mbus=$(sed -n 's/^AFTER_MBUS_G2D=//p' "$result")
    after_dma="$(sed -n 's/^AFTER_DMA_COUNT=//p' "$result")/$(sed -n 's/^AFTER_DMA_BYTES=//p' "$result")"
    after_ion="$(sed -n 's/^AFTER_ION_COUNT=//p' "$result")/$(sed -n 's/^AFTER_ION_BYTES=//p' "$result")"
    inspect_rc=$(sed -n 's/^INSPECT_RC=//p' "$result")

    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$group" "$index" "$name" "$duration" "$start" "$stop" \
        "$app_rc" "$(value "$statistics" camera)" \
        "$(value "$statistics" lost)" "$(value "$statistics" encoded)" \
        "$(value "$statistics" written)" "$(value "$statistics" app_drop)" \
        "$(value "$statistics" capture_fps)" "$(value "$decode" avg)" \
        "$(value "$decode" p95)" "$(value "$decode" p99)" \
        "$(value "$decode" max)" "$(value "$yuv" avg)" \
        "$(value "$yuv" p95)" "$(value "$yuv" p99)" \
        "$(value "$yuv" max)" "$(value "$blend" avg)" \
        "$(value "$blend" p95)" "$(value "$blend" p99)" \
        "$(value "$blend" max)" "$(value "$encode" avg)" \
        "$(value "$encode" p95)" "$(value "$encode" p99)" \
        "$(value "$encode" max)" "$(value "$g2d_profile" process_cpu)" \
        "$(value "$queue" avg)" "$(value "$queue" max)" \
        "$(value "$queue" full)" "$(value "$queue" dropped)" \
        "$(value "$writer" avg)" "$(value "$writer" p95)" \
        "$(value "$writer" p99)" "$(value "$writer" max)" \
        "$(value "$writer" '>100ms')" "$(value "$profile" max_rss)" \
        "$after_g2d" "$after_bus" "$after_mbus" "$after_dma" \
        "$after_ion" "$inspect_rc" "$(value "$mp4" frames)" \
        "$(value "$mp4" duration)" "$(value "$mp4" fps)" \
        "$(value "$mp4" pts_monotonic)" >> "$summary"
}

run_normal()
{
    group=$1
    index=$2
    duration=$3
    name=$4
    interval=${5:-1}
    echo "START phase=$group index=$index name=$name duration=$duration $(date '+%Y-%m-%dT%H:%M:%S%z')"
    "$runner" "$duration" "$name" "$interval"
    run_rc=$?
    if [ "$run_rc" -ne 0 ]; then
        echo "FAIL phase=$group index=$index name=$name rc=$run_rc $(date '+%Y-%m-%dT%H:%M:%S%z')" >&2
        exit "$run_rc"
    fi
    append_result "$group" "$index" "$name" "$duration"
    echo "PASS phase=$group index=$index name=$name $(date '+%Y-%m-%dT%H:%M:%S%z')"
}

run_rapid()
{
    index=$1
    delay=$2
    name=$(printf 'rapid_%02d_%03dms' "$index" "$delay")
    echo "START phase=rapid index=$index delay_ms=$delay $(date '+%Y-%m-%dT%H:%M:%S%z')"
    "$runner" 60 "$name" 1 "$delay"
    run_rc=$?
    if [ "$run_rc" -ne 0 ]; then
        echo "FAIL phase=rapid index=$index name=$name rc=$run_rc $(date '+%Y-%m-%dT%H:%M:%S%z')" >&2
        exit "$run_rc"
    fi
    append_result rapid "$index" "$name" "active+$delay-ms"
    echo "PASS phase=rapid index=$index name=$name $(date '+%Y-%m-%dT%H:%M:%S%z')"
}

case "$phase" in
    lifecycle100)
        index=1
        while [ "$index" -le 100 ]; do
            name=$(printf 'lifecycle_%03d' "$index")
            run_normal lifecycle "$index" 5 "$name" 1
            index=$((index + 1))
        done
        ;;
    mixed30)
        sequence='1 30 2 60 5 180 10 300 2 10 1 60 5 30 180 300 1 5 30 2 300 10 60 180 5 1 180 30 2 60 10 300'
        index=1
        for duration in $sequence; do
            name=$(printf 'mixed_%02d_%03ds' "$index" "$duration")
            run_normal mixed "$index" "$duration" "$name" 2
            index=$((index + 1))
        done
        ;;
    rapid50)
        index=1
        while [ "$index" -le 50 ]; do
            case $((index % 4)) in
                0) delay=200 ;;
                1) delay=300 ;;
                2) delay=400 ;;
                *) delay=500 ;;
            esac
            run_rapid "$index" "$delay"
            index=$((index + 1))
        done
        ;;
    benchmark)
        run_normal benchmark 1 10 benchmark_10s 1
        run_normal benchmark 2 60 benchmark_60s 2
        run_normal benchmark 3 300 benchmark_5min 5
        ;;
    long2h)
        run_normal long 1 7200 long_2h 30
        ;;
    postlong50)
        index=1
        while [ "$index" -le 50 ]; do
            name=$(printf 'postlong_%02d' "$index")
            run_normal postlong "$index" 5 "$name" 1
            index=$((index + 1))
        done
        ;;
    long4h)
        run_normal long 2 14400 long_4h 60
        ;;
    *)
        echo "unknown phase: $phase" >&2
        exit 2
        ;;
esac

echo "PHASE_PASS $phase $(date '+%Y-%m-%dT%H:%M:%S%z')"
