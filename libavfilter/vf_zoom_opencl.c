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
#include "opencl.h"
#include "opencl_source.h"

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

typedef struct ZoomOpenCLContext {
    OpenCLFilterContext ocf;

    FFDrawContext             dc;
    const AVPixFmtDescriptor* desc;

    char*           schedule_file_path;
    double*         schedule;
    unsigned long   schedule_size;
    unsigned long   schedule_index;

    double          zoom_max;
    // used for actual zoom
    double          zoom;
    // used to determine if we need to adjust the zoom when w/h are set & different than expected
    double          shadowZoom;
    double          x;
    double          y;
    FFDrawColor     fillcolor;

    int             desiredWidth;
    int             desiredHeight;
    int             exact;
    double          outAspectRatio;

    char*           zoom_expr_str;
    AVExpr*         zoom_expr;
    char*           x_expr_str;
    AVExpr*         x_expr;
    char*           y_expr_str;
    AVExpr*         y_expr;

    int     nb_planes;
    double  var_values[VAR_VARS_NB];

    int              initialised;
    cl_kernel        kernel;
    cl_command_queue command_queue;

} ZoomOpenCLContext;

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

#define OFFSET(x) offsetof(ZoomOpenCLContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption zoom_opencl_options[] = {
    { "schedule",           "binary file of <double> xyzxyzxyz...", OFFSET(schedule_file_path), AV_OPT_TYPE_STRING, {.str=""},       CHAR_MIN, CHAR_MAX, FLAGS },
    { "zoom",               "set zoom offset expression",           OFFSET(zoom_expr_str),  AV_OPT_TYPE_STRING, {.str="1"},          CHAR_MIN, CHAR_MAX, FLAGS },
    { "z",                  "set zoom offset expression",           OFFSET(zoom_expr_str),  AV_OPT_TYPE_STRING, {.str="1"},          CHAR_MIN, CHAR_MAX, FLAGS },
    { "x",                  "set x offset expression",              OFFSET(x_expr_str),     AV_OPT_TYPE_STRING, {.str="0.5"},        CHAR_MIN, CHAR_MAX, FLAGS },
    { "y",                  "set y offset expression",              OFFSET(y_expr_str),     AV_OPT_TYPE_STRING, {.str="0.5"},        CHAR_MIN, CHAR_MAX, FLAGS },
    { "ar",                 "set aspect ratio",                     OFFSET(outAspectRatio), AV_OPT_TYPE_DOUBLE, {.dbl=0},            0.00    ,   100   , FLAGS },
    { "width",              "set desired width",                    OFFSET(desiredWidth),   AV_OPT_TYPE_INT   , {.i64=-1},           -1      ,   65536 , FLAGS },
    { "height",             "set desired height",                   OFFSET(desiredHeight),  AV_OPT_TYPE_INT   , {.i64=-1},           -1      ,   65536 , FLAGS },
    { "exact",              "set frame size is exact or div by 2",  OFFSET(exact),          AV_OPT_TYPE_BOOL  , {.i64=0},            0       ,   1     , FLAGS },
    { "fillcolor",          "set color for background",             OFFSET(fillcolor.rgba), AV_OPT_TYPE_COLOR,  {.str="black@0"},  CHAR_MIN, CHAR_MAX, FLAGS },

    { NULL }
};

AVFILTER_DEFINE_CLASS(zoom_opencl);

static av_cold int init(AVFilterContext *ctx)
{
    return ff_opencl_filter_init(ctx);
}

