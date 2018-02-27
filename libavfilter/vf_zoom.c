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
 * Zoom in and out filter
 */

#include "../libavutil/avassert.h"
#include "../libavutil/opt.h"
#include "../libavutil/avstring.h"
#include "../libavutil/eval.h"
#include "../libavutil/pixdesc.h"
#include "./drawutils.h"
#include "./avfilter.h"
#include "./formats.h"
#include "./internal.h"
#include "./video.h"
#include "../libswscale/swscale.h"

static const char *const var_names[] = {
        "z",        /// -> last zoom
        "t",        /// -> time stamp
        NULL
};
enum var_name {
    VAR_Z,
    VAR_T,
    VAR_VARS_NB
};

typedef struct ZoomContext {
    const AVClass *class;

    FFDrawContext             dc;
    const AVPixFmtDescriptor* desc;


    double          zoom;
    int             interpolation;
    FFDrawColor     fillcolor;

    char*           zoom_expr_str;
    AVExpr*         zoom_expr;

    int     nb_planes;
    int     nb_components;
    double  var_values[VAR_VARS_NB];

    struct SwsContext* sws;
} ZoomContext;

enum {
    FAST_BILINEAR   = SWS_FAST_BILINEAR,
    BILINEAR        = SWS_BILINEAR,
    BICUBIC         = SWS_BICUBIC,
    X               = SWS_X,
    POINT           = SWS_POINT,
    AREA            = SWS_AREA,
    BICUBLIN        = SWS_BICUBLIN,
    GAUSS           = SWS_GAUSS,
    SINC            = SWS_SINC,
    LANCZOS         = SWS_LANCZOS,
    SPLINE          = SWS_SPLINE
};

#define OFFSET(x) offsetof(ZoomContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption zoom_options[] = {
    { "zoom",               "set zoom offset expression",           OFFSET(zoom_expr_str),  AV_OPT_TYPE_STRING, {.str="1"},        CHAR_MIN, CHAR_MAX, FLAGS },
    { "z",                  "set zoom offset expression",           OFFSET(zoom_expr_str),  AV_OPT_TYPE_STRING, {.str="1"},        CHAR_MIN, CHAR_MAX, FLAGS },
    { "fillcolor",          "set color for background",             OFFSET(fillcolor.rgba), AV_OPT_TYPE_COLOR,  {.str="black@0"},  CHAR_MIN, CHAR_MAX, FLAGS },

    { "interpolation",      "enable interpolation when scaling",    OFFSET(interpolation),  AV_OPT_TYPE_INT,    {.i64=FAST_BILINEAR}, SWS_FAST_BILINEAR,   SPLINE, FLAGS, "interpolation"},
      { "fast_bilinear",                                      0,                        0,  AV_OPT_TYPE_CONST,  {.i64=FAST_BILINEAR}, 0,                        0, FLAGS, "interpolation"},
      { "bilinear",                                           0,                        0,  AV_OPT_TYPE_CONST,  {.i64=BILINEAR     }, 0,                        0, FLAGS, "interpolation"},
      { "bicubic",                                            0,                        0,  AV_OPT_TYPE_CONST,  {.i64=BICUBIC      }, 0,                        0, FLAGS, "interpolation"},
      { "x",                                                  0,                        0,  AV_OPT_TYPE_CONST,  {.i64=X            }, 0,                        0, FLAGS, "interpolation"},
      { "point",                                              0,                        0,  AV_OPT_TYPE_CONST,  {.i64=POINT        }, 0,                        0, FLAGS, "interpolation"},
      { "area",                                               0,                        0,  AV_OPT_TYPE_CONST,  {.i64=AREA         }, 0,                        0, FLAGS, "interpolation"},
      { "bicublin",                                           0,                        0,  AV_OPT_TYPE_CONST,  {.i64=BICUBLIN     }, 0,                        0, FLAGS, "interpolation"},
      { "gauss",                                              0,                        0,  AV_OPT_TYPE_CONST,  {.i64=GAUSS        }, 0,                        0, FLAGS, "interpolation"},
      { "sinc",                                               0,                        0,  AV_OPT_TYPE_CONST,  {.i64=SINC         }, 0,                        0, FLAGS, "interpolation"},
      { "lanczos",                                            0,                        0,  AV_OPT_TYPE_CONST,  {.i64=LANCZOS      }, 0,                        0, FLAGS, "interpolation"},
      { "spline",                                             0,                        0,  AV_OPT_TYPE_CONST,  {.i64=SPLINE       }, 0,                        0, FLAGS, "interpolation"},

    { NULL }
};

