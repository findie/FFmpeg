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

    int hsub, vsub;
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
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV422P,
        AV_PIX_FMT_YUV444P,
        AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUV411P,
        AV_PIX_FMT_GRAY8,
        AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUVJ422P,
        AV_PIX_FMT_YUVJ444P,
        AV_PIX_FMT_NV12,
        AV_PIX_FMT_NV21,
        AV_PIX_FMT_GRAY16LE,
        AV_PIX_FMT_YUV440P,
        AV_PIX_FMT_YUVJ440P,
        AV_PIX_FMT_YUVA420P,
        AV_PIX_FMT_YUV420P16LE,
        AV_PIX_FMT_YUV422P16LE,
        AV_PIX_FMT_YUV444P16LE,
        AV_PIX_FMT_YA8,
        AV_PIX_FMT_GBRP,
        AV_PIX_FMT_GBRP16LE,
        AV_PIX_FMT_YUVA422P,
        AV_PIX_FMT_YUVA444P,
        AV_PIX_FMT_YUVA420P16LE,
        AV_PIX_FMT_YUVA422P16LE,
        AV_PIX_FMT_YUVA444P16LE,
        AV_PIX_FMT_NV16,
        AV_PIX_FMT_YA16LE,
        AV_PIX_FMT_GBRAP,
        AV_PIX_FMT_GBRAP16LE,
        AV_PIX_FMT_YUVJ411P,
        AV_PIX_FMT_NV24,
        AV_PIX_FMT_NV42,

        AV_PIX_FMT_NONE
    };

    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}
static int config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ZoomContext *zoom = ctx->priv;

    zoom->desc = av_pix_fmt_desc_get(inlink->format);

    zoom->hsub = zoom->desc->log2_chroma_w;
    zoom->vsub = zoom->desc->log2_chroma_h;

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

    const double originalAspectRatio = 1.0 * in_w / in_h;

    if(originalAspectRatio < aspectRatio){
      outlink->w = in_w;
      outlink->h = round(in_h * (originalAspectRatio / aspectRatio));
    }else{
      outlink->w = round(in_w * (aspectRatio / originalAspectRatio));
      outlink->h = in_h;
    }

    if(outlink->w % 2 != 0){
      outlink->w -= 1;
    }
    if(outlink->h % 2 != 0){
      outlink->h -= 1;
    }
    if(outlink->w <= 0){
      outlink->w = 2;
    }
    if(outlink->h <= 0){
      outlink->h = 2;
    }

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

// this function takes x/y already scaled to the chroma sub
// supports only planar formats
static inline uint8_t sample8_bilinear_at(uint8_t *data,
                                          int linesize, int pixelstep,
                                          float x, float y,
                                          int w, int h,
                                          int8_t oob_value
                                          )
{
    int ix = x;
    int iy = y;
    float fracx = x - ix;
    float fracy = y - iy;
    float ifracx = 1.0f - fracx;
    float ifracy = 1.0f - fracy;
    float lin0, lin1;

    // check if requested value is out of bounds
    if(x < 0 || y < 0 || x > w - 1 || y > h - 1){
        return oob_value;
    }

    uint8_t *row_y = data + iy * linesize;

    // top left
    uint8_t *a11 = row_y + ix * pixelstep;
    // top right = top left + 1px
    uint8_t *a12 = a11 + pixelstep;

    // bottom left = top left + 1row
    uint8_t *a21 = a11 + linesize;
    // bottom right = bottom left + 1px
    uint8_t *a22 = a21 + pixelstep;

    // top interp
    lin0 = ifracx * (*a11) + fracx * (*a12);
    // bottom interp
    lin1 = ifracx * (*a21) + fracx * (*a22);

    // vertical interp
    return ifracy * lin0 + fracy * lin1;
}


// this function takes x/y already scaled to the chroma sub
// supports only planar formats
static inline uint16_t sample16_bilinear_at(uint8_t *data,
                                            int linesize, int pixelstep,
                                            float x, float y,
                                            int w, int h,
                                            int16_t oob_value
                                            )
{
    int ix = x;
    int iy = y;
    float fracx = x - ix;
    float fracy = y - iy;
    float ifracx = 1.0f - fracx;
    float ifracy = 1.0f - fracy;
    float lin0, lin1;

    // check if requested value is out of bounds
    if(x < 0 || y < 0 || x > w - 1 || y > h - 1){
        return oob_value;
    }

    uint8_t *row_y = data + iy * linesize;

    // top left
    uint8_t *a11 = row_y + ix * pixelstep;
    // top right = top left + 1px
    uint8_t *a12 = a11 + pixelstep;

    // bottom left = top left + 1row
    uint8_t *a21 = a11 + linesize;
    // bottom right = bottom left + 1px
    uint8_t *a22 = a21 + pixelstep;

    // top interp
    lin0 = ifracx * (*((uint16_t*)a11)) + fracx * (*((uint16_t*)a12));
    // bottom interp
    lin1 = ifracx * (*((uint16_t*)a21)) + fracx * (*((uint16_t*)a22));

    // vertical interp
    return ifracy * lin0 + fracy * lin1;
}


