#ifndef T113_CEDAR_VENCODER_ABI_H
#define T113_CEDAR_VENCODER_ABI_H

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal ABI for the libvencoder.so shipped in the KickPi T113 image.
 *
 * This is the 32-bit Tina Cedar vencoder interface.  The declarations mirror
 * the vendor vencoder.h used to build libvenc_jpeg.so; keep this file small so
 * the application does not depend on a board SDK being installed at build
 * time.
 */

typedef enum {
    CEDAR_VENC_CODEC_H264 = 0,
    CEDAR_VENC_CODEC_JPEG = 1
} CedarVencCodecType;

typedef enum {
    CEDAR_VENC_PIXEL_YUV420SP = 0,
    CEDAR_VENC_PIXEL_YVU420SP = 1,
    CEDAR_VENC_PIXEL_YUV420P = 2,
    CEDAR_VENC_PIXEL_YVU420P = 3,
    CEDAR_VENC_PIXEL_YUV422SP = 4,
    CEDAR_VENC_PIXEL_YVU422SP = 5,
    CEDAR_VENC_PIXEL_YUV422P = 6,
    CEDAR_VENC_PIXEL_YVU422P = 7,
    CEDAR_VENC_PIXEL_YUYV422 = 8,
    CEDAR_VENC_PIXEL_UYVY422 = 9,
    CEDAR_VENC_PIXEL_YVYU422 = 10,
    CEDAR_VENC_PIXEL_VYUY422 = 11,
    CEDAR_VENC_PIXEL_ARGB = 12,
    CEDAR_VENC_PIXEL_RGBA = 13,
    CEDAR_VENC_PIXEL_ABGR = 14,
    CEDAR_VENC_PIXEL_BGRA = 15,
    CEDAR_VENC_PIXEL_TILE_32X32 = 16,
    CEDAR_VENC_PIXEL_TILE_128X32 = 17,
    CEDAR_VENC_PIXEL_AFBC_AW = 18
} CedarVencPixelFormat;

/*
 * Overlay ABI from the Allwinner Cedar vencoder.h family.  Reference source:
 * https://github.com/Allwinner-Homlet/H6-CedarC/blob/master/include/vencoder.h
 * The board's
 * libvenc_jpeg.so independently confirms VENC_IndexParamSetOverlay (0x404),
 * the 64-block limit, the ARGB type field, and the overlay header fields in
 * its JpegSetParameter implementation.  Keep these declarations separate
 * from the production encoder until the board smoke test validates them.
 */
#define CEDAR_VENC_MAX_OVERLAY_SIZE 64U
#define CEDAR_VENC_INDEX_PARAM_SET_OVERLAY 0x404
#define CEDAR_VENC_INDEX_PARAM_RGB2YUV 0x009
#define CEDAR_VENC_INDEX_PARAM_COLOR_FORMAT 0x00d
#define CEDAR_VENC_INDEX_PARAM_SIZE 0x00e
#define CEDAR_VENC_INDEX_PARAM_MAX_SUPPORT_SIZE 0x016
#define CEDAR_VENC_INDEX_PARAM_CHECK_COLOR_FORMAT 0x017

typedef enum {
    CEDAR_VENC_OVERLAY_ARGB_MIN = -1,
    CEDAR_VENC_OVERLAY_ARGB8888 = 0,
    CEDAR_VENC_OVERLAY_ARGB4444 = 1,
    CEDAR_VENC_OVERLAY_ARGB1555 = 2,
    CEDAR_VENC_OVERLAY_ARGB_MAX = 3
} CedarVencOverlayArgbType;

typedef enum {
    CEDAR_VENC_NORMAL_OVERLAY = 0,
    CEDAR_VENC_COVER_OVERLAY = 1,
    CEDAR_VENC_LUMA_REVERSE_OVERLAY = 2
} CedarVencOverlayType;

typedef struct {
    uint8_t use_cover_yuv_flag;
    uint8_t cover_y;
    uint8_t cover_u;
    uint8_t cover_v;
} CedarVencOverlayCoverYuv;

typedef struct {
    uint16_t start_mb_x;
    uint16_t end_mb_x;
    uint16_t start_mb_y;
    uint16_t end_mb_y;
    uint8_t extra_alpha_flag;
    uint8_t extra_alpha;
    CedarVencOverlayCoverYuv cover_yuv;
    CedarVencOverlayType overlay_type;
    uint8_t *overlay_blk_addr;
    uint32_t bitmap_size;
} CedarVencOverlayHeader;

typedef struct {
    uint8_t blk_num;
    CedarVencOverlayArgbType argb_type;
    CedarVencOverlayHeader overlay_header_list[CEDAR_VENC_MAX_OVERLAY_SIZE];
} CedarVencOverlayInfo;

typedef struct {
    int index;
    CedarVencPixelFormat color_format;
} CedarVencCheckColorFormat;

#if UINTPTR_MAX == 0xffffffffU
_Static_assert(offsetof(CedarVencOverlayHeader, overlay_blk_addr) == 20U,
               "unexpected Cedar overlay header pointer offset");
_Static_assert(offsetof(CedarVencOverlayHeader, bitmap_size) == 24U,
               "unexpected Cedar overlay header bitmap offset");
_Static_assert(sizeof(CedarVencOverlayHeader) == 28U,
               "unexpected Cedar overlay header size");
_Static_assert(offsetof(CedarVencOverlayInfo, overlay_header_list) == 8U,
               "unexpected Cedar overlay info header offset");
