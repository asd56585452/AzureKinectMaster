#pragma once

#include <iostream>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

// Stores a single attachment (e.g. calibration JSON) read from the input MKV.
struct AttachmentData {
    std::string          filename;
    std::string          mimetype;
    std::vector<uint8_t> data;
};

class KinectProcessor {
private:
    AVFormatContext* fmt_ctx = nullptr;
    AVFormatContext* out_fmt_ctx = nullptr;

    int color_idx = -1;
    int depth_idx = -1;

    // Color pipeline
    AVCodecContext* color_dec_ctx = nullptr;
    AVCodecContext* color_enc_ctx = nullptr;
    AVStream* out_color_stream = nullptr;
    SwsContext* color_sws_ctx = nullptr;
    AVRational      color_input_tb = { 0, 0 };

    // Depth pipeline
    AVCodecContext* depth_dec_ctx = nullptr;
    AVCodecContext* depth_enc_ctx = nullptr;
    AVStream* out_depth_stream = nullptr;
    AVRational      depth_input_tb = { 0, 0 };

    // Frame counters
    int64_t color_frame_count = 0;
    int64_t depth_frame_count = 0;

    // Calibration attachments to carry over from input to output.
    std::vector<AttachmentData> attachments;

public:
    KinectProcessor() = default;

    bool Initialize(const char* inputPath);
    bool InitColorEncoder(int qscale);
    bool InitDepthEncoder(int width, int height);
    bool InitMuxer(const char* outputPath);
    void Process();
    bool Finalize(const char* outputPath);

    int GetColorWidth()  const { return color_dec_ctx ? color_dec_ctx->width : 0; }
    int GetColorHeight() const { return color_dec_ctx ? color_dec_ctx->height : 0; }
    int GetDepthWidth()  const { return depth_dec_ctx ? depth_dec_ctx->width : 0; }
    int GetDepthHeight() const { return depth_dec_ctx ? depth_dec_ctx->height : 0; }

private:
    void EncodeAndWrite(AVCodecContext* enc_ctx, AVFrame* frame, AVStream* out_stream);
    void HandleColorEncoding(AVFrame* decoded_frame);
    void HandleDepthEncoding(AVFrame* decoded_frame);
};