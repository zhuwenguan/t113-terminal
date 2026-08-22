#ifndef T113_CEDAR_VENCODER_ABI_H
#define T113_CEDAR_VENCODER_ABI_H

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
    CEDAR_VENC_PIXEL_YVU420P = 3
} CedarVencPixelFormat;

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

#endif
