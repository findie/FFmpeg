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
#include "avfilter.h"
#include "formats.h"
#include "internal.h"
#include "video.h"
#include <stdatomic.h>

#define ABSDIFF(a, b) (abs((int)(a)-(int)(b)))

typedef struct FrameChangeContext {
    const AVClass *

    class;

    int threshold;
    int show;

    AVFrame *frame_prev;
    unsigned int frame_nr;

    int count_mode;
} FrameChangeContext;

enum {
    COUNT_MODE_ABSOLUTE,
    COUNT_MODE_PERCENTAGE
};

#define OFFSET(x) offsetof(FrameChangeContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM
static const AVOption framechange_options[] = {
        {"threshold",  "threshold after which a pixel counts as change",                      OFFSET(threshold),  AV_OPT_TYPE_INT,   {.i64 = 10},                    0, 255,
                FLAGS},
        {"show",       "show changes",                                                        OFFSET(show),       AV_OPT_TYPE_BOOL,  {.i64 =  0},                    0, 1,
                FLAGS},

        {"mode",       "how to count changes",                                                OFFSET(count_mode), AV_OPT_TYPE_INT,   {.i64 =  COUNT_MODE_ABSOLUTE},  0, 1,
                FLAGS, "mode"},
        {"absolute",   "count pixel change above threshold as 1, below as 0",                 OFFSET(count_mode), AV_OPT_TYPE_CONST, {.i64 = COUNT_MODE_ABSOLUTE},   0, 0,
                FLAGS, "mode"},
        {"percentage", "count pixel change above threshold as ABS(change) / 255, below as 0", OFFSET(count_mode), AV_OPT_TYPE_CONST, {.i64 = COUNT_MODE_PERCENTAGE}, 0, 0,
                FLAGS, "mode"},
        {NULL}
};

AVFILTER_DEFINE_CLASS(framechange);

static av_cold int init(AVFilterContext *ctx) {
    FrameChangeContext *framechange = ctx->priv;

    framechange->frame_nr = 0;

    return 0;
}

static int query_formats(AVFilterContext *ctx) {
    static const enum AVPixelFormat pix_fmts[] = {
            AV_PIX_FMT_YUV420P,
            AV_PIX_FMT_GRAY8,
            AV_PIX_FMT_NONE
    };

    AVFilterFormats * fmts_list = ff_make_format_list(pix_fmts);
    if (!fmts_list)
        return AVERROR(ENOMEM);
    return ff_set_common_formats(ctx, fmts_list);
}

static int config_props(AVFilterLink *inlink) {
//    AVFilterContext *ctx = inlink->dst;
//    FrameChangeContext *framechange = ctx->priv;

    return 0;
}

static int filter_slice(AVFilterContext *ctx, uint8_t *a, uint8_t *b, int width, int height, int linesize) {
    FrameChangeContext *s = ctx->priv;

    const int threshold = s->threshold;
    const int show = s->show;
    const int count_mode = s->count_mode;

    const int stride = linesize;

    int x, y;

    uint64_t change_count = 0;

    uint8_t *line_a = a;
    uint8_t *line_b = b;

    // fixme split functions based on count_mode
    if (count_mode == COUNT_MODE_ABSOLUTE) {

        for (y = 0; y < height; y++) {

            for (x = 0; x < width; x++) {
                int change = ABSDIFF(line_a[x], line_b[x]);
                int changed = (change > threshold);

                if (changed) {
                    change_count += 255;
                }

                if (show) {
                    line_a[x] = changed ? 255 : 0;
                }
            }

            line_a += stride;
            line_b += stride;
        }

    } else if (count_mode == COUNT_MODE_PERCENTAGE) {

        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++) {
                int change = ABSDIFF(line_a[x], line_b[x]);
                int changed = (change > threshold);

                if (changed) {
                    change_count += change;
                }

                if (show) {
                    line_a[x] = changed ? change : 0;
                }
            }

            line_a += stride;
            line_b += stride;
        }
    }

    return change_count;
}

static int filter_frame(AVFilterLink *inlink, AVFrame *in) {
    AVFilterContext *ctx = inlink->dst;
    FrameChangeContext *framechange = ctx->priv;
    AVFilterLink *outlink = ctx->outputs[0];

    unsigned int frame_nr = framechange->frame_nr++;
    const int show = framechange->show;

    // if we will mutate the in by showing changes
    // make a copy for later use as previous frame
    AVFrame *original_in = show ? ff_get_video_buffer(ctx->outputs[0], in->width, in->height) : NULL;
    if (show) {
        av_frame_copy(original_in, in);
    }

    if (framechange->frame_prev) {

        uint64_t change = filter_slice(
                ctx,
                in->data[0], framechange->frame_prev->data[0],
                in->width, in->height, in->linesize[0]);

        double perc = change / 255.0 / (in->width * in->height);

        av_log(framechange, AV_LOG_INFO, "frame: %d change: %f\n",
               frame_nr,
               perc
        );

    }

    if (framechange->frame_prev)
        av_frame_free(&framechange->frame_prev);

    // if frame was altered, keep the original as previous
    // else clone the in image
    framechange->frame_prev = show ? original_in : av_frame_clone(in);

    return ff_filter_frame(outlink, in);
}

static av_cold void uninit(AVFilterContext *ctx) {
    FrameChangeContext *framechange = ctx->priv;

    av_frame_free(&framechange->frame_prev);
}

static const AVFilterPad framechange_inputs[] = {
        {
                .name         = "default",
                .type         = AVMEDIA_TYPE_VIDEO,
                .config_props = config_props,
                .filter_frame = filter_frame,
        },
        {NULL}
};

static const AVFilterPad framechange_outputs[] = {
        {
                .name = "default",
                .type = AVMEDIA_TYPE_VIDEO,
        },
        {NULL}
};

AVFilter ff_vf_framechange = {
        .name          = "framechange",
        .description   = NULL_IF_CONFIG_SMALL("Count percentage of pixel changes."),
        .priv_size     = sizeof(FrameChangeContext),
        .init          = init,
        .uninit        = uninit,
        .query_formats = query_formats,
        .inputs        = framechange_inputs,
        .outputs       = framechange_outputs,
        .priv_class    = &framechange_class,
        .flags         = AVFILTER_FLAG_SLICE_THREADS,
};
