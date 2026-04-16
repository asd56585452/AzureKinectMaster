#include <iostream>
#include <string>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "KinectCompressor.hpp"

bool KinectProcessor::Initialize(const char* inputPath) {
    if (avformat_open_input(&fmt_ctx, inputPath, nullptr, nullptr) < 0) {
        std::cerr << "[Error] Cannot open input file: " << inputPath << std::endl;
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx, nullptr) < 0) {
        std::cerr << "[Error] Cannot find stream information." << std::endl;
        return false;
    }

    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        AVStream* stream = fmt_ctx->streams[i];

        // Handle video streams identified by title metadata.
        AVDictionaryEntry* tag = av_dict_get(stream->metadata, "title", nullptr, 0);
        if (tag) {
            std::string title = tag->value;
            const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);

            if (title == "COLOR") {
                color_idx = i;
                color_input_tb = stream->time_base;
                color_dec_ctx = avcodec_alloc_context3(decoder);
                avcodec_parameters_to_context(color_dec_ctx, stream->codecpar);
                avcodec_open2(color_dec_ctx, decoder, nullptr);
                std::cout << "[Found] Color stream at index " << i << std::endl;
            }
            else if (title == "DEPTH") {
                depth_idx = i;
                depth_input_tb = stream->time_base;
                depth_dec_ctx = avcodec_alloc_context3(decoder);
                avcodec_parameters_to_context(depth_dec_ctx, stream->codecpar);
                avcodec_open2(depth_dec_ctx, decoder, nullptr);
                std::cout << "[Found] Depth stream at index " << i << std::endl;
            }
        }

        // Collect attachment streams (calibration JSON is stored here).
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_ATTACHMENT &&
            stream->codecpar->extradata && stream->codecpar->extradata_size > 0)
        {
            AttachmentData att;
            AVDictionaryEntry* fname = av_dict_get(stream->metadata, "filename", nullptr, 0);
            AVDictionaryEntry* mime = av_dict_get(stream->metadata, "mimetype", nullptr, 0);
            att.filename = fname ? fname->value : ("attachment_" + std::to_string(i));
            att.mimetype = mime ? mime->value : "application/octet-stream";
            att.data.assign(stream->codecpar->extradata,
                stream->codecpar->extradata + stream->codecpar->extradata_size);
            attachments.push_back(att);
            std::cout << "[Found] Attachment: " << att.filename
                << " (" << att.data.size() << " bytes)" << std::endl;
        }
    }

    if (attachments.empty())
        std::cout << "[Warning] No attachments found �X calibration data will NOT be preserved." << std::endl;

    return true;
}

bool KinectProcessor::InitMuxer(const char* outputPath) {
    avformat_alloc_output_context2(&out_fmt_ctx, nullptr, "matroska", outputPath);
    if (!out_fmt_ctx) {
        std::cerr << "[Error] Failed to create output format context." << std::endl;
        return false;
    }

    // --- Color stream ---
    out_color_stream = avformat_new_stream(out_fmt_ctx, nullptr);
    if (!out_color_stream) {
        std::cerr << "[Error] Failed to create color output stream." << std::endl;
        return false;
    }
    avcodec_parameters_from_context(out_color_stream->codecpar, color_enc_ctx);
    out_color_stream->codecpar->codec_tag = MKTAG('M', 'J', 'P', 'G');
    out_color_stream->time_base = color_input_tb;
    av_dict_set(&out_color_stream->metadata, "title", "COLOR", 0);

    // --- Depth stream ---
    out_depth_stream = avformat_new_stream(out_fmt_ctx, nullptr);
    if (!out_depth_stream) {
        std::cerr << "[Error] Failed to create depth output stream." << std::endl;
        return false;
    }
    avcodec_parameters_from_context(out_depth_stream->codecpar, depth_enc_ctx);
    out_depth_stream->codecpar->codec_tag = 0;
    out_depth_stream->time_base = depth_input_tb;
    av_dict_set(&out_depth_stream->metadata, "title", "DEPTH", 0);

    // --- Attachment streams (calibration data) ---
    for (const auto& att : attachments) {
        AVStream* att_stream = avformat_new_stream(out_fmt_ctx, nullptr);
        if (!att_stream) {
            std::cerr << "[Warning] Failed to create attachment stream: " << att.filename << std::endl;
            continue;
        }
        att_stream->codecpar->codec_type = AVMEDIA_TYPE_ATTACHMENT;
        att_stream->codecpar->codec_id = AV_CODEC_ID_NONE;
        att_stream->codecpar->extradata = (uint8_t*)av_malloc(att.data.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        att_stream->codecpar->extradata_size = (int)att.data.size();
        memcpy(att_stream->codecpar->extradata, att.data.data(), att.data.size());
        memset(att_stream->codecpar->extradata + att.data.size(), 0, AV_INPUT_BUFFER_PADDING_SIZE);
        av_dict_set(&att_stream->metadata, "filename", att.filename.c_str(), 0);
        av_dict_set(&att_stream->metadata, "mimetype", att.mimetype.c_str(), 0);
        std::cout << "[Info] Preserved attachment: " << att.filename << std::endl;
    }

    // --- Open output and write header ---
    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt_ctx->pb, outputPath, AVIO_FLAG_WRITE) < 0) {
            std::cerr << "[Error] Failed to open output file for writing." << std::endl;
            return false;
        }
    }

    int ret = avformat_write_header(out_fmt_ctx, nullptr);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        std::cerr << "[Error] avformat_write_header failed: " << errbuf << std::endl;
        return false;
    }

    return true;
}

