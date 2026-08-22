#include "jpeg_codec.h"
#include "video_frame.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    FILE *file;
    long file_size;
    uint8_t *data;
    JpegDecoder decoder;
    VideoFrame frame;
    int result = EXIT_FAILURE;

    if (argc != 2) {
        fprintf(stderr, "usage: %s image.mjpg\n", argv[0]);
        return EXIT_FAILURE;
    }
    file = fopen(argv[1], "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0 ||
        (file_size = ftell(file)) <= 0 || fseek(file, 0, SEEK_SET) != 0) {
        perror("open JPEG");
        if (file != NULL) {
            fclose(file);
        }
        return EXIT_FAILURE;
    }
    data = malloc((size_t)file_size);
    if (data == NULL || fread(data, 1U, (size_t)file_size, file) !=
                       (size_t)file_size) {
        perror("read JPEG");
        free(data);
        fclose(file);
        return EXIT_FAILURE;
    }
    fclose(file);
    memset(&decoder, 0, sizeof(decoder));
    memset(&frame, 0, sizeof(frame));
    if (jpeg_decoder_init(&decoder, 1280U) < 0 ||
        video_frame_init(&frame, 1280U, 720U) < 0) {
        fprintf(stderr, "decoder/frame init failed: %s\n", strerror(errno));
        jpeg_decoder_close(&decoder);
        free(data);
        return EXIT_FAILURE;
    }
    if (jpeg_decoder_decode(&decoder, data, (size_t)file_size, &frame)) {
        result = EXIT_SUCCESS;
    }
    video_frame_close(&frame);
    jpeg_decoder_close(&decoder);
    free(data);
    return result;
}
