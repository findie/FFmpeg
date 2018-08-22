/*
 * Copyright (c) 2018-2018 Stefan-Gabriel Muscalu @ Findie Dev Ltd
 *
 * This file is part of FFmpeg.
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
 * Pixel change by Stefan-Gabriel Muscalu @ Findie Dev Ltd
 */

#include "../libavutil/avassert.h"
#include "../libavutil/opt.h"
#include "../libavutil/pixdesc.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "ssim.h"

#define ABSDIFF(a,b) (abs((int)(a)-(int)(b)))
#define SUM_LEN(w) (((w) >> 2) + 3)

typedef struct SSIMChangeContext {
    const AVClass *class;

    AVFrame *frame_prev;
    unsigned int frame_nr;
    int nb_components;
    float coefs[4];
    int planewidth[4];
    int planeheight[4];
    int *temp;
    int max;

    SSIMDSPContext dsp;
} SSIMChangeContext;

enum {
    COUNT_MODE_ABSOLUTE,
    COUNT_MODE_PERCENTAGE
};

#define OFFSET(x) offsetof(SSIMChangeContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption ssimchange_options[] = {
    { NULL }
};

AVFILTER_DEFINE_CLASS(ssimchange);

static av_cold int init(AVFilterContext *ctx)
{
//    SSIMChangeContext *ssimchange = ctx->priv;

    return 0;
}

static void ssim_4x4xn_8bit(const uint8_t *main, ptrdiff_t main_stride,
                            const uint8_t *ref, ptrdiff_t ref_stride,
                            int (*sums)[4], int width)
{
    int x, y, z;

    for (z = 0; z < width; z++) {
        uint32_t s1 = 0, s2 = 0, ss = 0, s12 = 0;

        for (y = 0; y < 4; y++) {
            for (x = 0; x < 4; x++) {
                int a = main[x + y * main_stride];
                int b = ref[x + y * ref_stride];

                s1  += a;
                s2  += b;
                ss  += a*a;
                ss  += b*b;
                s12 += a*b;
            }
        }

        sums[z][0] = s1;
        sums[z][1] = s2;
        sums[z][2] = ss;
        sums[z][3] = s12;
        main += 4;
        ref += 4;
    }
}

static float ssim_end1(int s1, int s2, int ss, int s12)
{
    static const int ssim_c1 = (int)(.01*.01*255*255*64 + .5);
    static const int ssim_c2 = (int)(.03*.03*255*255*64*63 + .5);

    int fs1 = s1;
    int fs2 = s2;
    int fss = ss;
    int fs12 = s12;
    int vars = fss * 64 - fs1 * fs1 - fs2 * fs2;
    int covar = fs12 * 64 - fs1 * fs2;

    return (float)(2 * fs1 * fs2 + ssim_c1) * (float)(2 * covar + ssim_c2)
           / ((float)(fs1 * fs1 + fs2 * fs2 + ssim_c1) * (float)(vars + ssim_c2));
}

static float ssim_endn_8bit(const int (*sum0)[4], const int (*sum1)[4], int width)
{
    float ssim = 0.0;
    int i;

    for (i = 0; i < width; i++)
        ssim += ssim_end1(sum0[i][0] + sum0[i + 1][0] + sum1[i][0] + sum1[i + 1][0],
                          sum0[i][1] + sum0[i + 1][1] + sum1[i][1] + sum1[i + 1][1],
                          sum0[i][2] + sum0[i + 1][2] + sum1[i][2] + sum1[i + 1][2],
                          sum0[i][3] + sum0[i + 1][3] + sum1[i][3] + sum1[i + 1][3]);
    return ssim;
}