static int config_props(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    ZoomOpenCLContext *zoom = ctx->priv;

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
    if(zoom->desiredWidth > 0 && zoom->desiredHeight > 0) {
      zoom->outAspectRatio = 1.0 * zoom->desiredWidth / zoom->desiredHeight;
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

    return ff_opencl_filter_config_input(inlink);
}
static int config_output(AVFilterLink *outlink)
{
    ZoomOpenCLContext *s = outlink->src->priv;
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
    s->shadowZoom = 1;

    if(s->desiredWidth > 0 && s->desiredHeight > 0) {
        s->shadowZoom = 1.0 * s->desiredWidth / outlink->w;
        outlink->w = s->desiredWidth;
        outlink->h = s->desiredHeight;
    }

    if(!s->exact) {
        if (outlink->w % 2 != 0) {
            outlink->w -= 1;
        }
        if (outlink->h % 2 != 0) {
            outlink->h -= 1;
        }
    }
    if(outlink->w <= 0){
      outlink->w = 2;
    }
    if(outlink->h <= 0){
      outlink->h = 2;
    }
    s->ocf.output_width = outlink->w;
    s->ocf.output_height = outlink->h;

    return ff_opencl_filter_config_output(outlink);
}

static int zoom_opencl_load(AVFilterContext *avctx, AVFrame *in)
{
    ZoomOpenCLContext *ctx = avctx->priv;
    cl_int cle;
    int err, planes;
    AVHWFramesContext *hwfc = (AVHWFramesContext*)in->hw_frames_ctx->data;


    ctx->desc = av_pix_fmt_desc_get(hwfc->sw_format);
    ff_draw_init(&ctx->dc,  hwfc->sw_format,     FF_DRAW_PROCESS_ALPHA);
    ff_draw_color(&ctx->dc, &ctx->fillcolor,     ctx->fillcolor.rgba );


    err = ff_opencl_filter_load_program(avctx, &ff_opencl_source_zoom, 1);
    if (err < 0)
        return err;

    planes = 0;
    for (int i = 0; i < ctx->desc->nb_components; i++)
        planes = FFMAX(planes,
                       ctx->desc->comp[i].plane + 1);

    ctx->nb_planes = planes;

    ctx->command_queue = clCreateCommandQueue(ctx->ocf.hwctx->context,
                                              ctx->ocf.hwctx->device_id,
                                              0, &cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create OpenCL "
                                   "command queue %d.\n", cle);

    ctx->kernel = clCreateKernel(ctx->ocf.program, "zoom", &cle);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to create kernel %d.\n", cle);

    ctx->initialised = 1;

    return 0;

    fail:
    if (ctx->command_queue)
        clReleaseCommandQueue(ctx->command_queue);
    if (ctx->kernel)
        clReleaseKernel(ctx->kernel);
    return err;
}

static int filter_frame(AVFilterLink *avctx, AVFrame *in)
{
    AVFilterContext *ctx = avctx->dst;
    ZoomOpenCLContext *zoom = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];
    int err, plane, kernel_arg;
    AVFrame *out;
    size_t global_work[2];
    cl_int cle;

    if (!in->hw_frames_ctx)
        return AVERROR(EINVAL);

    const int out_w = outlink->w;
    const int out_h = outlink->h;

    if (!zoom->initialised) {
        err = zoom_opencl_load(ctx, in);
        if (err < 0)
            return err;
    }

    out = ff_get_video_buffer(outlink, out_w, out_h);
    if (!out) {
        av_frame_free(&in);
        return AVERROR(ENOMEM);
    }

    // eval T (time)
    zoom->var_values[VAR_T] = in->pts == AV_NOPTS_VALUE ?
                            NAN :
                            in->pts * av_q2d(avctx->time_base);

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
      zoom->zoom = zoom->var_values[VAR_Z] = zoom->var_values[VAR_ZOOM] = schedule[offset + 2];
      av_log(zoom, AV_LOG_DEBUG, "schedule index %ld x:%.3f y:%.3f z:%.3f\n", offset / 3, zoom->x, zoom->y, zoom->zoom);

    }else{
      // eval Z (zoom)
      zoom->zoom = zoom->var_values[VAR_Z] = zoom->var_values[VAR_ZOOM] = av_expr_eval(zoom->zoom_expr,
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

    for (plane = 0; plane < zoom->nb_planes; plane++) {
        cl_mem mem;
        cl_float2 pan = {(float)zoom->x, (float)zoom->y};
        cl_float cl_zoom = zoom->zoom;
        cl_float cl_shadowZoom = zoom->shadowZoom;
        cl_float cl_oob_plane_color = zoom->fillcolor.comp[plane].u32[0] / (float)(1 << zoom->desc->comp[plane].depth);
        kernel_arg = 0;

        mem = (cl_mem)out->data[plane];
        CL_SET_KERNEL_ARG(zoom->kernel, kernel_arg, cl_mem, &mem);
        kernel_arg++;

        CL_SET_KERNEL_ARG(zoom->kernel, kernel_arg, cl_int, &zoom->schedule_index);
        kernel_arg++;

        CL_SET_KERNEL_ARG(zoom->kernel, kernel_arg, cl_float2, &pan);
        kernel_arg++;

        CL_SET_KERNEL_ARG(zoom->kernel, kernel_arg, cl_float, &cl_zoom);
        kernel_arg++;

        CL_SET_KERNEL_ARG(zoom->kernel, kernel_arg, cl_float, &cl_shadowZoom);
        kernel_arg++;

        CL_SET_KERNEL_ARG(zoom->kernel, kernel_arg, cl_float, &cl_oob_plane_color);
        kernel_arg++;

        mem = (cl_mem)in->data[plane];
        CL_SET_KERNEL_ARG(zoom->kernel, kernel_arg, cl_mem, &mem);
        kernel_arg++;

        err = ff_opencl_filter_work_size_from_image(ctx, global_work,
                                                    out, plane, 0);
        if (err < 0)
            goto fail;

        cle = clEnqueueNDRangeKernel(zoom->command_queue, zoom->kernel, 2, NULL,
                                     global_work, NULL, 0, NULL, NULL);
        CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to enqueue xfade kernel "
                                       "for plane %d: %d.\n", plane, cle);
    }

    cle = clFinish(zoom->command_queue);
    CL_FAIL_ON_ERROR(AVERROR(EIO), "Failed to finish command queue: %d.\n", cle);

    err = av_frame_copy_props(out, in);
    if (err < 0)
        goto fail;

    av_frame_free(&in);
    return ff_filter_frame(outlink, out);

fail:
    av_frame_free(&out);
    return err;
}

