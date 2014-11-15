/*
 * This file is part of FFmpeg.
 *
 * Copyright (C) 2011, 2012 Hyllian/Jararaca - sergiogdb@gmail.com
 *
 * Copyright (c) 2014 Arwa Arif <arwaarif1994@gmail.com>
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * XBR Filter is used for depixelization of image.
 * This is based on Hyllian's xBR shader.
 *
 * @see http://www.libretro.com/forums/viewtopic.php?f=6&t=134
 * @see https://github.com/yoyofr/iFBA/blob/master/fba_src/src/intf/video/scalers/xbr.cpp
 *
 * @todo add threading and FATE test
 */

#include "libavutil/opt.h"
#include "libavutil/avassert.h"
#include "libavutil/pixdesc.h"
#include "internal.h"

#define RGB_MASK      0x00FFFFFF
#define LB_MASK       0x00FEFEFE
#define RED_BLUE_MASK 0x00FF00FF
#define GREEN_MASK    0x0000FF00

typedef struct {
    const AVClass *class;
    int n;
    uint32_t rgbtoyuv[1<<24];
} XBRContext;

#define OFFSET(x) offsetof(XBRContext, x)
static const AVOption xbr_options[] = {
    { "n", "set scale factor", OFFSET(n), AV_OPT_TYPE_INT, {.i64 = 3}, 2, 4, },
    { NULL }
};

AVFILTER_DEFINE_CLASS(xbr);

static uint32_t pixel_diff(uint32_t x, uint32_t y, const uint32_t *r2y)
{
#define YMASK 0xff0000
#define UMASK 0x00ff00
#define VMASK 0x0000ff

    uint32_t yuv1 = r2y[x & 0xffffff];
    uint32_t yuv2 = r2y[y & 0xffffff];

    return (abs((yuv1 & YMASK) - (yuv2 & YMASK)) >> 16) +
           (abs((yuv1 & UMASK) - (yuv2 & UMASK)) >>  8) +
           abs((yuv1 & VMASK) - (yuv2 & VMASK));
}

#define ALPHA_BLEND_128_W(dst, src) dst = ((src & LB_MASK) >> 1) + ((dst & LB_MASK) >> 1)

#define ALPHA_BLEND_32_W(dst, src) \
    dst = ((RED_BLUE_MASK & ((dst & RED_BLUE_MASK) + ((((src & RED_BLUE_MASK) - \
          (dst & RED_BLUE_MASK))) >>3))) | (GREEN_MASK & ((dst & GREEN_MASK) + \
          ((((src & GREEN_MASK) - (dst & GREEN_MASK))) >>3))))

#define ALPHA_BLEND_64_W(dst, src) \
    dst = ((RED_BLUE_MASK & ((dst & RED_BLUE_MASK) + ((((src & RED_BLUE_MASK) - \
          (dst & RED_BLUE_MASK))) >>2))) | (GREEN_MASK & ((dst & GREEN_MASK) + \
          ((((src & GREEN_MASK) - (dst & GREEN_MASK))) >>2))))

#define ALPHA_BLEND_192_W(dst, src) \
    dst = ((RED_BLUE_MASK & ((dst & RED_BLUE_MASK) + ((((src & RED_BLUE_MASK) - \
          (dst & RED_BLUE_MASK)) * 3) >>2))) | (GREEN_MASK & ((dst & GREEN_MASK) + \
          ((((src & GREEN_MASK) - (dst & GREEN_MASK)) * 3) >>2))))

#define ALPHA_BLEND_224_W(dst, src) \
    dst = ((RED_BLUE_MASK & ((dst & RED_BLUE_MASK) + ((((src & RED_BLUE_MASK) - \
          (dst & RED_BLUE_MASK)) * 7) >>3))) | (GREEN_MASK & ((dst & GREEN_MASK) + \
          ((((src & GREEN_MASK) - (dst & GREEN_MASK)) * 7) >>3))))

#define df(A, B) pixel_diff(A, B, r2y)

#define eq(A, B)\
    (df(A, B) < 155)\