void KinectProcessor::Process() {
    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!pkt || !frame) {
        std::cerr << "[Error] Failed to allocate packet or frame." << std::endl;
        return;
    }

    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == color_idx && color_dec_ctx) {
            if (avcodec_send_packet(color_dec_ctx, pkt) >= 0)
                while (avcodec_receive_frame(color_dec_ctx, frame) >= 0)
                    HandleColorEncoding(frame);
        }
        else if (pkt->stream_index == depth_idx && depth_dec_ctx) {
            if (avcodec_send_packet(depth_dec_ctx, pkt) >= 0)
                while (avcodec_receive_frame(depth_dec_ctx, frame) >= 0)
                    HandleDepthEncoding(frame);
        }
        av_packet_unref(pkt);
        if ((color_frame_count + depth_frame_count) % 30 == 0) {
            std::cout << "[Progress] color=" << color_frame_count
                << " depth=" << depth_frame_count << std::endl;
        }
    }

    // Flush decoders.
    if (color_dec_ctx) {
        avcodec_send_packet(color_dec_ctx, nullptr);
        while (avcodec_receive_frame(color_dec_ctx, frame) >= 0) HandleColorEncoding(frame);
    }
    if (depth_dec_ctx) {
        avcodec_send_packet(depth_dec_ctx, nullptr);
        while (avcodec_receive_frame(depth_dec_ctx, frame) >= 0) HandleDepthEncoding(frame);
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);

    std::cout << "[Info] Processed " << color_frame_count << " color frames, "
        << depth_frame_count << " depth frames." << std::endl;
}

void KinectProcessor::EncodeAndWrite(AVCodecContext* enc_ctx, AVFrame* frame, AVStream* out_stream) {
    AVPacket* pkt = av_packet_alloc();

    if (avcodec_send_frame(enc_ctx, frame) < 0) {
        av_packet_free(&pkt);
        return;
    }

    int ret;
    while ((ret = avcodec_receive_packet(enc_ctx, pkt)) >= 0) {
        av_packet_rescale_ts(pkt, enc_ctx->time_base, out_stream->time_base);
        pkt->stream_index = out_stream->index;
        if (av_interleaved_write_frame(out_fmt_ctx, pkt) < 0)
            std::cerr << "[Error] Failed to write packet." << std::endl;
        av_packet_unref(pkt);
    }

    av_packet_free(&pkt);
}

bool KinectProcessor::Finalize(const char* outputPath) {
    // Flush encoders.
    if (color_enc_ctx) EncodeAndWrite(color_enc_ctx, nullptr, out_color_stream);
    if (depth_enc_ctx) EncodeAndWrite(depth_enc_ctx, nullptr, out_depth_stream);

    if (out_fmt_ctx) av_write_trailer(out_fmt_ctx);

    if (color_sws_ctx) { sws_freeContext(color_sws_ctx); color_sws_ctx = nullptr; }
    if (color_enc_ctx) avcodec_free_context(&color_enc_ctx);
    if (depth_enc_ctx) avcodec_free_context(&depth_enc_ctx);
    if (color_dec_ctx) avcodec_free_context(&color_dec_ctx);
    if (depth_dec_ctx) avcodec_free_context(&depth_dec_ctx);

    if (out_fmt_ctx && !(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&out_fmt_ctx->pb);
    if (out_fmt_ctx) avformat_free_context(out_fmt_ctx);
    if (fmt_ctx)     avformat_close_input(&fmt_ctx);

    std::cout << "[Success] Output written: " << outputPath << std::endl;
    return true;
}

// --- Depth encoder ---

bool KinectProcessor::InitDepthEncoder(int width, int height) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_FFV1);
    if (!codec) {
        std::cerr << "[Error] FFV1 encoder not found." << std::endl;
        return false;
    }

    depth_enc_ctx = avcodec_alloc_context3(codec);
    depth_enc_ctx->width = width;
    depth_enc_ctx->height = height;
    depth_enc_ctx->time_base = depth_input_tb;
    depth_enc_ctx->pix_fmt = AV_PIX_FMT_GRAY16LE; // K4A depth is always little-endian.
    depth_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    depth_enc_ctx->level = 3;
    av_opt_set(depth_enc_ctx->priv_data, "slices", "4", 0);

    if (avcodec_open2(depth_enc_ctx, codec, nullptr) < 0) {
        std::cerr << "[Error] Failed to open depth encoder." << std::endl;
        return false;
    }
    return true;
}