static av_cold void uninit(AVFilterContext *ctx)
{
    ZoomOpenCLContext *zoom = ctx->priv;
    av_expr_free(zoom->zoom_expr);
    av_expr_free(zoom->x_expr);
    av_expr_free(zoom->y_expr);

    zoom->zoom_expr = NULL;
    if (zoom->schedule != NULL)
      av_free(zoom->schedule);

    cl_int cle;

    if (zoom->kernel) {
        cle = clReleaseKernel(zoom->kernel);
        if (cle != CL_SUCCESS)
            av_log(ctx, AV_LOG_ERROR, "Failed to release kernel: %d.\n", cle);
    }

    if (zoom->command_queue) {
        cle = clReleaseCommandQueue(zoom->command_queue);
        if (cle != CL_SUCCESS)
            av_log(ctx, AV_LOG_ERROR, "Failed to release "
                                        "command queue: %d.\n", cle);
    }

    ff_opencl_filter_uninit(ctx);
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

AVFilter ff_vf_zoom_opencl = {
    .name          = "zoom_opencl",
    .description   = NULL_IF_CONFIG_SMALL("Zoom in and out video, apply pan and change aspect ratio."),
    .priv_size     = sizeof(ZoomOpenCLContext),
    .init          = init,
    .uninit        = uninit,
    .query_formats = ff_opencl_filter_query_formats,
    .inputs        = zoom_inputs,
    .outputs       = zoom_outputs,
    .priv_class    = &zoom_opencl_class,
    .flags_internal = FF_FILTER_FLAG_HWFRAME_AWARE
};