#define INIT_SRC_DST_POINTERS(level)                                                                    \
    uint32_t *E = (uint32_t *)(output->data[0] + y * output->linesize[0] * (level));                    \
    const uint32_t *sa2 = (uint32_t *)(input->data[0] + y * input->linesize[0] - 8); /* center */       \
    const uint32_t *sa1 = sa2 - (input->linesize[0]>>2); /* up x1 */                                    \
    const uint32_t *sa0 = sa1 - (input->linesize[0]>>2); /* up x2 */                                    \
    const uint32_t *sa3 = sa2 + (input->linesize[0]>>2); /* down x1 */                                  \
    const uint32_t *sa4 = sa3 + (input->linesize[0]>>2); /* down x2 */                                  \
                                                                                                        \
    if (y <= 1) {                                                                                       \
        sa0 = sa1;                                                                                      \
        if (y == 0) {                                                                                   \
            sa0 = sa1 = sa2;                                                                            \
        }                                                                                               \
    }                                                                                                   \
                                                                                                        \
    if (y >= input->height - 2) {                                                                       \
        sa4 = sa3;                                                                                      \
        if (y == input->height - 1) {                                                                   \
            sa4 = sa3 = sa2;                                                                            \
        }                                                                                               \
    }

#define INIT_21_PIXELS                                      \
    const uint32_t B1 = sa0[2];                             \
    const uint32_t PB = sa1[2];                             \
    const uint32_t PE = sa2[2];                             \
    const uint32_t PH = sa3[2];                             \
    const uint32_t H5 = sa4[2];                             \
                                                            \
    const int pprev = 2 - (x > 0);                          \
    const uint32_t A1 = sa0[pprev];                         \
    const uint32_t PA = sa1[pprev];                         \
    const uint32_t PD = sa2[pprev];                         \
    const uint32_t PG = sa3[pprev];                         \
    const uint32_t G5 = sa4[pprev];                         \
                                                            \
    const int pprev2 = pprev - (x > 1);                     \
    const uint32_t A0 = sa1[pprev2];                        \
    const uint32_t D0 = sa2[pprev2];                        \
    const uint32_t G0 = sa3[pprev2];                        \
                                                            \
    const int pnext = 3 - (x == input->width - 1);          \
    const uint32_t C1 = sa0[pnext];                         \
    const uint32_t PC = sa1[pnext];                         \
    const uint32_t PF = sa2[pnext];                         \
    const uint32_t PI = sa3[pnext];                         \
    const uint32_t I5 = sa4[pnext];                         \
                                                            \
    const int pnext2 = pnext + 1 - (x >= input->width - 2); \
    const uint32_t C4 = sa1[pnext2];                        \
    const uint32_t F4 = sa2[pnext2];                        \
    const uint32_t I4 = sa3[pnext2];

#define FILT2(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1,   \
              N0, N1, N2, N3) do {                                                                  \
    if (PE != PH && PE != PF) {                                                                     \
        const unsigned e = df(PE,PC) + df(PE,PG) + df(PI,H5) + df(PI,F4) + (df(PH,PF)<<2);          \
        const unsigned i = df(PH,PD) + df(PH,I5) + df(PF,I4) + df(PF,PB) + (df(PE,PI)<<2);          \
        if (e < i && (!eq(PF,PB) && !eq(PH,PD) || eq(PE,PI)                                         \
                      && (!eq(PF,I4) && !eq(PH,I5))                                                 \
                      || eq(PE,PG) || eq(PE,PC))) {                                                 \
            const unsigned ke  = df(PF,PG);                                                         \
            const unsigned ki  = df(PH,PC);                                                         \
            const int left = ke<<1 <= ki && PE != PG && PD != PG;                                   \
            const int up   = ke >= ki<<1 && PE != PC && PB != PC;                                   \
            const unsigned px  = df(PE,PF) <= df(PE,PH) ? PF : PH;                                  \
            if (left && up) {                                                                       \
                ALPHA_BLEND_224_W(E[N3], px);                                                       \
                ALPHA_BLEND_64_W( E[N2], px);                                                       \
                E[N1] = E[N2];                                                                      \
            } else if (left) {                                                                      \
                ALPHA_BLEND_192_W(E[N3], px);                                                       \
                ALPHA_BLEND_64_W( E[N2], px);                                                       \
            } else if (up) {                                                                        \
                ALPHA_BLEND_192_W(E[N3], px);                                                       \
                ALPHA_BLEND_64_W( E[N1], px);                                                       \
            } else { /* diagonal */                                                                 \
                ALPHA_BLEND_128_W(E[N3], px);                                                       \
            }                                                                                       \
        } else if (e <= i) {                                                                        \
            ALPHA_BLEND_128_W( E[N3], ((df(PE,PF) <= df(PE,PH)) ? PF : PH));                        \
        }                                                                                           \
    }                                                                                               \
} while (0)

