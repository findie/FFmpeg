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

typedef struct ThreadData {
    uint8_t* template;
    int template_w, template_h, template_linesize;

    uint8_t* dest;
    uint8_t* src;
    int dest_w, dest_h, dest_linesize;

    uint8_t channel;
} ThreadData;

typedef struct TintContext {
    const AVClass *class;
    int nb_planes;

    float strength;
    FFDrawColor from;
    FFDrawColor to;

    struct YUV _from;
    struct YUV _to;

    struct plane_info planes[3];

    uint8_t lut_tint_with_strength[3][256];

    uint8_t lut_image_with_strength[256];
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

    // create LUT for tinting
    float lerp_range_y = tint->_to.Y - tint->_from.Y;
    float lerp_range_u = tint->_to.U - tint->_from.U;
    float lerp_range_v = tint->_to.V - tint->_from.V;

    for (int i = 0; i < 256; i++) {
        tint->lut_tint_with_strength[0][i] = (i / 255.0f * lerp_range_y + tint->_from.Y) * tint->strength;
        tint->lut_tint_with_strength[1][i] = (i / 255.0f * lerp_range_u + tint->_from.U) * tint->strength;
        tint->lut_tint_with_strength[2][i] = (i / 255.0f * lerp_range_v + tint->_from.V) * tint->strength;

        tint->lut_image_with_strength[i] = i * (1.0f - tint->strength);
    }

    return 0;
}

static int tint_plane_slice(AVFilterContext *ctx, void *arg, int jobnr, int nb_jobs) {
  TintContext *s = ctx->priv;
  ThreadData *td = arg;

  uint8_t *template = td->template;
  int template_w = td->template_w;
  int template_h = td->template_h;
  int template_linesize = td->template_linesize;

  uint8_t *dest = td->dest;
  uint8_t *src  = td->src;
  int dest_w = td->dest_w;
  int dest_h = td->dest_h;
  int dest_linesize = td->dest_linesize;

  uint8_t *lut_tint_with_strength = s->lut_tint_with_strength[td->channel];
  uint8_t *lut_image_with_strength = s->lut_image_with_strength;

  const int slice_start = (dest_h *  jobnr   ) / nb_jobs;
  const int slice_end   = (dest_h * (jobnr+1)) / nb_jobs;

  uint8_t w_mult = template_w / dest_w;
  uint8_t h_mult = template_h / dest_h;

  unsigned int dest_stride = dest_linesize;
  unsigned int template_stride = template_linesize;

  uint8_t *dest_row = dest + slice_start * dest_stride;
  uint8_t *src_row  = src  + slice_start * dest_stride;
  uint8_t *template_row = template + (slice_start * h_mult) * template_stride;

  for (unsigned int y = slice_start; y < slice_end; y++) {
    for (unsigned int x = 0; x < dest_w; x++) {
      uint8_t template_value = template_row[x * w_mult];

      dest_row[x] =
          lut_image_with_strength[src_row[x]] +
          lut_tint_with_strength[template_value];
    }

    dest_row += dest_stride;
    src_row  += dest_stride;
    template_row += template_stride * h_mult;
  }

  return 0;
}


static void tint_plane(uint8_t *template, int template_w, int template_h, int template_linesize,
                       uint8_t *src,
                       uint8_t *dest, int dest_w, int dest_h, int dest_linesize,
                       uint8_t *lut_tint_with_strength,
                       uint8_t *lut_image_with_strength) {

  uint8_t w_mult = template_w / dest_w;
  uint8_t h_mult = template_h / dest_h;

  unsigned int dest_stride = dest_linesize;
  unsigned int template_stride = template_linesize;

  for (unsigned int y = 0; y < dest_h; y++) {
    for (unsigned int x = 0; x < dest_w; x++) {
      uint8_t template_value = template[(y * h_mult) * template_stride + x * w_mult];

      dest[y * dest_stride + x] =
          lut_image_with_strength[src[y * dest_linesize + x]] +
          lut_tint_with_strength[template_value];
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

    if(tint->strength == 0){
        return ff_filter_frame(outlink, in);
    }

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
      ThreadData td;

      // common data
      td.template = template_luma;
      td.template_w = frame_w;
      td.template_h = frame_h;
      td.template_linesize = template_linesize;

      // color components
      td.src = in->data[1];
      td.dest = out->data[1];
      td.dest_w = tint->planes[1].width;
      td.dest_h = tint->planes[1].height;
      td.dest_linesize = out->linesize[1];
      td.channel = 1;

//      tint_plane(td.template, td.template_w, td.template_h, td.template_linesize,
//                 td.src,
//                 td.dest, td.dest_w, td.dest_h, td.dest_linesize,
//                 tint->lut_tint_with_strength[td.channel],
//                 tint->lut_image_with_strength);
      ctx->internal->execute(ctx, tint_plane_slice, &td, NULL, FFMIN(in->height, ff_filter_get_nb_threads(ctx)));
      emms_c();

      td.src = in->data[2];
      td.dest = out->data[2];
      td.dest_w = tint->planes[2].width;
      td.dest_h = tint->planes[2].height;
      td.dest_linesize = out->linesize[2];
      td.channel = 2;

//      tint_plane(td.template, td.template_w, td.template_h, td.template_linesize,
//                 td.src,
//                 td.dest, td.dest_w, td.dest_h, td.dest_linesize,
//                 tint->lut_tint_with_strength[td.channel],
//                 tint->lut_image_with_strength);
      ctx->internal->execute(ctx, tint_plane_slice, &td, NULL, FFMIN(in->height, ff_filter_get_nb_threads(ctx)));
      emms_c();

      // last is luma (which may also be the template)
      td.src = in->data[0];
      td.dest = out->data[0];
      td.dest_w = tint->planes[0].width;
      td.dest_h = tint->planes[0].height;
      td.dest_linesize = out->linesize[0];
      td.channel = 0;

//      tint_plane(td.template, td.template_w, td.template_h, td.template_linesize,
//                 td.src,
//                 td.dest, td.dest_w, td.dest_h, td.dest_linesize,
//                 tint->lut_tint_with_strength[td.channel],
//                 tint->lut_image_with_strength);
      ctx->internal->execute(ctx, tint_plane_slice, &td, NULL, FFMIN(in->height, ff_filter_get_nb_threads(ctx)));
      emms_c();
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
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC | AVFILTER_FLAG_SLICE_THREADS,
};
