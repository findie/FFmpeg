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
#include <stdio.h>
#include <string.h>

static const char *const var_names[] = {
        "z", "zoom",/// -> last zoom
        "t",        /// -> time stamp
        "x",        /// -> last x
        "y",        /// -> last y
        NULL
};
enum var_name {
    VAR_Z, VAR_ZOOM,
    VAR_T,
    VAR_X,
    VAR_Y,
    VAR_VARS_NB
};

typedef struct ZoomContext {
    const AVClass *class;

    FFDrawContext             dc;
    const AVPixFmtDescriptor* desc;

    char*           schedule_file_path;
    double*         schedule;
    unsigned long   schedule_size;
    unsigned long   schedule_index;

    double          zoom_max;
    double          zoom;
    double          x;
    double          y;
    int             interpolation;
    FFDrawColor     fillcolor;

    double          outAspectRatio;

    char*           zoom_expr_str;
    AVExpr*         zoom_expr;
    char*           x_expr_str;
    AVExpr*         x_expr;
    char*           y_expr_str;
    AVExpr*         y_expr;

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
    { "schedule",           "binary file of <double> xyzxyzxyz...", OFFSET(schedule_file_path), AV_OPT_TYPE_STRING, {.str=""},       CHAR_MIN, CHAR_MAX, FLAGS },
    { "zoom",               "set zoom offset expression",           OFFSET(zoom_expr_str),  AV_OPT_TYPE_STRING, {.str="1"},          CHAR_MIN, CHAR_MAX, FLAGS },
    { "z",                  "set zoom offset expression",           OFFSET(zoom_expr_str),  AV_OPT_TYPE_STRING, {.str="1"},          CHAR_MIN, CHAR_MAX, FLAGS },
    { "x",                  "set x offset expression",              OFFSET(x_expr_str),     AV_OPT_TYPE_STRING, {.str="0.5"},        CHAR_MIN, CHAR_MAX, FLAGS },
    { "y",                  "set y offset expression",              OFFSET(y_expr_str),     AV_OPT_TYPE_STRING, {.str="0.5"},        CHAR_MIN, CHAR_MAX, FLAGS },
    { "ar",                 "set aspect ratio",                     OFFSET(outAspectRatio), AV_OPT_TYPE_DOUBLE, {.dbl=0},          0.00    ,   100   , FLAGS },
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
    zoom->var_values[VAR_ZOOM] = 1;
    zoom->var_values[VAR_X] = 0.5;
    zoom->var_values[VAR_Y] = 0.5;
    zoom->var_values[VAR_T] = NAN;
    zoom->schedule = NULL;
    zoom->schedule_size = 0;
    zoom->schedule_index = 0;

    if(zoom->outAspectRatio == 0) {
      zoom->outAspectRatio = 1.0 * inlink->w / inlink->h;
    }

    if (zoom->outAspectRatio <= 1) {
      zoom->zoom_max = FFMIN(zoom->outAspectRatio * inlink->h, inlink->h);
    }else{
      zoom->zoom_max = FFMIN(inlink->w, inlink->w / zoom->outAspectRatio);
    }

    if (strlen(zoom->schedule_file_path) > 0) {
      FILE *f = av_fopen_utf8(zoom->schedule_file_path, "r");
      uint64_t file_size = 0;

      if (!f) {
        int ret = AVERROR(errno);
        av_log(zoom, AV_LOG_ERROR, "Cannot open file '%s' for reading schedule: %s\n",
               zoom->schedule_file_path, av_err2str(ret));
        return ret;
      }

      fseek(f, 0L, SEEK_END);
      file_size = ftell(f);
      fseek(f, 0L, SEEK_SET);

      if(file_size == 0){
        av_log(zoom, AV_LOG_ERROR, "File '%s' contents are empty, file size is 0\n", zoom->schedule_file_path);
        return AVERROR(EINVAL);
      }

      if(file_size % sizeof(double) != 0) {
        av_log(zoom, AV_LOG_ERROR, "File '%s' contents are unaliged to double. File size %ld should be %ld\n",
            zoom->schedule_file_path,
            file_size,
            file_size / sizeof(double) * sizeof(double));
        return AVERROR(EINVAL);
      }

      unsigned long num_doubles = file_size / sizeof(double);

      if(num_doubles % 3 != 0) {
        av_log(zoom, AV_LOG_ERROR, "File '%s' double values are unaliged to XYZXYZXYZ... (not divisible by 3). Value count is %ld, should be %ld\n",
            zoom->schedule_file_path,
            num_doubles,
            num_doubles / 3 * 3);
        return AVERROR(EINVAL);
      }

      zoom->schedule_size = file_size / sizeof(double) / 3;

      zoom->schedule = av_malloc(sizeof(char) * file_size);
      if(zoom->schedule == NULL) {
        return AVERROR(ENOMEM);
      }
      fread(zoom->schedule, sizeof(char), file_size, f);
      fclose(f);
    }


    int ret;
    if(
            (ret = av_expr_parse(&zoom->zoom_expr, zoom->zoom_expr_str, var_names,
                                 NULL, NULL, NULL, NULL, 0, ctx)) < 0 ||
            (ret = av_expr_parse(&zoom->x_expr, zoom->x_expr_str, var_names,
                                 NULL, NULL, NULL, NULL, 0, ctx)) < 0 ||
            (ret = av_expr_parse(&zoom->y_expr, zoom->y_expr_str, var_names,
                                 NULL, NULL, NULL, NULL, 0, ctx)) < 0
        )
        return AVERROR(EINVAL);

    return 0;
}
static int config_output(AVFilterLink *outlink)
{
    ZoomContext *s = outlink->src->priv;

    const double aspectRatio = s->outAspectRatio;

    const int in_w = outlink->src->inputs[0]->w;
    const int in_h = outlink->src->inputs[0]->h;

    outlink->w = aspectRatio <= 1 ? aspectRatio * in_h : in_w;
    outlink->h = aspectRatio <= 1 ? in_h : in_w / aspectRatio;
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

    const int in_w  = in->width;
    const int in_h  = in->height;
    const int in_f  = in->format;

          int out_w = in->width * zoom_val;
          int out_h = in->height * zoom_val;
    const int out_f = outlink->format;

    int adjusted_in_w = in_w;
    int adjusted_in_h = in_h;

    const double aspectRatio = zoom->outAspectRatio;
    if (aspectRatio <= 1) {
      adjusted_in_w = aspectRatio * in_h;
    }else{
      adjusted_in_h = in_w / aspectRatio;
    }

    const double x  = zoom->x;
    const double y  = zoom->y;

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

    const int dx = av_clip_c(adjusted_in_w * x - out_w/2, 0, adjusted_in_w - out_w);
    const int dy = av_clip_c(adjusted_in_h * y - out_h/2, 0, adjusted_in_h - out_h);
    av_log(zoom, AV_LOG_DEBUG, "dx: %d dy: %d\n", dx, dy);
    av_log(zoom, AV_LOG_DEBUG, "in_w: %d in_h: %d\n", in_w, in_h);
    av_log(zoom, AV_LOG_DEBUG, "adjusted_in_w: %d adjusted_in_h: %d\n", adjusted_in_w,adjusted_in_h);

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

          int in_w  = in->width / zoom_val;
          int in_h  = in->height / zoom_val;
    const int in_f  = in->format;

    const double aspectRatio = zoom->outAspectRatio;
    if (aspectRatio <= 1) {
      in_w = aspectRatio * in_h;
    }else{
      in_h = in_w / aspectRatio;
    }

    const int out_w = out->width;
    const int out_h = out->height;
    const int out_f = outlink->format;

    const double x  = zoom->x;
    const double y  = zoom->y;

    if(out_h <= 0 || out_w <= 0)
        goto bypass;

    av_log(zoom, AV_LOG_DEBUG, "in_w: %d in_h: %d\n", in_w, in_h);
    av_log(zoom, AV_LOG_DEBUG, "out_w: %d out_h: %d\n", out_w, out_h);

    const int dx = av_clip_c(in->width * x - in_w / 2, 0, in->width - in_w);
    const int dy = av_clip_c(in->height * y - in_h / 2, 0, in->height - in_h);
    av_log(zoom, AV_LOG_DEBUG, "x: %0.3f y: %0.3f\n", x, y);
    av_log(zoom, AV_LOG_DEBUG, "dx: %d dy: %d\n", dx, dy);


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

    const int out_w = outlink->w;
    const int out_h = outlink->h;

    out = ff_get_video_buffer(outlink, out_w, out_h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }
    av_frame_copy_props(out, in);

    // eval T (time)
    zoom->var_values[VAR_T] = in->pts == AV_NOPTS_VALUE ?
                            NAN :
                            in->pts * av_q2d(inlink->time_base);

    if(zoom->schedule){
      unsigned long offset = zoom->schedule_index * 3;
      zoom->schedule_index += 1;

      double *schedule = zoom->schedule;

      if(offset >= zoom->schedule_size * 3){
        av_log(zoom, AV_LOG_WARNING, "schedule index %ld exceeds schedule size of %ld (%ld values)\n", offset / 3, zoom->schedule_size, zoom->schedule_size * 3);
        offset = zoom->schedule_size * 3 - 3;
      }

      av_log(zoom, AV_LOG_DEBUG, "schedule index %ld x:%.3f y:%.3f z:%.3f\n", offset / 3, zoom->schedule_size, zoom->schedule_size * 3);

      // XYZ
      zoom->x = zoom->var_values[VAR_X] = schedule[offset + 0];
      zoom->y = zoom->var_values[VAR_Y] = schedule[offset + 1];
      zoom_val = zoom->zoom = zoom->var_values[VAR_Z] = zoom->var_values[VAR_ZOOM] = schedule[offset + 2];
    }else{
      // eval Z (zoom)
      zoom_val = zoom->zoom = zoom->var_values[VAR_Z] = zoom->var_values[VAR_ZOOM] = av_expr_eval(zoom->zoom_expr,
                                                                                                  zoom->var_values,
                                                                                                  NULL);
      // eval x/y
      zoom->x = zoom->var_values[VAR_X] = av_expr_eval(zoom->x_expr, zoom->var_values, NULL);
      zoom->y = zoom->var_values[VAR_Y] = av_expr_eval(zoom->y_expr, zoom->var_values, NULL);
    }

    if(zoom->zoom < 0 || zoom->zoom >= zoom->zoom_max) {
      av_log(zoom, AV_LOG_WARNING, "zoom value %.2f is out of range of [0-%.3f]\n", zoom->zoom, zoom->zoom_max);
      zoom->zoom = av_clipd_c(zoom->zoom, 0, zoom->zoom_max);
    }
    if(zoom->x < 0 || zoom->x > 1){
        av_log(zoom, AV_LOG_WARNING, "x position %.2f is out of range of [0-1]\n", zoom->x);
        zoom->x = av_clipd_c(zoom->x, 0, 1);
    }
		if(zoom->y < 0 || zoom->y > 1){
				av_log(zoom, AV_LOG_WARNING, "y position %.2f is out of range of [0-1]\n", zoom->y);
        zoom->y = av_clipd_c(zoom->y, 0, 1);
		}
    // copy in the background
    ff_fill_rectangle(&zoom->dc, &zoom->fillcolor,
                      out->data, out->linesize,
                      0, 0,
                      out_w, out_h);

    // scale
    if(zoom_val == 1) {
        // it's 1, just copy
        // quite an expensive noop :D
        ff_copy_rectangle2(&zoom->dc,
                           out->data, out->linesize,
                           in->data, in->linesize,
                           0, 0,
                           av_clip_c(in_w * zoom->x - out_w / 2.0, 0, in_w - out_w),
                           av_clip_c(in_h * zoom->y - out_h / 2.0, 0, in_h - out_h),
                           out_w, out_h);
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
    av_expr_free(zoom->x_expr);
    av_expr_free(zoom->y_expr);

    zoom->zoom_expr = NULL;
    if (zoom->schedule != NULL)
      av_freep(zoom->schedule);
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