AVFILTER_DEFINE_CLASS(zoom);

static av_cold int init(AVFilterContext *ctx)
{
//    ZoomContext *zoom = ctx->priv;
    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    return ff_set_common_formats(ctx, ff_draw_supported_pixel_formats(0));
}
static int config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ZoomContext *zoom = ctx->priv;

    zoom->desc = av_pix_fmt_desc_get(inlink->format);

    zoom->nb_planes = av_pix_fmt_count_planes(inlink->format);
    zoom->nb_components = zoom->desc->nb_components;

    ff_draw_init(&zoom->dc,  inlink->format,     FF_DRAW_PROCESS_ALPHA);
    ff_draw_color(&zoom->dc, &zoom->fillcolor,   zoom->fillcolor.rgba );

    zoom->var_values[VAR_Z] = 1;
    zoom->var_values[VAR_T] = NAN;

    int ret;
    if(
            (ret = av_expr_parse(&zoom->zoom_expr, zoom->zoom_expr_str, var_names,
                                 NULL, NULL, NULL, NULL, 0, ctx)) < 0
      )
        return AVERROR(EINVAL);

    return 0;
}
static int config_output(AVFilterLink *outlink)
{
//    AVFilterContext *ctx = outlink->src;
//    ZoomContext *zoom = ctx->priv;
    return 0;
}

static AVFrame* alloc_frame(enum AVPixelFormat pixfmt, int w, int h)
{
    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return NULL;

    frame->format = pixfmt;
    frame->width  = w;
    frame->height = h;

    if (av_frame_get_buffer(frame, 32) < 0) {
        av_frame_free(&frame);
        return NULL;
    }

    return frame;
}

static int zoom_out(ZoomContext *zoom, AVFrame *in, AVFrame *out, AVFilterLink *outlink)
{
    int ret = 0;
    zoom->sws = sws_alloc_context();
    if (!zoom->sws) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    const double zoom_val = zoom->zoom;

    const int in_w = in->width;
    const int in_h = in->height;
    const int in_f = in->format;

          int out_w = out->width * zoom_val;
          int out_h = out->height * zoom_val;
    const int out_f = outlink->format;

    if(out_w % 2 == 1) out_w--;
    if(out_h % 2 == 1) out_h--;

    if(out_h <= 0 || out_w <= 0)
        goto bypass;

    // todo there's surely a way to implement this without a temp frame
    AVFrame* temp_frame = alloc_frame(out_f, out_w, out_h);

    av_opt_set_int(zoom->sws, "srcw", in_w, 0);
    av_opt_set_int(zoom->sws, "srch", in_h, 0);
    av_opt_set_int(zoom->sws, "src_format", in_f, 0);
    av_opt_set_int(zoom->sws, "dstw", out_w, 0);
    av_opt_set_int(zoom->sws, "dsth", out_h, 0);
    av_opt_set_int(zoom->sws, "dst_format", out_f, 0);

    if(zoom->interpolation)
        av_opt_set_int(zoom->sws, "sws_flags", zoom->interpolation, 0);

    if ((ret = sws_init_context(zoom->sws, NULL, NULL)) < 0)
        goto error;

    sws_scale(zoom->sws, (const uint8_t *const *)&in->data, in->linesize, 0, in_h, temp_frame->data, temp_frame->linesize);

    sws_freeContext(zoom->sws);
    zoom->sws = NULL;

    const int dx = (in_w - out_w) / 2;
    const int dy = (in_h - out_h) / 2;

    ff_copy_rectangle2(&zoom->dc,
                       out->data, out->linesize,
                       temp_frame->data, temp_frame->linesize,
                       dx, dy, 0, 0,
                       out_w, out_h);

    av_frame_free(&temp_frame);

error:
    return ret;
bypass:
    return 0;
}