typedef struct float2 {
    float x, y;
} float2;

static inline float2 scale_coords_pxout_to_pxin(float2 pix_out, float2 dim_out, float ZOOM, float2 dim_in, float2 PAN) {

    float2 pix_in;

    if (ZOOM < 1) {
        //                                               canvas offset   obj scaled center offset   scaled px location
        // px_out                                      = dim_out * PAN - dim_in / 2 * ZOOM        + px_in * ZOOM

        // -dim_out * PAN + px_out                     = (-dim_in/2 + px_in) * ZOOM

        // (-dim_out * PAN + px_out) / ZOOM            = -dim_in/2 + px_in

        // (-dim_out * PAN + px_out) / ZOOM + dim_in/2 = px_in

        pix_in.x = (-dim_out.x * PAN.x + pix_out.x) / ZOOM + dim_in.x / 2;
        pix_in.y = (-dim_out.y * PAN.y + pix_out.y) / ZOOM + dim_in.y / 2;
    }
    // zoom >= 1
    else {

        pix_in.x = (pix_out.x - dim_out.x / 2.0f) / ZOOM + dim_in.x * PAN.x;
        pix_in.y = (pix_out.y - dim_out.y / 2.0f) / ZOOM + dim_in.y * PAN.y;
    }

    return pix_in;
}


static inline float2 scale_coords_find_PAN(float2 pix_in, float2 pix_out, float2 dim_out, float ZOOM, float2 dim_in) {
    float2 PAN;

    if(ZOOM < 1){
        // taken from scale_coords_pxout_to_pxin
        // pix_in                                                = (-dim_out * PAN + pix_out) / ZOOM + dim_in / 2;
        // pix_in - dim_in / 2                                   = (-dim_out * PAN + pix_out) / ZOOM
        // (pix_in - dim_in / 2) * ZOOM                          = -dim_out * PAN + pix_out
        // (pix_in - dim_in / 2) * ZOOM - pix_out                = -dim_out * PAN
        // ((pix_in - dim_in / 2) * ZOOM - pix_out) / (-dim_out) = PAN

        PAN.x = ((pix_in.x - dim_in.x / 2.0f) * ZOOM - pix_out.x) / (-dim_out.x);
        PAN.y = ((pix_in.y - dim_in.y / 2.0f) * ZOOM - pix_out.y) / (-dim_out.y);
    }
    // zoom >= 1
    else {
        // taken from scale_coords_pxout_to_pxin
        // pix_in                                           = (pix_out - dim_out/2) / ZOOM + dim_in * PAN
        // pix_in - (pix_out - dim_out/2) / ZOOM            = dim_in * PAN
        // (pix_in - (pix_out - dim_out/2) / ZOOM) / dim_in = PAN

        PAN.x = (pix_in.x - (pix_out.x - dim_out.x / 2.0f) / ZOOM) / dim_in.x;
        PAN.y = (pix_in.y - (pix_out.y - dim_out.y / 2.0f) / ZOOM) / dim_in.y;
    }
    return PAN;
}

static inline float clampf(float val, float min, float max) {
    if(val < min) return min;
    if(val > max) return max;
    return val;
}

static inline float2 clamp_pan_inbounds(float2 PAN, float2 dim_out, float ZOOM, float2 dim_in) {

    float2 adjusted_dim_in = {dim_in.x * ZOOM, dim_in.y * ZOOM};
    
    float2 top_left = scale_coords_find_PAN((float2){0.0f, 0.0f}, (float2){-1.0f, -1.0f}, dim_out, ZOOM, dim_in);
    //float2 bottom_right = {1.0f - top_left.x, 1.0f - top_left.y};
    float2 bottom_right = scale_coords_find_PAN((float2){ dim_in.x + 0.0f,  dim_in.y + 0.0f},
                                                (float2){dim_out.x + ZOOM, dim_out.y + ZOOM},
                                                dim_out, ZOOM, dim_in);

    float2 CLAMPED_PAN = {0.0f, 0.0f};

    if(ZOOM < 1) {
        // if it fits
        if(adjusted_dim_in.x <= dim_out.x && adjusted_dim_in.y <= dim_out.y) {
            CLAMPED_PAN.x = clampf(PAN.x, top_left.x, bottom_right.x);
            CLAMPED_PAN.y = clampf(PAN.y, top_left.y, bottom_right.y);
        }
            // if it doesn't fix
        else {

            CLAMPED_PAN.x = adjusted_dim_in.x > dim_out.x ?
                            // doesn't fit on W
                            FFMAX(FFMIN(1 - PAN.x, top_left.x), bottom_right.x):
                            // fits on W
                            FFMAX(FFMIN(PAN.y, bottom_right.x), top_left.x);
            CLAMPED_PAN.y = adjusted_dim_in.y > dim_out.y ?
                            // doesn't fit on W
                            FFMAX(FFMIN(1 - PAN.y, top_left.y), bottom_right.y):
                            // fits on W
                            FFMAX(FFMIN(PAN.y, bottom_right.y), top_left.y);

        }
    } else {

        CLAMPED_PAN.x = clampf(PAN.x, top_left.x, bottom_right.x);
        CLAMPED_PAN.y = clampf(PAN.y, top_left.y, bottom_right.y);
    }

    return CLAMPED_PAN;
}