#endif

typedef struct {
    int nLeft;
    int nTop;
    int nWidth;
    int nHeight;
} CedarVencRect;

typedef struct {
    int bEnable;
    int index;
    int nQPoffset;
    uint8_t roi_abs_flag;
    CedarVencRect sRect;
} CedarVencRoiConfig;

typedef struct {
    uint8_t bEncH264Nalu;
    uint32_t nInputWidth;
    uint32_t nInputHeight;
    uint32_t nDstWidth;
    uint32_t nDstHeight;
    uint32_t nStride;
    CedarVencPixelFormat eInputFormat;
    void *memops;
    void *veOpsS;
    void *pVeOpsSelf;
    uint8_t bOnlyWbFlag;
    uint8_t bLbcLossyComEnFlag2x;
    uint8_t bLbcLossyComEnFlag2_5x;
    uint8_t bIsVbvNoCache;
} CedarVencBaseConfig;

typedef struct {
    unsigned long nID;
    int64_t nPts;
    uint32_t nFlag;
    uint8_t *pAddrPhyY;
    uint8_t *pAddrPhyC;
    uint8_t *pAddrVirY;
    uint8_t *pAddrVirC;
    int nWidth;
    int nHeight;
    int nAlign;
    int bEnableCorp;
    CedarVencRect sCropInfo;
    int ispPicVar;
    int ispPicVarChroma;
    int bUseInputBufferRoi;
    CedarVencRoiConfig roi_param[8];
    int bAllocMemSelf;
    int nShareBufFd;
    uint8_t bUseCsiColorFormat;
    CedarVencPixelFormat eCsiColorFormat;
    int envLV;
} CedarVencInputBuffer;

typedef struct {
    int nID;
    int64_t nPts;
    uint32_t nFlag;
    uint32_t nSize0;
    uint32_t nSize1;
    uint8_t *pData0;
    uint8_t *pData1;
    struct {
        int CurrQp;
        int avQp;
        int nGopIndex;
        int nFrameIndex;
        int nTotalIndex;
    } frame_info;
    uint32_t nSize2;
    uint8_t *pData2;
} CedarVencOutputBuffer;

typedef struct {
    uint32_t nBufferNum;
    uint32_t nSizeY;
    uint32_t nSizeC;
} CedarVencAllocateBufferParam;

typedef void *CedarVideoEncoder;

typedef CedarVideoEncoder *(*CedarVideoEncCreate)(CedarVencCodecType codec);
typedef void (*CedarVideoEncDestroy)(CedarVideoEncoder *encoder);
typedef int (*CedarVideoEncInit)(CedarVideoEncoder *encoder,
                                 CedarVencBaseConfig *config);
typedef int (*CedarVideoEncUnInit)(CedarVideoEncoder *encoder);
typedef int (*CedarAllocInputBuffer)(CedarVideoEncoder *encoder,
                                     CedarVencAllocateBufferParam *param);
typedef int (*CedarGetOneAllocInputBuffer)(CedarVideoEncoder *encoder,
                                           CedarVencInputBuffer *buffer);
typedef int (*CedarFlushCacheAllocInputBuffer)(CedarVideoEncoder *encoder,
                                               CedarVencInputBuffer *buffer);
typedef int (*CedarReturnOneAllocInputBuffer)(CedarVideoEncoder *encoder,
                                              CedarVencInputBuffer *buffer);
typedef int (*CedarReleaseAllocInputBuffer)(CedarVideoEncoder *encoder);
typedef int (*CedarAddOneInputBuffer)(CedarVideoEncoder *encoder,
                                      CedarVencInputBuffer *buffer);
typedef int (*CedarVideoEncodeOneFrame)(CedarVideoEncoder *encoder);
typedef int (*CedarAlreadyUsedInputBuffer)(CedarVideoEncoder *encoder,
                                           CedarVencInputBuffer *buffer);
typedef int (*CedarGetOneBitstreamFrame)(CedarVideoEncoder *encoder,
                                         CedarVencOutputBuffer *buffer);
typedef int (*CedarFreeOneBitstreamFrame)(CedarVideoEncoder *encoder,
                                          CedarVencOutputBuffer *buffer);
typedef int (*CedarVideoEncSetParameter)(CedarVideoEncoder *encoder,
                                         int index, void *value);

/* Short names used by standalone ABI probes; production names stay stable. */
typedef CedarVideoEncCreate CedarVencCreate;
typedef CedarVideoEncDestroy CedarVencDestroy;
typedef CedarVideoEncInit CedarVencInit;
typedef CedarAllocInputBuffer CedarVencAllocInputBuffer;
typedef CedarGetOneAllocInputBuffer CedarVencGetInputBuffer;
typedef CedarFlushCacheAllocInputBuffer CedarVencFlushInputBuffer;
typedef CedarReturnOneAllocInputBuffer CedarVencReturnInputBuffer;
typedef CedarAddOneInputBuffer CedarVencAddInputBuffer;
typedef CedarVideoEncodeOneFrame CedarVencEncode;
typedef CedarAlreadyUsedInputBuffer CedarVencUsedInputBuffer;
typedef CedarGetOneBitstreamFrame CedarVencGetOutputBuffer;
typedef CedarFreeOneBitstreamFrame CedarVencFreeOutputBuffer;
typedef CedarVideoEncSetParameter CedarVencSetParameter;
typedef int (*CedarVencGetParameter)(CedarVideoEncoder *encoder,
                                     int index, void *value);

#endif