static void xbr2x(AVFrame * input, AVFrame * output, const uint32_t * r2y)
{
    int x,y;
    int next_line = output->linesize[0]>>2;

    for (y = 0; y < input->height; y++) {
        INIT_SRC_DST_POINTERS(2)

        for (x = 0; x < input->width; x++) {
            INIT_21_PIXELS

            E[0] = E[1] = E[next_line] = E[next_line + 1] = PE; // 0, 1, 2, 3

            FILT2(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, 0, 1, next_line, next_line+1);
            FILT2(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0, next_line, 0, next_line+1, 1);
            FILT2(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I4, I5, next_line+1, next_line, 1, 0);
            FILT2(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C1, C4, 1, next_line+1, 0, next_line);

            sa0 += 1;
            sa1 += 1;
            sa2 += 1;
            sa3 += 1;
            sa4 += 1;

            E += 2;
        }
    }
}

#define FILT3(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1,   \
              N0, N1, N2, N3, N4, N5, N6, N7, N8) do {                                              \
    if (PE != PH && PE != PF) {                                                                     \
        const unsigned e = df(PE,PC) + df(PE,PG) + df(PI,H5) + df(PI,F4) + (df(PH,PF)<<2);          \
        const unsigned i = df(PH,PD) + df(PH,I5) + df(PF,I4) + df(PF,PB) + (df(PE,PI)<<2);          \
        if (e < i && (!eq(PF,PB) && !eq(PF,PC) || !eq(PH,PD) && !eq(PH,PG) || eq(PE,PI)             \
                      && (!eq(PF,F4) && !eq(PF,I4) || !eq(PH,H5) && !eq(PH,I5))                     \
                      || eq(PE,PG) || eq(PE,PC))) {                                                 \
            const unsigned ke  = df(PF,PG);                                                         \
            const unsigned ki  = df(PH,PC);                                                         \
            const int left = ke<<1 <= ki && PE != PG && PD != PG;                                   \
            const int up   = ke >= ki<<1 && PE != PC && PB != PC;                                   \
            const unsigned px  = df(PE,PF) <= df(PE,PH) ? PF : PH;                                  \
            if (left && up) {                                                                       \
                ALPHA_BLEND_192_W(E[N7], px);                                                       \
                ALPHA_BLEND_64_W( E[N6], px);                                                       \
                E[N5] = E[N7];                                                                      \
                E[N2] = E[N6];                                                                      \
                E[N8] = px;                                                                         \
            } else if (left) {                                                                      \
                ALPHA_BLEND_192_W(E[N7], px);                                                       \
                ALPHA_BLEND_64_W( E[N5], px);                                                       \
                ALPHA_BLEND_64_W( E[N6], px);                                                       \
                E[N8] = px;                                                                         \
            } else if (up) {                                                                        \
                ALPHA_BLEND_192_W(E[N5], px);                                                       \
                ALPHA_BLEND_64_W( E[N7], px);                                                       \
                ALPHA_BLEND_64_W( E[N2], px);                                                       \
                E[N8] = px;                                                                         \
            } else { /* diagonal */                                                                 \
                ALPHA_BLEND_224_W(E[N8], px);                                                       \
                ALPHA_BLEND_32_W( E[N5], px);                                                       \
                ALPHA_BLEND_32_W( E[N7], px);                                                       \
            }                                                                                       \
        } else if (e <= i) {                                                                        \
            ALPHA_BLEND_128_W(E[N8], ((df(PE,PF) <= df(PE,PH)) ? PF : PH));                         \
        }                                                                                           \
    }                                                                                               \
} while (0)

