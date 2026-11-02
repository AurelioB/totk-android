/* WebP as Pillow 12.3 writes and reads it (src/_webp.c): WebPEncode with the given quality / method / exact on an ARGB
   picture, wrapped by WebPMuxAssemble (VP8X when there is alpha); reading goes through WebPAnimDecoder (RGBA, or RGB when
   the file has no alpha), as Image.open() does. libwebp 1.6.0 = the library Pillow 12.3.0 bundles. */
#include "cg.h"
#include <stdlib.h>
#include <string.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

int cg_webp_encode(const CgImg *im, int quality, int method, uint8_t **out, size_t *n) {
    *out = NULL; *n = 0;
    WebPConfig config; WebPPicture pic; WebPMemoryWriter writer;
    if (im->mode != CG_RGBA && im->mode != CG_RGB) return -1;
    if (!WebPConfigInit(&config)) return -1;
    config.lossless = 0;
    config.quality = (float)quality;
    config.alpha_quality = 100;
    config.method = method;
    config.exact = 1;
    if (!WebPValidateConfig(&config) || !WebPPictureInit(&pic)) return -1;
    pic.width = im->w; pic.height = im->h; pic.use_argb = 1;
    if (!WebPPictureAlloc(&pic)) return -1;
    for (int y = 0; y < im->h; y++) {
        const uint8_t *src = cg_px(im, 0, y);
        uint32_t *dst = pic.argb + (size_t)pic.argb_stride * y;
        for (int x = 0; x < im->w; x++, src += 4)
            dst[x] = (uint32_t)src[2] | (uint32_t)src[1] << 8 | (uint32_t)src[0] << 16 | (im->mode == CG_RGBA ? (uint32_t)src[3] : 0xffu) << 24;
    }
    WebPMemoryWriterInit(&writer);
    pic.writer = WebPMemoryWrite; pic.custom_ptr = &writer;
    int ok = WebPEncode(&config, &pic);
    WebPPictureFree(&pic);
    if (!ok) { WebPMemoryWriterClear(&writer); return -1; }
    WebPMux *mux = WebPMuxNew();
    WebPData image = { writer.mem, writer.size }, assembled = { NULL, 0 };
    int rc = -1;
    if (mux && WebPMuxSetImage(mux, &image, 0) == WEBP_MUX_OK && WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK && assembled.size) {
        if ((*out = malloc(assembled.size))) { memcpy(*out, assembled.bytes, assembled.size); *n = assembled.size; rc = 0; }
    }
    WebPDataClear(&assembled);
    WebPMuxDelete(mux);
    WebPMemoryWriterClear(&writer);
    return rc;
}

CgImg *cg_webp_decode(const uint8_t *data, size_t n) {
    WebPBitstreamFeatures features; WebPAnimInfo info; WebPData src = { data, n };
    int alpha = 1;
    if (WebPGetFeatures(data, n, &features) == VP8_STATUS_OK) alpha = features.has_alpha;
    WebPAnimDecoder *dec = WebPAnimDecoderNew(&src, NULL);   /* default options: MODE_RGBA, no threads */
    CgImg *im = NULL; uint8_t *buf; int ts;
    if (dec && WebPAnimDecoderGetInfo(dec, &info) && WebPAnimDecoderGetNext(dec, &buf, &ts) &&
        (im = cg_new(alpha ? CG_RGBA : CG_RGB, (int)info.canvas_width, (int)info.canvas_height))) {
        memcpy(im->p, buf, (size_t)info.canvas_width * info.canvas_height * 4);
        if (!alpha) for (size_t i = 0; i < (size_t)im->w * im->h; i++) im->p[i * 4 + 3] = 255;   /* rawmode RGBX */
    }
    WebPAnimDecoderDelete(dec);
    return im;
}
