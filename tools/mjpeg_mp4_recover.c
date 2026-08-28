#define _FILE_OFFSET_BITS 64

#include "video_recorder.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    const char *path;

    if (argc != 2) {
        fprintf(stderr, "usage: %s FILE.mp4\n", argv[0]);
        return EXIT_FAILURE;
    }
    path = argv[1];
    if (video_recorder_recover_file(path, 800U, 600U, 30U) < 0) {
        fprintf(stderr, "cannot recover %s: %s\n", path, strerror(errno));
        return EXIT_FAILURE;
    }
    printf("recovered=%s\n", path);
    return EXIT_SUCCESS;
}
