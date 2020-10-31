/*
 * Copyright (C) 2020  Joshua Minter
 *
 * This file is part of ReplaySorcery.
 *
 * ReplaySorcery is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ReplaySorcery is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with ReplaySorcery.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "ffenc.h"
#include "../util.h"
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/bprint.h>

typedef struct FFmpegEncoder {
   RSEncoder encoder;
   RSDevice *input;
   AVCodecContext *codecCtx;
   AVFilterGraph *filterGraph;
   AVFilterContext *filterSrc;
   AVFilterContext *filterSink;
   AVFrame *frame;
   AVDictionary *options;
   int error;
} FFmpegEncoder;

static void ffmpegEncoderDestroy(RSEncoder *encoder) {
   FFmpegEncoder *ffmpeg = (FFmpegEncoder *)encoder;
   rsOptionsDestroy(&ffmpeg->options);
   av_frame_free(&ffmpeg->frame);
   avfilter_graph_free(&ffmpeg->filterGraph);
   avcodec_parameters_free(&ffmpeg->encoder.params);
   avcodec_free_context(&ffmpeg->codecCtx);
}

static int ffmpegEncoderGetPacket(RSEncoder *encoder, AVPacket *packet) {
   int ret;
   FFmpegEncoder *ffmpeg = (FFmpegEncoder *)encoder;
   while ((ret = avcodec_receive_packet(ffmpeg->codecCtx, packet)) == AVERROR(EAGAIN)) {
      if ((ret = rsDeviceGetFrame(ffmpeg->input, ffmpeg->frame)) < 0) {
         return ret;
      }
      if ((ret = av_buffersrc_add_frame(ffmpeg->filterSrc, ffmpeg->frame)) < 0) {
         av_log(ffmpeg->filterGraph, AV_LOG_ERROR,
                "Failed to send frame to filter graph: %s\n", av_err2str(ret));
         return ret;
      }
      if ((ret = av_buffersink_get_frame(ffmpeg->filterSink, ffmpeg->frame)) < 0) {
         av_log(ffmpeg->filterGraph, AV_LOG_ERROR,
                "Failed to receive frame from filter graph: %s\n", av_err2str(ret));
         return ret;
      }

      ffmpeg->frame->pict_type = AV_PICTURE_TYPE_NONE;
      ret = avcodec_send_frame(ffmpeg->codecCtx, ffmpeg->frame);
      av_frame_unref(ffmpeg->frame);
      if (ret < 0) {
         av_log(ffmpeg->codecCtx, AV_LOG_ERROR, "Failed to send frame to encoder: %s\n",
                av_err2str(ret));
         return ret;
      }
   }

   if (ret < 0) {
      av_log(ffmpeg->codecCtx, AV_LOG_ERROR,
             "Failed to receive packet from encoder: %s\n", av_err2str(ret));
      return ret;
   }
   return 0;
}

int rsFFmpegEncoderCreate(RSEncoder **encoder, const char *name, RSDevice *input) {
   int ret;
   FFmpegEncoder *ffmpeg = av_mallocz(sizeof(FFmpegEncoder));
   *encoder = &ffmpeg->encoder;
   if (ffmpeg == NULL) {
      ret = AVERROR(ENOMEM);
      goto error;
   }

   ffmpeg->encoder.destroy = ffmpegEncoderDestroy;
   ffmpeg->encoder.getPacket = ffmpegEncoderGetPacket;
   ffmpeg->input = input;
   AVCodec *codec = avcodec_find_encoder_by_name(name);
   if (codec == NULL) {
      av_log(NULL, AV_LOG_ERROR, "Encoder not found: %s\n", name);
      ret = AVERROR_ENCODER_NOT_FOUND;
      goto error;
   }

   ffmpeg->codecCtx = avcodec_alloc_context3(codec);
   if (ffmpeg->codecCtx == NULL) {
      ret = AVERROR(ENOMEM);
      goto error;
   }
   if ((ret = avcodec_parameters_to_context(ffmpeg->codecCtx, input->params)) < 0) {
      goto error;
   }
   ffmpeg->codecCtx->codec_id = codec->id;
   ffmpeg->codecCtx->time_base = input->timebase;

   ffmpeg->encoder.params = avcodec_parameters_alloc();
   if (ffmpeg->encoder.params == NULL) {
      ret = AVERROR(ENOMEM);
      goto error;
   }
   ffmpeg->filterGraph = avfilter_graph_alloc();
   if (ffmpeg->filterGraph == NULL) {
      ret = AVERROR(ENOMEM);
      goto error;
   }
   ffmpeg->frame = av_frame_alloc();
   if (ffmpeg->frame == NULL) {
      ret = AVERROR(ENOMEM);
      goto error;
   }

   return 0;
error:
   rsEncoderDestroy(encoder);
   return ret;
}

void rsFFmpegEncoderOption(RSEncoder *encoder, const char *key, const char *fmt, ...) {
   FFmpegEncoder *ffmpeg = (FFmpegEncoder *)encoder;
   va_list args;
   va_start(args, fmt);
   rsOptionsSetv(&ffmpeg->options, &ffmpeg->error, key, fmt, args);
   va_end(args);
}

AVCodecContext *rsFFmpegEncoderGetContext(RSEncoder *encoder) {
   FFmpegEncoder *ffmpeg = (FFmpegEncoder *)encoder;
   return ffmpeg->codecCtx;
}

int rsFFmpegEncoderOpen(RSEncoder *encoder, const char *filter, ...) {
   int ret;
   FFmpegEncoder *ffmpeg = (FFmpegEncoder *)encoder;
   AVBPrint buffer;
   av_bprint_init(&buffer, 0, AV_BPRINT_SIZE_UNLIMITED);
   char *filterDesc = NULL;
   if (ffmpeg->error < 0) {
      ret = ffmpeg->error;
      goto error;
   }
   if ((ret = avcodec_open2(ffmpeg->codecCtx, NULL, &ffmpeg->options)) < 0) {
      av_log(ffmpeg->codecCtx, AV_LOG_ERROR, "Failed to open decoder: %s\n",
             av_err2str(ret));
      return ret;
   }
   rsOptionsDestroy(&ffmpeg->options);

   if ((ret = avcodec_parameters_from_context(ffmpeg->encoder.params, ffmpeg->codecCtx)) <
       0) {
      return ret;
   }
   ffmpeg->encoder.timebase = ffmpeg->codecCtx->time_base;

   AVCodecParameters *params = ffmpeg->input->params;
   AVRational timebase = ffmpeg->input->timebase;
   switch (params->codec_type) {
   case AVMEDIA_TYPE_VIDEO:
      av_bprintf(&buffer, "buffer@src=video_size=%ix%i:pix_fmt=%i:time_base=%i/%i,",
                 params->width, params->height, params->format, timebase.num,
                 timebase.den);
      break;
   default:
      break;
   }

   va_list args;
   va_start(args, filter);
   av_vbprintf(&buffer, filter, args);
   va_end(args);
   AVCodecContext *codecCtx = ffmpeg->codecCtx;
   switch (codecCtx->codec_type) {
   case AVMEDIA_TYPE_VIDEO:
      av_bprintf(&buffer, ",buffersink@sink");
      break;
   default:
      break;
   }

   if (!av_bprint_is_complete(&buffer)) {
      ret = AVERROR(ENOMEM);
      goto error;
   }
   if ((ret = av_bprint_finalize(&buffer, &filterDesc)) < 0) {
      goto error;
   }

   AVFilterInOut *inputs;
   AVFilterInOut *outputs;
   if ((ret = avfilter_graph_parse2(ffmpeg->filterGraph, filterDesc, &inputs, &outputs)) <
       0) {
      av_log(ffmpeg->filterGraph, AV_LOG_ERROR, "Failed to parse filter graph: %s\n",
             av_err2str(ret));
      goto error;
   }
   avfilter_inout_free(&inputs);
   avfilter_inout_free(&outputs);

   if ((ret = avfilter_graph_config(ffmpeg->filterGraph, ffmpeg->filterGraph)) < 0) {
      av_log(ffmpeg->filterGraph, AV_LOG_ERROR, "Failed to configure filter graph: %s\n",
             av_err2str(ret));
      goto error;
   }

   av_freep(&filterDesc);
   ffmpeg->filterSrc = avfilter_graph_get_filter(ffmpeg->filterGraph, "buffer@src");
   ffmpeg->filterSink = avfilter_graph_get_filter(ffmpeg->filterGraph, "buffersink@sink");

   return 0;
error:
   av_freep(&filterDesc);
   av_bprint_finalize(&buffer, NULL);
   return ret;
}
