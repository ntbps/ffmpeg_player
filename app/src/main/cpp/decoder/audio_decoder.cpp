//
// Created by templechen on 2019-05-04.
//

#include <pthread.h>
#include "audio_decoder.h"
#include "../common/native_log.h"

extern "C" {
#include "libswresample/swresample.h"
#include "libavformat/avformat.h"
#include "libavutil/opt.h"
}
#define MAX_AUDIO_FRAME_SIZE 192000

audio_decoder::audio_decoder() {

}

audio_decoder::~audio_decoder() {

}

void audio_decoder::decode(const char *url, circle_av_frame_queue *video_queue, audio_looper *audioLooper) {
    this->url = url;
    this->audio_queue = video_queue;
    this->audioLooper = audioLooper;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create(&worker_thread, &attr, trampoline, this);
}

void *audio_decoder::trampoline(void *p) {
    avformat_network_init();
    const char *url = ((audio_decoder *) p)->url;
    circle_av_frame_queue *audio_queue = ((audio_decoder *) p)->audio_queue;
    audio_looper *audioLooper = ((audio_decoder *) p)->audioLooper;
    //封装格式上下文
    AVFormatContext *formatContext = avformat_alloc_context();
    if (avformat_open_input(&formatContext, url, nullptr, nullptr) < 0) {
        ALOGD("can not open ", url)
        return nullptr;
    }
    //获取信息
    if (avformat_find_stream_info(formatContext, nullptr) < 0) {
        ALOGD("can not find stream info", url)
        return nullptr;
    }
    //获取音频流的索引位置
    int audio_stream_index = -1;
    for (int i = 0; i < formatContext->nb_streams; ++i) {
        if (formatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_index = i;
            break;
        }
    }
    if (audio_stream_index == -1) {
        ALOGD("can not find video stream info")
        return nullptr;
    }
    AVCodecContext *codecContext = avcodec_alloc_context3(nullptr);
    avcodec_parameters_to_context(codecContext, formatContext->streams[audio_stream_index]->codecpar);
    AVCodec *videoCodec = avcodec_find_decoder(codecContext->codec_id);
    //打开解码器
    if (avcodec_open2(codecContext, videoCodec, nullptr) < 0) {
        ALOGD("can not open video decoder")
        return nullptr;
    }

    AVPacket *packet = av_packet_alloc();

    //Out Audio Param
    //AAC:1024  MP3:1152

//    uint64_t out_channel_layout = AV_CH_LAYOUT_STEREO;
//    AVSampleFormat out_sample_fmt = AV_SAMPLE_FMT_S16;
//    int out_sample_rate = 44100;
//    int64_t in_channel_layout = av_get_default_channel_layout(codecContext->channels);
//    SwrContext *swrContext = swr_alloc_set_opts(nullptr, out_channel_layout, out_sample_fmt, out_sample_rate,
//                                                in_channel_layout, codecContext->sample_fmt,
//                                                codecContext->sample_rate, 0,
//                                                nullptr);
    SwrContext *swrContext = swr_alloc();
    av_opt_set_channel_layout(swrContext, "in_channel_layout", av_get_default_channel_layout(codecContext->channels),
                              0);
    av_opt_set_channel_layout(swrContext, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
    av_opt_set_int(swrContext, "in_sample_rate", codecContext->sample_rate, 0);
    av_opt_set_int(swrContext, "out_sample_rate", 44100, 0);
    av_opt_set_sample_fmt(swrContext, "in_sample_fmt", codecContext->sample_fmt, 0);
    av_opt_set_sample_fmt(swrContext, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
    swr_init(swrContext);
    audioLooper->postMessage(audioLooper->kMsgSwrContextInit, swrContext);

    int ret = 0;
    double ratio = av_q2d(formatContext->streams[audio_stream_index]->time_base) * 1000;
    while (true) {
        if (p == nullptr) {
            break;
        }
        if (av_read_frame(formatContext, packet) < 0) {
            ALOGD("read frame end")
            break;
        }
        if (packet->stream_index == audio_stream_index) {
            AVFrame *pFrame = av_frame_alloc();
            avcodec_send_packet(codecContext, packet);
            if (ret < 0 && ret != AVERROR(EAGAIN) && ret != AVERROR_EOF) {
                ALOGD("avcodec send packet ret = %d", ret);
                av_packet_unref(packet);
                continue;
            }
            ret = avcodec_receive_frame(codecContext, pFrame);
            if (ret < 0 && ret != AVERROR_EOF) {
                ALOGD("avcodec receive frame ret = %d", ret);
                av_packet_unref(packet);
                av_frame_unref(pFrame);
                continue;
            }
            if (pFrame->pts == AV_NOPTS_VALUE) {
                pFrame->pts = static_cast<int64_t>(pFrame->best_effort_timestamp * ratio);
            } else {
                pFrame->pts = static_cast<int64_t>(pFrame->pts * ratio);
            }
            int res = audio_queue->push(pFrame);
            __android_log_print(ANDROID_LOG_DEBUG, "audio", " %lld, %d", pFrame->pts, pFrame->nb_samples);
            if (res == -1) {
                av_frame_free(&pFrame);
                break;
            }
            av_packet_unref(packet);
        }
    }
    av_packet_free(&packet);
    avcodec_close(codecContext);
    avformat_close_input(&formatContext);

    __android_log_print(ANDROID_LOG_DEBUG, "audio", " decoder over");
    return nullptr;
}