void KinectProcessor::HandleDepthEncoding(AVFrame* decoded_frame) {
    AVFrame* depth_frame = av_frame_alloc();
    depth_frame->format = depth_enc_ctx->pix_fmt;   // GRAY16LE
    depth_frame->width = decoded_frame->width;
    depth_frame->height = decoded_frame->height;
    if (av_frame_get_buffer(depth_frame, 32) < 0) {
        std::cerr << "[Error] Depth av_frame_get_buffer failed." << std::endl;
        av_frame_free(&depth_frame);
        return;
    }

    AVPixelFormat src_fmt = (AVPixelFormat)decoded_frame->format;
    int w = decoded_frame->width;
    int h = decoded_frame->height;

    if (src_fmt == AV_PIX_FMT_GRAY16BE) {
        // Byte-swap BE -> LE
        for (int y = 0; y < h; y++) {
            const uint8_t* src = decoded_frame->data[0] + y * decoded_frame->linesize[0];
            uint8_t* dst = depth_frame->data[0] + y * depth_frame->linesize[0];
            for (int x = 0; x < w; x++) {
                dst[2 * x] = src[2 * x + 1];
                dst[2 * x + 1] = src[2 * x];
            }
        }
    }
    else if (src_fmt == AV_PIX_FMT_GRAY16LE) {
        // Already LE, just copy
        for (int y = 0; y < h; y++) {
            memcpy(depth_frame->data[0] + y * depth_frame->linesize[0],
                decoded_frame->data[0] + y * decoded_frame->linesize[0],
                w * 2);
        }
    }
    else {
        std::cerr << "[Error] Unexpected depth pixel format: " << src_fmt << std::endl;
        av_frame_free(&depth_frame);
        return;
    }

    depth_frame->pts = decoded_frame->pts;
    EncodeAndWrite(depth_enc_ctx, depth_frame, out_depth_stream);
    depth_frame_count++;

    av_frame_free(&depth_frame);
}

// --- Color encoder ---

bool KinectProcessor::InitColorEncoder(int qscale) {
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        std::cerr << "[Error] MJPEG encoder not found." << std::endl;
        return false;
    }

    color_enc_ctx = avcodec_alloc_context3(codec);
    color_enc_ctx->width = color_dec_ctx->width;
    color_enc_ctx->height = color_dec_ctx->height;
    color_enc_ctx->time_base = color_input_tb;
    color_enc_ctx->pix_fmt = AV_PIX_FMT_YUVJ422P;
    color_enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER | AV_CODEC_FLAG_QSCALE;
    color_enc_ctx->global_quality = FF_QP2LAMBDA * qscale;

    if (avcodec_open2(color_enc_ctx, codec, nullptr) < 0) {
        std::cerr << "[Error] Failed to open MJPEG encoder." << std::endl;
        return false;
    }
    return true;
}

void KinectProcessor::HandleColorEncoding(AVFrame* decoded_frame) {
    if (!color_sws_ctx) {
        color_sws_ctx = sws_getContext(
            decoded_frame->width, decoded_frame->height, (AVPixelFormat)decoded_frame->format,
            color_enc_ctx->width, color_enc_ctx->height, color_enc_ctx->pix_fmt,
            SWS_BICUBIC, nullptr, nullptr, nullptr);

        // Preserve full-range color (pc levels) from source.
        int* inv_table, * table, srcRange, dstRange, brightness, contrast, saturation;
        sws_getColorspaceDetails(color_sws_ctx, &inv_table, &srcRange, &table,
            &dstRange, &brightness, &contrast, &saturation);
        sws_setColorspaceDetails(color_sws_ctx,
            sws_getCoefficients(SWS_CS_DEFAULT), 1,
            sws_getCoefficients(SWS_CS_DEFAULT), 1,
            brightness, contrast, saturation);
    }

    AVFrame* enc_frame = av_frame_alloc();
    enc_frame->format = color_enc_ctx->pix_fmt;
    enc_frame->width = color_enc_ctx->width;
    enc_frame->height = color_enc_ctx->height;
    av_frame_get_buffer(enc_frame, 32);

    sws_scale(color_sws_ctx, decoded_frame->data, decoded_frame->linesize,
        0, decoded_frame->height, enc_frame->data, enc_frame->linesize);

    enc_frame->pts = decoded_frame->pts;
    EncodeAndWrite(color_enc_ctx, enc_frame, out_color_stream);
    color_frame_count++;

    av_frame_free(&enc_frame);
}