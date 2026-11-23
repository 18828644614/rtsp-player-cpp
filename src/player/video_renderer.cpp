#include "player/video_renderer.h"

#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <climits>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace rtsp {
namespace {

void require_sdl(bool result, const char* operation) {
    if (!result) {
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
    }
}

SDL_Texture* create_texture(SDL_Renderer* renderer, const AVFrame& frame) {
    // 明确有限/全范围及 601/709 矩阵，避免把常见电视范围视频当作 JPEG。
    const bool full = frame.color_range == AVCOL_RANGE_JPEG;
    const auto colorspace = frame.colorspace == AVCOL_SPC_BT709
                                ? (full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED)
                                : (full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED);
    const auto properties = SDL_CreateProperties();
    require_sdl(properties != 0, "create texture properties");
    const bool configured =
        SDL_SetNumberProperty(
            properties, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_IYUV
        )
        && SDL_SetNumberProperty(
            properties, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING
        )
        && SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, frame.width)
        && SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, frame.height)
        && SDL_SetNumberProperty(properties, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, colorspace);
    auto* texture = configured ? SDL_CreateTextureWithProperties(renderer, properties) : nullptr;
    SDL_DestroyProperties(properties);
    require_sdl(texture != nullptr, "create YUV texture");
    return texture;
}

}  // namespace

VideoRenderer::VideoRenderer() {
    SDL_SetMainReady();
    try {
        require_sdl(SDL_Init(SDL_INIT_VIDEO), "initialize SDL video");
        window_ = SDL_CreateWindow("RTSP Player", 960, 540, SDL_WINDOW_RESIZABLE);
        require_sdl(window_ != nullptr, "create video window");
        renderer_ = SDL_CreateRenderer(window_, nullptr);
        require_sdl(renderer_ != nullptr, "create video renderer");
        require_sdl(SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255), "set background color");
        redraw();
    } catch (...) {
        close();
        throw;
    }
}

VideoRenderer::~VideoRenderer() {
    close();
}

void VideoRenderer::close() {
    SDL_DestroyTexture(texture_);
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool VideoRenderer::poll_events(int timeout_ms) {
    SDL_Event event;
    bool closing = false;
    if (SDL_WaitEventTimeout(&event, timeout_ms)) {
        do {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                closing = true;
            }
            if (event.type == SDL_EVENT_WINDOW_EXPOSED
                || event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
                dirty_ = true;
            }
        } while (SDL_PollEvent(&event));
    }
    return closing;
}

void VideoRenderer::prepare_texture(const AVFrame& frame) {
    if (frame.format != AV_PIX_FMT_YUV420P || frame.width <= 0 || frame.height <= 0) {
        throw std::runtime_error("P3 video display requires YUV420P frames");
    }
    if (width_ != frame.width || height_ != frame.height || colorspace_ != frame.colorspace
        || color_range_ != frame.color_range) {
        SDL_DestroyTexture(texture_);
        texture_ = nullptr;
        texture_ = create_texture(renderer_, frame);
        require_sdl(SDL_SetTextureScaleMode(texture_, SDL_SCALEMODE_LINEAR), "set texture scaling");
        width_ = frame.width;
        height_ = frame.height;
        colorspace_ = frame.colorspace;
        color_range_ = frame.color_range;
    }
    // 同时考虑像素宽高比；普通方形像素使用视频原始宽高。
    std::int64_t display_width = frame.width;
    if (frame.sample_aspect_ratio.num > 0 && frame.sample_aspect_ratio.den > 0) {
        display_width =
            (display_width * frame.sample_aspect_ratio.num + frame.sample_aspect_ratio.den / 2)
            / frame.sample_aspect_ratio.den;
    }
    if (display_width <= 0 || display_width > INT_MAX) {
        throw std::runtime_error("invalid video display aspect ratio");
    }
    require_sdl(
        SDL_SetRenderLogicalPresentation(
            renderer_, static_cast<int>(display_width), frame.height,
            SDL_LOGICAL_PRESENTATION_LETTERBOX
        ),
        "set video aspect ratio"
    );
}

void VideoRenderer::present(const AVFrame& frame) {
    prepare_texture(frame);
    require_sdl(
        SDL_UpdateYUVTexture(
            texture_, nullptr, frame.data[0], frame.linesize[0], frame.data[1], frame.linesize[1],
            frame.data[2], frame.linesize[2]
        ),
        "upload YUV frame"
    );
    dirty_ = true;
    redraw();
}

void VideoRenderer::redraw() {
    if (!dirty_) {
        return;
    }
    require_sdl(SDL_RenderClear(renderer_), "clear video renderer");
    if (texture_) {
        require_sdl(SDL_RenderTexture(renderer_, texture_, nullptr, nullptr), "draw video texture");
    }
    require_sdl(SDL_RenderPresent(renderer_), "present video frame");
    dirty_ = false;
}

void VideoRenderer::hide() {
    require_sdl(SDL_HideWindow(window_), "hide video window");
}

}  // namespace rtsp