static void xbr3x(AVFrame *input, AVFrame *output, const uint32_t *r2y)
{
    const int nl = output->linesize[0]>>2;
    const int nl1 = nl + nl;
    int x,y;

    for (y = 0; y < input->height; y++) {
        INIT_SRC_DST_POINTERS(3)

        for (x = 0; x < input->width; x++){
            INIT_21_PIXELS

            E[0]   = E[1]     = E[2]     = PE;
            E[nl]  = E[nl+1]  = E[nl+2]  = PE; // 3, 4, 5
            E[nl1] = E[nl1+1] = E[nl1+2] = PE; // 6, 7, 8

            FILT3(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, 0, 1, 2, nl, nl+1, nl+2, nl1, nl1+1, nl1+2);
            FILT3(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0, nl1, nl, 0, nl1+1, nl+1, 1, nl1+2, nl+2, 2);
            FILT3(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I4, I5, nl1+2, nl1+1, nl1, nl+2, nl+1, nl, 2, 1, 0);
            FILT3(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C1, C4, 2, nl+2, nl1+2, 1, nl+1, nl1+1, 0, nl, nl1);

            sa0 += 1;
            sa1 += 1;
            sa2 += 1;
            sa3 += 1;
            sa4 += 1;

            E += 3;
        }
    }
}

#define FILT4(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1,   \
              N15, N14, N11, N3, N7, N10, N13, N12, N9, N6, N2, N1, N5, N8, N4, N0) do {            \
    if (PE != PH && PE != PF) {                                                                     \
        const unsigned e = df(PE,PC) + df(PE,PG) + df(PI,H5) + df(PI,F4) + (df(PH,PF)<<2);          \
        const unsigned i = df(PH,PD) + df(PH,I5) + df(PF,I4) + df(PF,PB) + (df(PE,PI)<<2);          \
        if (e < i && (!eq(PF,PB) && !eq(PH,PD) || eq(PE,PI)                                         \
                      && (!eq(PF,I4) && !eq(PH,I5))                                                 \
                      || eq(PE,PG) || eq(PE,PC))) {                                                 \
            const unsigned ke  = df(PF,PG);                                                         \
            const unsigned ki  = df(PH,PC);                                                         \
            const int left = ke<<1 <= ki && PE != PG && PD != PG;                                   \
            const int up   = ke >= ki<<1 && PE != PC && PB != PC;                                   \
            const unsigned px  = df(PE,PF) <= df(PE,PH) ? PF : PH;                                  \
            if (left && up) {                                                                       \
                ALPHA_BLEND_192_W(E[N13], px);                                                      \
                ALPHA_BLEND_64_W( E[N12], px);                                                      \
                E[N15] = E[N14] = E[N11] = px;                                                      \
                E[N10] = E[N3]  = E[N12];                                                           \
                E[N7]  = E[N13];                                                                    \
            } else if (left) {                                                                      \
                ALPHA_BLEND_192_W(E[N11], px);                                                      \
                ALPHA_BLEND_192_W(E[N13], px);                                                      \
                ALPHA_BLEND_64_W( E[N10], px);                                                      \
                ALPHA_BLEND_64_W( E[N12], px);                                                      \
                E[N14] = px;                                                                        \
                E[N15] = px;                                                                        \
            } else if (up) {                                                                        \
                ALPHA_BLEND_192_W(E[N14], px);                                                      \
                ALPHA_BLEND_192_W(E[N7 ], px);                                                      \
                ALPHA_BLEND_64_W( E[N10], px);                                                      \
                ALPHA_BLEND_64_W( E[N3 ], px);                                                      \
                E[N11] = px;                                                                        \
                E[N15] = px;                                                                        \
            } else { /* diagonal */                                                                 \
                ALPHA_BLEND_128_W(E[N11], px);                                                      \
                ALPHA_BLEND_128_W(E[N14], px);                                                      \
                E[N15] = px;                                                                        \
            }                                                                                       \
        } else if (e <= i) {                                                                        \
            ALPHA_BLEND_128_W( E[N15], ((df(PE,PF) <= df(PE,PH)) ? PF : PH));                       \
        }                                                                                           \
    }                                                                                               \
} while (0)