static float ssim_plane(SSIMDSPContext *dsp,
                        uint8_t *main, int main_stride,
                        uint8_t *ref, int ref_stride,
                        int width, int height, void *temp,
                        int max)
{
    int z = 0, y;
    float ssim = 0.0;
    int (*sum0)[4] = temp;
    int (*sum1)[4] = sum0 + SUM_LEN(width);

    width >>= 2;
    height >>= 2;

    for (y = 1; y < height; y++) {
        for (; z <= y; z++) {
            FFSWAP(void*, sum0, sum1);
            dsp->ssim_4x4_line(&main[4 * z * main_stride], main_stride,
                               &ref[4 * z * ref_stride], ref_stride,
                               sum0, width);
        }

        ssim += dsp->ssim_end_line((const int (*)[4])sum0, (const int (*)[4])sum1, width - 1);
    }

    return ssim / ((height - 1) * (width - 1));
}


static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    SSIMChangeContext *ssimchange = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    unsigned int frame_nr = ssimchange->frame_nr ++;
    unsigned int i;

    if(ssimchange->frame_prev) {

        float c[4] = {0, 0, 0, 0};
        float ssimv = 0.0;

        for(i = 0; i < ssimchange->nb_components; i++){
            c[i] = ssim_plane(
                &ssimchange->dsp,
                in->data[i], in->linesize[i],
                ssimchange->frame_prev->data[i], ssimchange->frame_prev->linesize[i],
                ssimchange->planewidth[i], ssimchange->planeheight[i],
                ssimchange->temp, ssimchange->max
            );
            ssimv += ssimchange->coefs[i] * c[i];
        }

        av_log(ssimchange, AV_LOG_INFO, "frame: %d ssim: %f c: %f %f %f %f\n",
               frame_nr,
               ssimv,
               c[0], c[1], c[2], c[3]
        );

        av_frame_free(&ssimchange->frame_prev);
    }

    ssimchange->frame_prev = av_frame_clone(in);
	  if (!ssimchange->frame_prev)
	  	return AVERROR(ENOMEM);

    return ff_filter_frame(outlink, in);
}


static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUV420P, AV_PIX_FMT_YUV422P, AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV440P, AV_PIX_FMT_YUV411P, AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_props(AVFilterLink *inlink)
{
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);
    AVFilterContext *ctx  = inlink->dst;
    SSIMChangeContext *s = ctx->priv;

    s->frame_nr = 0;
    s->nb_components = desc->nb_components;

    int sum = 0, i;

    s->planeheight[1] = s->planeheight[2] = AV_CEIL_RSHIFT(inlink->h, desc->log2_chroma_h);
    s->planeheight[0] = s->planeheight[3] = inlink->h;
    s->planewidth[1]  = s->planewidth[2]  = AV_CEIL_RSHIFT(inlink->w, desc->log2_chroma_w);
    s->planewidth[0]  = s->planewidth[3]  = inlink->w;


    for (i = 0; i < s->nb_components; i++)
        sum += s->planeheight[i] * s->planewidth[i];
    for (i = 0; i < s->nb_components; i++)
        s->coefs[i] = (double) s->planeheight[i] * s->planewidth[i] / sum;


    s->temp = av_mallocz_array(2 * SUM_LEN(inlink->w), (desc->comp[0].depth > 8) ? sizeof(int64_t[4]) : sizeof(int[4]));
    if (!s->temp)
        return AVERROR(ENOMEM);
    s->max = (1 << desc->comp[0].depth) - 1;

    s->dsp.ssim_4x4_line = ssim_4x4xn_8bit;
    s->dsp.ssim_end_line = ssim_endn_8bit;
    if (ARCH_X86)
        ff_ssim_init_x86(&s->dsp);
    return 0;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    SSIMChangeContext *ssimchange = ctx->priv;

    av_frame_free(&ssimchange->frame_prev);
    av_freep(&ssimchange->temp);
}

static const AVFilterPad ssimchange_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad ssimchange_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_ssimchange = {
    .name          = "ssimchange",
    .description   = NULL_IF_CONFIG_SMALL("SSIM changes."),
    .priv_size     = sizeof(SSIMChangeContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = ssimchange_inputs,
    .outputs       = ssimchange_outputs,
    .priv_class    = &ssimchange_class,
    .flags         = AVFILTER_FLAG_SLICE_THREADS,
};
