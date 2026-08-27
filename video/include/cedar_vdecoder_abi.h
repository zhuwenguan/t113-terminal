#ifndef T113_CEDAR_VDECODER_ABI_H
#define T113_CEDAR_VDECODER_ABI_H

#include <stdint.h>

/* Minimal Cedar vdecoder ABI used by the T113 board image. */

/*
 * Tina's JPEG wrapper uses the legacy Cedar JPEG codec slot.  The public
 * Cedar header calls 0x106 MJPEG, but the T113 image maps 0x101 to the JPEG
 * decoder plugin and maps 0x106 to MSMPEG4V2.  Keep the board ABI value
 * explicit instead of relying on the newer public enum.
 */
enum {
    CEDAR_VIDEO_CODEC_FORMAT_MJPEG = 0x101
};

enum {
    CEDAR_PIXEL_FORMAT_YUV_PLANER_420 = 1,
    CEDAR_PIXEL_FORMAT_YV12 = 4,
    CEDAR_PIXEL_FORMAT_NV21 = 5,
    CEDAR_PIXEL_FORMAT_NV12 = 6
};

enum {
    CEDAR_VDECODE_RESULT_OK = 0,
    CEDAR_VDECODE_RESULT_FRAME_DECODED = 1,
    CEDAR_VDECODE_RESULT_CONTINUE = 2,
    CEDAR_VDECODE_RESULT_KEYFRAME_DECODED = 3
};

typedef struct {
    int eCodecFormat;
    int nWidth;
    int nHeight;
    int nFrameRate;
    int nFrameDuration;
    int nAspectRatio;
    int nCodecSpecificDataLen;
    char *pCodecSpecificData;
} CedarVideoStreamInfo;

/*
 * T113's libvdecoder consumes the extended Cedar VConfig used by
 * libjpegdecode.so.  The first 11 words are the documented scale/rotation
 * controls; the later words are private Tina fields.  Passing only the
 * two-word public VConfig makes the board read unrelated stack data.
 */
#define CEDAR_VCONFIG_WORDS 46U

enum {
    CEDAR_VCONFIG_SCALE_DOWN = 0U,
    CEDAR_VCONFIG_ROTATION = 1U,
    CEDAR_VCONFIG_H_SCALE_DOWN = 2U,
    CEDAR_VCONFIG_V_SCALE_DOWN = 3U,
    CEDAR_VCONFIG_SEC_H_SCALE_DOWN = 4U,
    CEDAR_VCONFIG_SEC_V_SCALE_DOWN = 5U,
    CEDAR_VCONFIG_ROTATE_DEGREE = 6U,
    CEDAR_VCONFIG_THUMBNAIL = 7U,
    CEDAR_VCONFIG_NO_B_FRAMES = 8U,
    CEDAR_VCONFIG_DISABLE_3D = 9U,
    CEDAR_VCONFIG_SUPPORTED_MAF = 10U,
    CEDAR_VCONFIG_OUTPUT_PIXEL_FORMAT = 12U,
    CEDAR_VCONFIG_VBV_BUFFER_SIZE = 18U,
    CEDAR_VCONFIG_MEMORY_OPERATIONS = 32U
};

typedef struct {
    int value[CEDAR_VCONFIG_WORDS];
} CedarVConfig;

typedef struct {
    char *pData;
    int nLength;
    int64_t nPts;
    int64_t nPcr;
    int bIsFirstPart;
    int bIsLastPart;
    int nID;
    int nStreamIndex;
    int bValid;
} CedarVideoStreamDataInfo;

typedef struct {
    int nID;
    int nStreamIndex;
    int ePixelFormat;
    int nWidth;
    int nHeight;
    int nLineStride;
    int nTopOffset;
    int nLeftOffset;
    int nBottomOffset;
    int nRightOffset;
    int nFrameRate;
    int nAspectRatio;
    int bIsProgressive;
    int bTopFieldFirst;
    int bRepeatTopField;
    int64_t nPts;
    int64_t nPcr;
    char *pData0;
    char *pData1;
    char *pData2;
    char *pData3;
    unsigned int phyYBufAddr;
    unsigned int phyCBufAddr;
} CedarVideoPicture;

typedef void *CedarVideoDecoder;

typedef CedarVideoDecoder *(*CedarCreateVideoDecoder)(void);
typedef void (*CedarDestroyVideoDecoder)(CedarVideoDecoder *decoder);
typedef int (*CedarInitializeVideoDecoder)(CedarVideoDecoder *decoder,
                                           CedarVideoStreamInfo *info,
                                           CedarVConfig *config);
typedef void (*CedarResetVideoDecoder)(CedarVideoDecoder *decoder);
typedef int (*CedarDecodeVideoStream)(CedarVideoDecoder *decoder,
                                      int end_of_stream,
                                      int decode_keyframe_only,
                                      int drop_bframe_if_delay,
                                      int64_t current_time_us);
typedef int (*CedarRequestVideoStreamBuffer)(CedarVideoDecoder *decoder,
                                             int required_size,
                                             char **buffer,
                                             int *buffer_size,
                                             char **ring_buffer,
                                             int *ring_buffer_size,
                                             int stream_index);
typedef int (*CedarSubmitVideoStreamData)(CedarVideoDecoder *decoder,
                                          CedarVideoStreamDataInfo *data,
                                          int stream_index);
typedef CedarVideoPicture *(*CedarRequestPicture)(CedarVideoDecoder *decoder,
                                                  int stream_index);
typedef int (*CedarReturnPicture)(CedarVideoDecoder *decoder,
                                  CedarVideoPicture *picture);

#endif
