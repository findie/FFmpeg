/*
 * Copyright (c) 2008 Affine Systems, Inc (Michael Sullivan, Bobby Impollonia)
 * Copyright (c) 2013 Andrey Utkin <andrey.krieger.utkin gmail com>
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
 * Box and grid drawing filters. Also a nice template for a filter
 * that needs to write in the input frame.
 */

#include "../libavutil/colorspace.h"
#include "../libavutil/common.h"
#include "../libavutil/opt.h"
#include "../libavutil/eval.h"
#include "../libavutil/pixdesc.h"
#include "../libavutil/parseutils.h"
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"

static const char *const var_names[] = {
    "dar",
    "hsub", "vsub",
    "in_h", "ih",      ///< height of the input video
    "in_w", "iw",      ///< width  of the input video
    "sar",
    "x",
    "y",
    "h",              ///< height of the rendered box
    "w",              ///< width  of the rendered box
    "t",
    "thickness",
    "time",
    "fill",
//    "c_r", "c_g", "c_b",
    NULL
};

enum { Y, U, V, A };
enum { R, G, B };

enum var_name {
    VAR_DAR,
    VAR_HSUB, VAR_VSUB,
    VAR_IN_H, VAR_IH,
    VAR_IN_W, VAR_IW,
    VAR_SAR,
    VAR_X,
    VAR_Y,
    VAR_H,
    VAR_W,
    VAR_T,
    VAR_THICKNESS,
    VAR_TIME,
    VAR_MAX,
//    VAR_C_R, VAR_C_G, VAR_C_B,
    VARS_NB
};

typedef struct DrawBoxContext {
    const AVClass *class;
    int x, y, w, h;
    int thickness;

    char *color_str;
    uint8_t rgba_color[4];

    unsigned char yuv_color[4];
    int invert_color; ///< invert luma color
    int vsub, hsub;   ///< chroma subsampling
    char *x_expr, *y_expr; ///< expression for x and y
    char *w_expr, *h_expr; ///< expression for width and height
    char *t_expr;          ///< expression for thickness
    double time;           ///< time
    int have_alpha;
    int replace;

    char *color_alpha_expr; /// < expression for alpha
    char *color_red_expr, *color_green_expr, *color_blue_expr; /// < expression for dynamic color RGB
    char *color_y_expr, *color_u_expr, *color_v_expr; /// < expression for dynamic color YUV
} DrawBoxContext;

static const int NUM_EXPR_EVALS = 5;

static void apply_color_rgba (DrawBoxContext *s, uint8_t rgba_color[4])
{
    s->yuv_color[Y] = RGB_TO_Y_CCIR(rgba_color[0], rgba_color[1], rgba_color[2]);
    s->yuv_color[U] = RGB_TO_U_CCIR(rgba_color[0], rgba_color[1], rgba_color[2], 0);
    s->yuv_color[V] = RGB_TO_V_CCIR(rgba_color[0], rgba_color[1], rgba_color[2], 0);
    s->yuv_color[A] = rgba_color[3];
}
static void apply_color_yuva (DrawBoxContext *s, uint8_t yuva_color[4])
{
    s->yuv_color[Y] = yuva_color[Y];
    s->yuv_color[U] = yuva_color[U];
    s->yuv_color[V] = yuva_color[V];
    s->yuv_color[A] = yuva_color[A];
}

static av_cold int init(AVFilterContext *ctx)
{
    DrawBoxContext *s = ctx->priv;

    if (!strcmp(s->color_str, "invert"))
        s->invert_color = 1;
    else if (av_parse_color(s->rgba_color, s->color_str, -1, ctx) < 0)
        return AVERROR(EINVAL);

    if (!s->invert_color) {
        apply_color_rgba(s, s->rgba_color);
    }

    return 0;
}