static int zoom_in (ZoomContext *zoom, AVFrame *in, AVFrame *out, AVFilterLink *outlink)
{
    int ret = 0;
    zoom->sws = sws_alloc_context();
    if (!zoom->sws) {
        ret = AVERROR(ENOMEM);
        goto error;
    }

    const double zoom_val = zoom->zoom;

          int in_w = in->width / zoom_val;
          int in_h = in->height / zoom_val;
    const int in_f = in->format;

    const int out_w = out->width;
    const int out_h = out->height;
    const int out_f = outlink->format;

    if(in_w % 2 == 1) in_w--;
    if(in_h % 2 == 1) in_h--;

    if(out_h <= 0 || out_w <= 0)
        goto bypass;

    const int dx = -(in_w - out_w) / 2;
    const int dy = -(in_h - out_h) / 2;

    int px[4], py[4];
    uint8_t *input[4];

    uint8_t chroma_w = zoom->desc->log2_chroma_w;
    uint8_t chroma_h = zoom->desc->log2_chroma_h;
    av_log(zoom, AV_LOG_DEBUG, "chroma_w: %d chroma_h: %d\n", chroma_w, chroma_h);
    av_log(zoom, AV_LOG_DEBUG, "l[0]: %d l[1]: %d l[2]: %d l[3]: %d \n", in->linesize[0], in->linesize[1],in->linesize[2],in->linesize[3]);
    av_log(zoom, AV_LOG_DEBUG, "planes: %d\n", zoom->nb_planes);
    av_log(zoom, AV_LOG_DEBUG, "components: %d\n", zoom->nb_components);

    px[1] = px[2] = AV_CEIL_RSHIFT(dx, chroma_w);
    //                    support for yuv*, rgb*, etc... (any components & planes)
    px[0] = px[3] = dx * (1.0 * zoom->nb_components / zoom->nb_planes);

    py[1] = py[2] = AV_CEIL_RSHIFT(dy, chroma_h);
    py[0] = py[3] = dy;

    for (int k = 0; in->data[k]; k++)
        input[k] = in->data[k] + py[k] * in->linesize[k] + px[k];

    av_opt_set_int(zoom->sws, "srcw", in_w, 0);
    av_opt_set_int(zoom->sws, "srch", in_h, 0);
    av_opt_set_int(zoom->sws, "src_format", in_f, 0);
    av_opt_set_int(zoom->sws, "dstw", out_w, 0);
    av_opt_set_int(zoom->sws, "dsth", out_h, 0);
    av_opt_set_int(zoom->sws, "dst_format", out_f, 0);
    if(zoom->interpolation)
        av_opt_set_int(zoom->sws, "sws_flags", zoom->interpolation, 0);

    if ((ret = sws_init_context(zoom->sws, NULL, NULL)) < 0)
        goto error;

    sws_scale(zoom->sws, (const uint8_t *const *)&input, in->linesize, 0, in_h, out->data, out->linesize);

    sws_freeContext(zoom->sws);
    zoom->sws = NULL;

    error:
    return ret;
    bypass:
    return 0;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in)
{
    AVFilterContext *ctx = inlink->dst;
    ZoomContext *zoom = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int ret;
    double zoom_val;
    AVFrame *out;

    const int in_w = in->width;
    const int in_h = in->height;

    out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);


    // eval T (time)
    zoom->var_values[VAR_T] = in->pts == AV_NOPTS_VALUE ?
                                NAN :
                                in->pts * av_q2d(inlink->time_base);
    // eval Z (zoom)
    zoom_val = zoom->zoom = zoom->var_values[VAR_Z] = av_expr_eval(zoom->zoom_expr, zoom->var_values, NULL);


    // copy in the background
    ff_fill_rectangle(&zoom->dc, &zoom->fillcolor,
                      out->data, out->linesize,
                      0, 0,
                      in_w, in_h);

    // scale
    if(zoom_val == 1) {
        // it's 1, just copy
        // quite an expensive noop :D
        ff_copy_rectangle2(&zoom->dc,
                           out->data, out->linesize,
                           in->data, in->linesize,
                           0, 0, 0, 0,
                           in_w, in_h);
    } else if (zoom_val <= 0) {
        // if it's 0 or lower do nothing
        // noop
    } else if (zoom_val < 1) {
        // zoom in (0, 1)
        ret = zoom_out(zoom, in, out, outlink);
        if(ret)
            goto error;
    } else if (zoom_val > 1){
        // zoom in (1, +ing)
        ret = zoom_in(zoom, in, out, outlink);
        if(ret)
            goto error;
    }

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);

error:
    av_frame_free(&out);
    return ret;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ZoomContext *zoom = ctx->priv;
    av_expr_free(zoom->zoom_expr);

    zoom->zoom_expr = NULL;
}

static const AVFilterPad zoom_inputs[] = {
    {
        .name         = "default",
        .type         = AVMEDIA_TYPE_VIDEO,
        .config_props = config_props,
        .filter_frame = filter_frame,
    },
    { NULL }
};

static const AVFilterPad zoom_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
        .config_props  = config_output,
    },
    { NULL }
};

AVFilter ff_vf_zoom = {
    .name          = "zoom",
    .description   = NULL_IF_CONFIG_SMALL("Zoom in and out video."),
    .priv_size     = sizeof(ZoomContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = query_formats,
    .inputs        = zoom_inputs,
    .outputs       = zoom_outputs,
    .priv_class    = &zoom_class,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
