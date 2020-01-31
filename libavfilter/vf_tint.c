/*
 * Copyright (c) 2012-2014 Clément Bœsch <u pkh me>
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
 * Tinting by Stefan-Gabriel Muscalu @ Findie Dev Ltd
 */

#include "libavutil/avassert.h"
#include "libavutil/imgutils.h"
#include "libavutil/opt.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include "drawutils.h"


struct plane_info {
    int width, height;
};

struct RGB {
    uint8_t R, G, B;
};
struct YUV {
    uint8_t Y, U, V;
};

typedef struct TintContext {
    const AVClass *class;
    int nb_planes;

    float strength;
    FFDrawColor from;
    FFDrawColor to;

    struct YUV _from;
    struct YUV _to;

    struct plane_info planes[3];

} TintContext;

#define OFFSET(x) offsetof(TintContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption tint_options[] = {
    { "strength", "set strength of the effect",     OFFSET(strength),  AV_OPT_TYPE_FLOAT, {.dbl=.5}, 0, 1, FLAGS},
    { "from",     "start color used on low values", OFFSET(from.rgba), AV_OPT_TYPE_COLOR, {.str="red"}, CHAR_MIN, CHAR_MAX, FLAGS},
    { "to",       "end color used on high values",  OFFSET(to.rgba),   AV_OPT_TYPE_COLOR, {.str="cyan"}, CHAR_MIN, CHAR_MAX, FLAGS},
    { NULL }
};

AVFILTER_DEFINE_CLASS(tint);

static av_cold int init(AVFilterContext *ctx)
{
    TintContext *tint = ctx->priv;

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_YUV440P,
        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
      return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}


static struct YUV rgb2yuv(uint8_t rgba[4]) {
  struct YUV out;
  out.Y = floor(rgba[0] * .299000 + rgba[1] * .587000 + rgba[2] * .114000);
  out.U = floor(rgba[0] * -.168736 + rgba[1] * -.331264 + rgba[2] * .500000 + 128);
  out.V = floor(rgba[0] * .500000 + rgba[1] * -.418688 + rgba[2] * -.081312 + 128);

  return out;
}

static int config_props(AVFilterLink *inlink)
{
    int p;
    AVFilterContext *ctx = inlink->dst;
    TintContext *tint = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    tint->_from = rgb2yuv(tint->from.rgba);
    tint->_to = rgb2yuv(tint->to.rgba);

    tint->nb_planes = 3;
    for (p = 0; p < tint->nb_planes; p++) {
        struct plane_info *plane = &tint->planes[p];
        int vsub = p ? desc->log2_chroma_h : 0;
        int hsub = p ? desc->log2_chroma_w : 0;

        plane->width      = AV_CEIL_RSHIFT(inlink->w, hsub);
        plane->height     = AV_CEIL_RSHIFT(inlink->h, vsub);
    }
    return 0;
}

static void tint_plane(uint8_t *template, int template_w, int template_h, int template_linesize,
                      uint8_t *dest, int dest_w, int dest_h, int dest_linesize,
                      uint8_t from_value, uint8_t to_value, float strength) {

  uint8_t w_mult = template_w / dest_w;
  uint8_t h_mult = template_h / dest_h;

  unsigned int dest_stride = dest_linesize;
  unsigned int template_stride = template_linesize;

  float lerp_range = to_value - from_value;

  for (unsigned int y = 0; y < dest_h; y++) {
    for (unsigned int x = 0; x < dest_w; x++) {
//      printf("X: %d (%d) Y: %d (%d) (source X: %d (%d) Y: %d (%d))\n", x, dest_w, y, dest_h, x * w_mult, template_w, y * h_mult, template_h);
      uint8_t template_value = template[(y * h_mult) * template_stride + x * w_mult];

      // floor(val / ${format_range} * ${lerp_range} + ${from})
      dest[y * dest_stride + x] =
          (dest[y * dest_stride + x] * (1 - strength)) +
          ((template_value / 255.0f * lerp_range + from_value) * strength);
    }
  }
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    TintContext *tint = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int direct = 0;
    AVFrame *out;

    if (av_frame_is_writable(in)) {
        direct = 1;
        out = in;
    } else {
        out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
        if (!out) {
            av_frame_free(&in);
            return AVERROR(ENOMEM);
        }
        av_frame_copy_props(out, in);
    }

    uint8_t* template_luma = in->data[0];
    int frame_w = tint->planes[0].width;
    int frame_h = tint->planes[0].height;
    int template_linesize = in->linesize[0];

    if(tint->strength > 0) {
      // color components
      tint_plane(template_luma, frame_w, frame_h, template_linesize,
                 out->data[1], tint->planes[1].width, tint->planes[1].height, in->linesize[1],
                 tint->_from.U, tint->_to.U, tint->strength
      );
      tint_plane(template_luma, frame_w, frame_h, template_linesize,
                 out->data[2], tint->planes[2].width, tint->planes[2].height, in->linesize[2],
                 tint->_from.V, tint->_to.V, tint->strength
      );
      // last is luma (which may also be the template)
      tint_plane(template_luma, frame_w, frame_h, template_linesize,
                 out->data[0], tint->planes[0].width, tint->planes[0].height, in->linesize[0],
                 tint->_from.Y, tint->_to.Y, tint->strength
      );
    }

    if (!direct)
        av_frame_free(&in);
    return ff_filter_frame(outlink, out);
}

static av_cold void uninit(AVFilterContext *ctx)
{
    int p;
    TintContext *tint = ctx->priv;

    for (p = 0; p < tint->nb_planes; p++) {
        struct plane_info *plane = &tint->planes[p];
    }
}

static const AVFilterPad tint_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad tint_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_tint = {
    .name          = "tint",
    .description   = NULL_IF_CONFIG_SMALL("Tint frame for a color to another color."),
    .priv_size     = sizeof(TintContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = tint_inputs,
    .outputs       = tint_outputs,
    .priv_class    = &tint_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