static int query_formats(AVFilterContext *ctx)
{
    static const enum AVPixelFormat pix_fmts[] = {
        AV_PIX_FMT_YUV444P,  AV_PIX_FMT_YUV422P,  AV_PIX_FMT_YUV420P,
        AV_PIX_FMT_YUV411P,  AV_PIX_FMT_YUV410P,
        AV_PIX_FMT_YUVJ444P, AV_PIX_FMT_YUVJ422P, AV_PIX_FMT_YUVJ420P,
        AV_PIX_FMT_YUV440P,  AV_PIX_FMT_YUVJ440P,
        AV_PIX_FMT_YUVA420P, AV_PIX_FMT_YUVA422P, AV_PIX_FMT_YUVA444P,
        AV_PIX_FMT_NONE
    };
    AVFilterFormats *fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int parse_data(AVFilterLink *inlink)
{
    // fixme optimize this routine
    // parse_and_eval should only be eval (parsing should be done at init)
    // there is no need to eval 5 times
    AVFilterContext *ctx = inlink->dst;
    DrawBoxContext *s = ctx->priv;
    double var_values[VARS_NB], res;
    char *expr;
    int ret;
    int i;

    var_values[VAR_IN_H] = var_values[VAR_IH] = inlink->h;
    var_values[VAR_IN_W] = var_values[VAR_IW] = inlink->w;
    var_values[VAR_SAR]  = inlink->sample_aspect_ratio.num ? av_q2d(inlink->sample_aspect_ratio) : 1;
    var_values[VAR_DAR]  = (double)inlink->w / inlink->h * var_values[VAR_SAR];
    var_values[VAR_HSUB] = s->hsub;
    var_values[VAR_VSUB] = s->vsub;
    var_values[VAR_X] = s->x;
    var_values[VAR_Y] = s->y;
    var_values[VAR_H] = s->h;
    var_values[VAR_W] = s->w;
    var_values[VAR_THICKNESS] = s->thickness;
    var_values[VAR_T] = s->time;
    var_values[VAR_TIME] = s->time;

    for (i = 0; i <= NUM_EXPR_EVALS; i++) {
        /* evaluate expressions, fail on last iteration */
        var_values[VAR_MAX] = inlink->w - s->x;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->w_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->w = var_values[VAR_W] = res;

        var_values[VAR_MAX] = inlink->h - s->y;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->h_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->h = var_values[VAR_H] = res;

        var_values[VAR_MAX] = inlink->w;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->x_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->x = var_values[VAR_X] = res;

        var_values[VAR_MAX] = inlink->h;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->y_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->y = var_values[VAR_Y] = res;

        var_values[VAR_MAX] = INT_MAX;
        if ((ret = av_expr_parse_and_eval(&res, (expr = s->t_expr),
                                          var_names, var_values,
                                          NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
            goto fail;
        s->thickness = var_values[VAR_THICKNESS] = res;

        // parse expression for A
        if(s->color_alpha_expr && s->color_alpha_expr[0])
        {
            av_log(s, AV_LOG_VERBOSE, "Enabled dynamic color on A! \n");

            if ((ret = av_expr_parse_and_eval(&res, (expr = s->color_alpha_expr),
                                              var_names, var_values,
                                              NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
                goto fail;
            s->yuv_color[A] = s->rgba_color[A] = av_clip_uint8_c(res);
            av_log(s, AV_LOG_VERBOSE, "A: %d \n", s->rgba_color[A]);
        }

        // parse expressions for RGB
        if(
            (s->color_red_expr && s->color_red_expr[0]) ||
            (s->color_green_expr && s->color_green_expr[0]) ||
            (s->color_blue_expr && s->color_blue_expr[0])
        )
        {
            uint8_t rgba[4];
            rgba[A] = s->yuv_color[A];
            av_log(s, AV_LOG_VERBOSE, "Enabled dynamic color on RGB! \n");

            for(uint8_t component = 0; component < 3; component++)
            {
                char *c_expr = component == 0 ? s->color_red_expr   :
                               component == 1 ? s->color_green_expr :
                                         /* 2 */s->color_blue_expr  ;

                if(c_expr[0]){
                    if ((ret = av_expr_parse_and_eval(&res, (expr = c_expr),
                                                      var_names, var_values,
                                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
                        goto fail;
                    rgba[component] = av_clip_uint8_c(res);
                } else {
                    rgba[component] = s->rgba_color[component];
                }
            }

            av_log(s, AV_LOG_VERBOSE, "R: %d G: %d G: %d\n", rgba[R], rgba[G], rgba[R]);

            apply_color_rgba(s, rgba);
        }

        // parse expressions for YUV
        if(
            (s->color_y_expr && s->color_y_expr[0]) ||
            (s->color_u_expr && s->color_u_expr[0]) ||
            (s->color_v_expr && s->color_v_expr[0])
        )
        {
            uint8_t yuva[4];
            yuva[A] = s->yuv_color[A];
            av_log(s, AV_LOG_VERBOSE, "Enabled dynamic color on YUV! \n");

            for(uint8_t component = 0; component < 3; component++)
            {
                char *c_expr = component == 0 ? s->color_y_expr   :
                               component == 1 ? s->color_u_expr :
                                         /* 2 */s->color_v_expr  ;

                if(c_expr[0]){
                    if ((ret = av_expr_parse_and_eval(&res, (expr = c_expr),
                                                      var_names, var_values,
                                                      NULL, NULL, NULL, NULL, NULL, 0, ctx)) < 0 && i == NUM_EXPR_EVALS)
                        goto fail;
                    yuva[component] = av_clip_uint8_c(res);
                } else {
                    yuva[component] = s->yuv_color[component];
                }
            }

            av_log(s, AV_LOG_VERBOSE, "Y: %d U: %d V: %d\n", yuva[R], yuva[G], yuva[R]);

            apply_color_yuva(s, yuva);
        }

    }

    /* if w or h are zero, use the input w/h */
    s->w = (s->w > 0) ? s->w : inlink->w;
    s->h = (s->h > 0) ? s->h : inlink->h;

    /* sanity check width and height */
    if (s->w <  0 || s->h <  0) {
        av_log(ctx, AV_LOG_ERROR, "Size values less than 0 are not acceptable.\n");
        return AVERROR(EINVAL);
    }

    av_log(ctx, AV_LOG_DEBUG, "x:%d y:%d w:%d h:%d color:0x%02X%02X%02X%02X\n",
           s->x, s->y, s->w, s->h,
           s->yuv_color[Y], s->yuv_color[U], s->yuv_color[V], s->yuv_color[A]);

    return 0;

    fail:
    av_log(ctx, AV_LOG_ERROR,
           "Error when evaluating the expression '%s' (%d).\n",
           expr, ret);
    return ret;
}
static int config_input(AVFilterLink *inlink)
{
    AVFilterContext *ctx = inlink->dst;
    DrawBoxContext *s = ctx->priv;
    const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(inlink->format);

    s->hsub = desc->log2_chroma_w;
    s->vsub = desc->log2_chroma_h;
    s->have_alpha = desc->flags & AV_PIX_FMT_FLAG_ALPHA;

    return parse_data(inlink);
}

static av_pure av_always_inline int pixel_belongs_to_box(DrawBoxContext *s, int x, int y)
{
    return (y - s->y < s->thickness) || (s->y + s->h - 1 - y < s->thickness) ||
           (x - s->x < s->thickness) || (s->x + s->w - 1 - x < s->thickness);
}

static int filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    DrawBoxContext *s = inlink->dst->priv;

    s->time = frame->pts == AV_NOPTS_VALUE ?
              NAN : frame->pts * av_q2d(inlink->time_base);
    av_log(s, AV_LOG_DEBUG, "pts: %lld time base:%f time: %f\n", frame->pts, av_q2d(inlink->time_base), s->time);
    parse_data(inlink);

    int plane, x, y, xb = s->x, yb = s->y;
    unsigned char *row[4];

    if (s->have_alpha && s->replace) {
        for (y = FFMAX(yb, 0); y < frame->height && y < (yb + s->h); y++) {
            row[0] = frame->data[0] + y * frame->linesize[0];
            row[3] = frame->data[3] + y * frame->linesize[3];

            for (plane = 1; plane < 3; plane++)
                row[plane] = frame->data[plane] +
                     frame->linesize[plane] * (y >> s->vsub);

            if (s->invert_color) {
                for (x = FFMAX(xb, 0); x < xb + s->w && x < frame->width; x++)
                    if (pixel_belongs_to_box(s, x, y))
                        row[0][x] = 0xff - row[0][x];
            } else {
                for (x = FFMAX(xb, 0); x < xb + s->w && x < frame->width; x++) {
                    if (pixel_belongs_to_box(s, x, y)) {
                        row[0][x           ] = s->yuv_color[Y];
                        row[1][x >> s->hsub] = s->yuv_color[U];
                        row[2][x >> s->hsub] = s->yuv_color[V];
                        row[3][x           ] = s->yuv_color[A];
                    }
                }
            }
        }
    } else {
        for (y = FFMAX(yb, 0); y < frame->height && y < (yb + s->h); y++) {
            row[0] = frame->data[0] + y * frame->linesize[0];

            for (plane = 1; plane < 3; plane++)
                row[plane] = frame->data[plane] +
                     frame->linesize[plane] * (y >> s->vsub);

            if (s->invert_color) {
                for (x = FFMAX(xb, 0); x < xb + s->w && x < frame->width; x++)
                    if (pixel_belongs_to_box(s, x, y))
                        row[0][x] = 0xff - row[0][x];
            } else {
                for (x = FFMAX(xb, 0); x < xb + s->w && x < frame->width; x++) {
                    double alpha = (double)s->yuv_color[A] / 255;

                    if (pixel_belongs_to_box(s, x, y)) {
                        row[0][x                 ] = (1 - alpha) * row[0][x                 ] + alpha * s->yuv_color[Y];
                        row[1][x >> s->hsub] = (1 - alpha) * row[1][x >> s->hsub] + alpha * s->yuv_color[U];
                        row[2][x >> s->hsub] = (1 - alpha) * row[2][x >> s->hsub] + alpha * s->yuv_color[V];
                    }
                }
            }
        }
    }

    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

static int process_command(AVFilterContext *ctx, const char *cmd, const char *args, char *res, int res_len, int flags)
{
    AVFilterLink *inlink = ctx->inputs[0];
    DrawBoxContext *s = ctx->priv;
    int old_x = s->x;
    int old_y = s->y;
    int old_w = s->w;
    int old_h = s->h;
    int old_t = s->thickness;
    int old_r = s->replace;
    int ret;

    ret = ff_filter_process_command(ctx, cmd, args, res, res_len, flags);
    if (ret < 0)
        return ret;

    ret = init(ctx);
    if (ret < 0)
        goto end;
    ret = config_input(inlink);
end:
    if (ret < 0) {
        s->x = old_x;
        s->y = old_y;
        s->w = old_w;
        s->h = old_h;
        s->thickness = old_t;
        s->replace = old_r;
    }

    return ret;
}

#define OFFSET(x) offsetof(DrawBoxContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_RUNTIME_PARAM

#if CONFIG_DRAWBOX_FILTER

static const AVOption drawbox_options[] = {
    { "x",         "set horizontal position of the left box edge", OFFSET(x_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "y",         "set vertical position of the top box edge",    OFFSET(y_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "width",     "set width of the box",                         OFFSET(w_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "w",         "set width of the box",                         OFFSET(w_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "height",    "set height of the box",                        OFFSET(h_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "h",         "set height of the box",                        OFFSET(h_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "color",     "set color of the box",                         OFFSET(color_str), AV_OPT_TYPE_STRING, { .str = "black" }, 0, 0, FLAGS },
    { "c",         "set color of the box",                         OFFSET(color_str), AV_OPT_TYPE_STRING, { .str = "black" }, 0, 0, FLAGS },

    { "color_alpha_expr",     "set channel alpha expression",                 OFFSET(color_alpha_expr), AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },

    { "color_red_expr",       "set channel red expression",                   OFFSET(color_red_expr),   AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },
    { "color_green_expr",     "set channel green expression",                 OFFSET(color_green_expr), AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },
    { "color_blue_expr",      "set channel blue expression",                  OFFSET(color_blue_expr),  AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },

    { "color_y_expr",         "set channel Y expression",                     OFFSET(color_y_expr),     AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },
    { "color_u_expr",         "set channel U expression",                     OFFSET(color_u_expr),     AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },
    { "color_v_expr",         "set channel V expression",                     OFFSET(color_v_expr),     AV_OPT_TYPE_STRING, { .str = "" }, 0, 0, FLAGS },

    { "thickness", "set the box thickness",                        OFFSET(t_expr),    AV_OPT_TYPE_STRING, { .str="3" },       0, 0, FLAGS },
    { "t",         "set the box thickness",                        OFFSET(t_expr),    AV_OPT_TYPE_STRING, { .str="3" },       0, 0, FLAGS },
    { "replace",   "replace color & alpha",                        OFFSET(replace),   AV_OPT_TYPE_BOOL,   { .i64=0 },         0,        1,        FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(drawbox);

static const AVFilterPad drawbox_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .config_props   = config_input,
        .filter_frame   = filter_frame,
        .needs_writable = 1,
    },
    { NULL }
};

static const AVFilterPad drawbox_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_drawbox = {
    .name          = "drawbox",
    .description   = NULL_IF_CONFIG_SMALL("Draw a colored box on the input video."),
    .priv_size     = sizeof(DrawBoxContext),
    .priv_class    = &drawbox_class,
    .init          = init,
    .query_formats = query_formats,
    .inputs        = drawbox_inputs,
    .outputs       = drawbox_outputs,
    .process_command = process_command,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
};
#endif /* CONFIG_DRAWBOX_FILTER */

#if CONFIG_DRAWGRID_FILTER
static av_pure av_always_inline int pixel_belongs_to_grid(DrawBoxContext *drawgrid, int x, int y)
{
    // x is horizontal (width) coord,
    // y is vertical (height) coord
    int x_modulo;
    int y_modulo;

    // Abstract from the offset
    x -= drawgrid->x;
    y -= drawgrid->y;

    x_modulo = x % drawgrid->w;
    y_modulo = y % drawgrid->h;

    // If x or y got negative, fix values to preserve logics
    if (x_modulo < 0)
        x_modulo += drawgrid->w;
    if (y_modulo < 0)
        y_modulo += drawgrid->h;

    return x_modulo < drawgrid->thickness  // Belongs to vertical line
        || y_modulo < drawgrid->thickness;  // Belongs to horizontal line
}

static int drawgrid_filter_frame(AVFilterLink *inlink, AVFrame *frame)
{
    DrawBoxContext *drawgrid = inlink->dst->priv;
    int plane, x, y;
    uint8_t *row[4];

    if (drawgrid->have_alpha && drawgrid->replace) {
        for (y = 0; y < frame->height; y++) {
            row[0] = frame->data[0] + y * frame->linesize[0];
            row[3] = frame->data[3] + y * frame->linesize[3];

            for (plane = 1; plane < 3; plane++)
                row[plane] = frame->data[plane] +
                     frame->linesize[plane] * (y >> drawgrid->vsub);

            if (drawgrid->invert_color) {
                for (x = 0; x < frame->width; x++)
                    if (pixel_belongs_to_grid(drawgrid, x, y))
                        row[0][x] = 0xff - row[0][x];
            } else {
                for (x = 0; x < frame->width; x++) {
                    if (pixel_belongs_to_grid(drawgrid, x, y)) {
                        row[0][x                  ] = drawgrid->yuv_color[Y];
                        row[1][x >> drawgrid->hsub] = drawgrid->yuv_color[U];
                        row[2][x >> drawgrid->hsub] = drawgrid->yuv_color[V];
                        row[3][x                  ] = drawgrid->yuv_color[A];
                    }
                }
            }
        }
    } else {
        for (y = 0; y < frame->height; y++) {
            row[0] = frame->data[0] + y * frame->linesize[0];

            for (plane = 1; plane < 3; plane++)
                row[plane] = frame->data[plane] +
                     frame->linesize[plane] * (y >> drawgrid->vsub);

            if (drawgrid->invert_color) {
                for (x = 0; x < frame->width; x++)
                    if (pixel_belongs_to_grid(drawgrid, x, y))
                        row[0][x] = 0xff - row[0][x];
            } else {
                for (x = 0; x < frame->width; x++) {
                    double alpha = (double)drawgrid->yuv_color[A] / 255;

                    if (pixel_belongs_to_grid(drawgrid, x, y)) {
                        row[0][x                  ] = (1 - alpha) * row[0][x                  ] + alpha * drawgrid->yuv_color[Y];
                        row[1][x >> drawgrid->hsub] = (1 - alpha) * row[1][x >> drawgrid->hsub] + alpha * drawgrid->yuv_color[U];
                        row[2][x >> drawgrid->hsub] = (1 - alpha) * row[2][x >> drawgrid->hsub] + alpha * drawgrid->yuv_color[V];
                    }
                }
            }
        }
    }

    return ff_filter_frame(inlink->dst->outputs[0], frame);
}

static const AVOption drawgrid_options[] = {
    { "x",         "set horizontal offset",   OFFSET(x_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "y",         "set vertical offset",     OFFSET(y_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "width",     "set width of grid cell",  OFFSET(w_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "w",         "set width of grid cell",  OFFSET(w_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "height",    "set height of grid cell", OFFSET(h_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "h",         "set height of grid cell", OFFSET(h_expr),    AV_OPT_TYPE_STRING, { .str="0" },       0, 0, FLAGS },
    { "color",     "set color of the grid",   OFFSET(color_str), AV_OPT_TYPE_STRING, { .str = "black" }, 0, 0, FLAGS },
    { "c",         "set color of the grid",   OFFSET(color_str), AV_OPT_TYPE_STRING, { .str = "black" }, 0, 0, FLAGS },
    { "thickness", "set grid line thickness", OFFSET(t_expr),    AV_OPT_TYPE_STRING, {.str="1"},         0, 0, FLAGS },
    { "t",         "set grid line thickness", OFFSET(t_expr),    AV_OPT_TYPE_STRING, {.str="1"},         0, 0, FLAGS },
    { "replace",   "replace color & alpha",   OFFSET(replace),   AV_OPT_TYPE_BOOL,   { .i64=0 },         0,        1,        FLAGS },
    { NULL }
};

AVFILTER_DEFINE_CLASS(drawgrid);

static const AVFilterPad drawgrid_inputs[] = {
    {
        .name           = "default",
        .type           = AVMEDIA_TYPE_VIDEO,
        .config_props   = config_input,
        .filter_frame   = drawgrid_filter_frame,
        .needs_writable = 1,
    },
    { NULL }
};

static const AVFilterPad drawgrid_outputs[] = {
    {
        .name = "default",
        .type = AVMEDIA_TYPE_VIDEO,
    },
    { NULL }
};

AVFilter ff_vf_drawgrid = {
    .name          = "drawgrid",
    .description   = NULL_IF_CONFIG_SMALL("Draw a colored grid on the input video."),
    .priv_size     = sizeof(DrawBoxContext),
    .priv_class    = &drawgrid_class,
    .init          = init,
    .query_formats = query_formats,
    .inputs        = drawgrid_inputs,
    .outputs       = drawgrid_outputs,
    .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC,
    .process_command = process_command,
};

#endif  /* CONFIG_DRAWGRID_FILTER */
