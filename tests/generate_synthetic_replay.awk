# Generate a deterministic historical replay fixture for regression testing.
# Usage: mkdir -p OUTPUT && awk -v output_dir=OUTPUT -f this_file

BEGIN {
    if (output_dir == "") {
        print "output_dir is required" > "/dev/stderr"
        exit 2
    }

    can0_path = output_dir "/can0.csv"
    can1_path = output_dir "/can1.csv"
    lappro_path = output_dir "/lappro.csv"
    print "timestamp_us,bus,id,flags,dlc,data" > can0_path
    print "timestamp_us,bus,id,flags,dlc,data" > can1_path
    print "timestamp_us,gps_time,connected,authenticated,streaming,fix_type," \
          "num_sv,speed_kmh,heading_deg,hacc_m,altitude,longitude,latitude," \
          "acc_x,acc_y,acc_z" > lappro_path

    pi = atan2(0, -1)
    origin_latitude = 30.0
    origin_longitude = 120.0
    latitude_scale = cos(origin_latitude * pi / 180.0)
    radius_m = 100.0
    loop_period_s = 90.0
    duration_s = 285
    first_timestamp_us = 1000000

    for (sample = 0; sample <= duration_s * 50; ++sample) {
        elapsed_s = sample / 50.0
        phase = 2.0 * pi * elapsed_s / loop_period_s
        x_m = radius_m * cos(phase)
        y_m = radius_m * sin(phase)
        latitude = origin_latitude + y_m / 111111.0
        longitude = origin_longitude + x_m / (111111.0 * latitude_scale)
        heading_deg = atan2(-sin(phase), cos(phase)) * 180.0 / pi
        if (heading_deg < 0.0) {
            heading_deg += 360.0
        }
        timestamp_us = first_timestamp_us + sample * 20000
        printf "%d,n/a,1,1,1,3,12,25.000000,%.6f,1.000000,100.000000," \
               "%.8f,%.8f,0.000000,0.000000,1.000000\n", \
               timestamp_us, heading_deg, longitude, latitude > lappro_path
    }

    for (sample = 0; sample <= duration_s * 20; ++sample) {
        timestamp_us = first_timestamp_us + sample * 50000
        print timestamp_us ",0,0x121,0,4,99 3a 8a 13" > can0_path
        print timestamp_us + 3 ",0,0x122,0,2,48 55" > can0_path
        print timestamp_us + 4 ",0,0x201,0,4,00 00 00 23" > can0_path
        print timestamp_us + 1 ",1,0x01e,0,8,00 00 00 00 60 09 00 00" > can1_path
        print timestamp_us + 2 ",1,0x00e,0,8,00 00 00 00 00 00 e8 80" > can1_path
    }

    close(can0_path)
    close(can1_path)
    close(lappro_path)
}