static void apply_zoom_plane(float ZOOM, float2 PAN, int plane,
                             int pix_step,
                             int pix_depth,
                             uint8_t *in, int linesize_in,
                             uint8_t *out, int linesize_out,
                             float2 dim_in,
                             float2 dim_out,
                             FFDrawColor *fillcolor) {
    int x, y;

    int h = dim_out.y;
    int w = dim_out.x;

    for(y = 0; y < h; y++) {
        for(x = 0; x < w; x++) {

            float2 src_location = scale_coords_pxout_to_pxin(
                (float2){(float)x, (float)y},
                dim_out,
                ZOOM,
                dim_in,
                PAN
            );

            if (pix_depth == 8) {
                int8_t value = sample8_bilinear_at(
                    in,
                    linesize_in,
                    pix_step,
                    src_location.x, src_location.y,
                    dim_in.x, dim_in.y,
                    fillcolor->comp[plane].u8[0] // out of bounds value
                );

                int8_t *dst_pixel = out +
                                    y * linesize_out +
                                    x * pix_step;

                (*dst_pixel) = value;
            }
            else {
                int16_t value = sample16_bilinear_at(
                                    in,
                                    linesize_in,
                                    pix_step,
                                    src_location.x, src_location.y,
                                    dim_in.x, dim_in.y,
                                    fillcolor->comp[plane].u16[0] // out of bounds value
                                );

                int16_t *dst_pixel = out +
                                     y * linesize_out +
                                     x * pix_step;

                (*dst_pixel) = value;
            }
        }
    }


}


static int apply_zoom(ZoomContext *s, AVFrame *in, AVFrame *out, FFDrawColor *fillcolor){
    int x, y, plane;

    int out_w = out->width;
    int out_h = out->height;
    int in_w = in->width;
    int in_h = in->height;

    int hsub = s->hsub;
    int vsub = s->vsub;
    const FFDrawContext *draw = &s->dc;
    const struct AVPixFmtDescriptor *desc = draw->desc;


    float2 dim_in_full  = {(float) in_w, (float) in_h};
    float2 dim_out_full = {(float)out_w, (float)out_h};

    float2 dim_in_chroma  = {(float)( in_w >> hsub), (float)( in_h >> vsub)};
    float2 dim_out_chroma = {(float)(out_w >> hsub), (float)(out_h >> vsub)};

    const float  ZOOM           = s->zoom;
    const float2 UNCLAMPED_PAN  = {s->x, s->y};
          float2 PAN            = clamp_pan_inbounds(UNCLAMPED_PAN, dim_out_full, ZOOM, dim_in_full);

    printf("ZOOM %.3f\n", ZOOM);
    printf("UNCLAMPED_PAN x %.3f y %.3f\n", UNCLAMPED_PAN.x, UNCLAMPED_PAN.y);
    printf("PAN x %.3f y %.3f\n", PAN.x, PAN.y);

    for(plane = 0; plane < desc->nb_components; plane++){
        float2 dim_in  = plane == 1 || plane == 2 ? dim_in_chroma  : dim_in_full;
        float2 dim_out = plane == 1 || plane == 2 ? dim_out_chroma : dim_out_full;

        apply_zoom_plane(ZOOM, PAN, plane,
                         desc->comp[plane].step,
                         desc->comp[plane].depth,
                         in->data[plane], in->linesize[plane],
                         out->data[plane], out->linesize[plane],
                         dim_in,
                         dim_out,
                         fillcolor);
    }

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

      // XYZ
      zoom->x = zoom->var_values[VAR_X] = schedule[offset + 0];
      zoom->y = zoom->var_values[VAR_Y] = schedule[offset + 1];
      zoom_val = zoom->zoom = zoom->var_values[VAR_Z] = zoom->var_values[VAR_ZOOM] = schedule[offset + 2];
      av_log(zoom, AV_LOG_DEBUG, "schedule index %ld x:%.3f y:%.3f z:%.3f\n", offset / 3, zoom->x, zoom->y, zoom->zoom);

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

	apply_zoom(zoom, in, out, &zoom->fillcolor);

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
      av_free(zoom->schedule);
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