static void xbr4x(AVFrame *input, AVFrame *output, const uint32_t *r2y)
{

    const int nl = output->linesize[0]>>2;
    const int nl1 = nl + nl;
    const int nl2 = nl1 + nl;
    int x, y;

    for (y = 0; y < input->height; y++) {
        INIT_SRC_DST_POINTERS(4)

        for (x = 0; x < input->width; x++) {
            INIT_21_PIXELS

            E[0]   = E[1]     = E[2]     = E[3]     = PE;
            E[nl]  = E[nl+1]  = E[nl+2]  = E[nl+3]  = PE; //  4,  5,  6,  7
            E[nl1] = E[nl1+1] = E[nl1+2] = E[nl1+3] = PE; //  8,  9, 10, 11
            E[nl2] = E[nl2+1] = E[nl2+2] = E[nl2+3] = PE; // 12, 13, 14, 15

            FILT4(PE, PI, PH, PF, PG, PC, PD, PB, PA, G5, C4, G0, D0, C1, B1, F4, I4, H5, I5, A0, A1, nl2+3, nl2+2, nl1+3,  3,  nl+3, nl1+2, nl2+1, nl2,  nl1+1,  nl+2, 2,  1, nl+1, nl1, nl, 0);
            FILT4(PE, PC, PF, PB, PI, PA, PH, PD, PG, I4, A1, I5, H5, A0, D0, B1, C1, F4, C4, G5, G0,  3,  nl+3,  2,  0,  1,  nl+2, nl1+3, nl2+3, nl1+2,  nl+1, nl,  nl1, nl1+1,nl2+2,nl2+1,nl2);
            FILT4(PE, PA, PB, PD, PC, PG, PF, PH, PI, C1, G0, C4, F4, G5, H5, D0, A0, B1, A1, I4, I5,  0,  1,  nl, nl2,  nl1,  nl+1,  2,  3,  nl+2,  nl1+1, nl2+1,nl2+2,nl1+2, nl+3,nl1+3,nl2+3);
            FILT4(PE, PG, PD, PH, PA, PI, PB, PF, PC, A0, I5, A1, B1, I4, F4, H5, G5, D0, G0, C1, C4, nl2,  nl1, nl2+1, nl2+3, nl2+2,  nl1+1,  nl,  0,  nl+1, nl1+2, nl1+3, nl+3, nl+2, 1, 2, 3);

            sa0 += 1;
            sa1 += 1;
            sa2 += 1;
            sa3 += 1;
            sa4 += 1;

            E += 4;
        }
    }
}

static int config_output(AVFilterLink *outlink)
{
    AVFilterContext *ctx = outlink->src;
    XBRContext *xbr = ctx->priv;
    AVFilterLink *inlink = ctx->inputs[0];

    outlink->w = inlink->w * xbr->n;
    outlink->h = inlink->h * xbr->n;
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_0RGB32, AV_PIX_FMT_NONE,
    };

    ff_set_common_formats(ctx, ff_make_format_list(pix_fmts));
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    AVFilterLink *outlink = ctx->outputs[0];
    XBRContext *xbr = ctx->priv;
    const uint32_t *r2y = xbr->rgbtoyuv;

    AVFrame *out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    av_frame_copy_props(out, in);
    if (xbr->n == 4)
        xbr4x(in, out, r2y);
    else if (xbr->n == 3)
        xbr3x(in, out, r2y);
    else
        xbr2x(in, out, r2y);

    out->width  = outlink->w;
    out->height = outlink->h;

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static int init(AVFilterContext *ctx)
{
    XBRContext *xbr = ctx->priv;
    uint32_t c;
    int bg, rg, g;

    for (bg = -255; bg < 256; bg++) {
        for (rg = -255; rg < 256; rg++) {
            const uint32_t u = (uint32_t)((-169*rg + 500*bg)/1000) + 128;
            const uint32_t v = (uint32_t)(( 500*rg -  81*bg)/1000) + 128;
            int startg = FFMAX3(-bg, -rg, 0);
            int endg = FFMIN3(255-bg, 255-rg, 255);
            uint32_t y = (uint32_t)(( 299*rg + 1000*startg + 114*bg)/1000);
            c = bg + (rg<<16) + 0x010101 * startg;
            for (g = startg; g <= endg; g++) {
                xbr->rgbtoyuv[c] = ((y++) << 16) + (u << 8) + v;
                c+= 0x010101;
            }
        }
    }

    return 0;
}

static const AVFilterPad xbr_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad xbr_outputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_output,
    },
    { NULL }
};

AVFilter ff_vf_xbr = {
    .name          = "xbr",
    .description   = NULL_IF_CONFIG_SMALL("Scale the input using xBR algorithm."),
    .inputs        = xbr_inputs,
    .outputs       = xbr_outputs,
    .query_formats = query_formats,
    .priv_size     = sizeof(XBRContext),
    .priv_class    = &xbr_class,
    .init          = init,
};
