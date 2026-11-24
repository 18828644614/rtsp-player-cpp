#include "player/video_renderer.h"

#include <SDL3/SDL.h>
extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

void check(bool value, const char* message) {
    if (!value) {
        throw std::runtime_error(message);
    }
}

void free_frame(AVFrame* frame) {
    av_frame_free(&frame);
}

void fill_frame(AVFrame& frame, int width, int height) {
    av_frame_unref(&frame);
    frame.format = AV_PIX_FMT_YUV420P;
    frame.width = width;
    frame.height = height;
    check(av_frame_get_buffer(&frame, 32) == 0, "cannot allocate test frame");
    for (int y = 0; y < height; ++y) {
        std::memset(frame.data[0] + y * frame.linesize[0], 235, static_cast<std::size_t>(width));
    }
    for (int y = 0; y < (height + 1) / 2; ++y) {
        std::memset(
            frame.data[1] + y * frame.linesize[1], 128, static_cast<std::size_t>((width + 1) / 2)
        );
        std::memset(
            frame.data[2] + y * frame.linesize[2], 128, static_cast<std::size_t>((width + 1) / 2)
        );
    }
}

void test_renderer(const char* snapshot) {
    rtsp::VideoRenderer display;
    int count = 0;
    auto** windows = SDL_GetWindows(&count);
    check(windows && count == 1, "expected one SDL window");
    auto* window = windows[0];
    SDL_free(windows);
    auto* renderer = SDL_GetRenderer(window);
    check(renderer != nullptr, "missing SDL renderer");
    std::unique_ptr<AVFrame, decltype(&free_frame)> frame(av_frame_alloc(), free_frame);
    check(frame != nullptr, "cannot allocate frame");
    fill_frame(*frame, 64, 48);
    display.present(*frame);

    // 软件渲染器在 present 后仍保留像素，可检查上传结果与黑边并输出 QA 图片。
    // 读取整个窗口，避免读回结果被逻辑视口裁成只有视频内容。
    check(
        SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED),
        "cannot select full window for readback"
    );
    auto* pixels = SDL_RenderReadPixels(renderer, nullptr);
    check(pixels != nullptr, "cannot read rendered pixels");
    Uint8 r, g, b, a;
    check(
        SDL_ReadSurfacePixel(pixels, pixels->w / 2, pixels->h / 2, &r, &g, &b, &a) && r > 240
            && g > 240 && b > 240,
        "YUV upload did not produce white pixels"
    );
    check(
        SDL_ReadSurfacePixel(pixels, 1, pixels->h / 2, &r, &g, &b, &a) && r < 5 && g < 5 && b < 5,
        "letterbox border is not black"
    );
    SDL_DestroySurface(pixels);

    // 红/蓝两块检查 U/V 顺序、有限范围和矩阵选择。
    frame->colorspace = AVCOL_SPC_SMPTE170M;
    frame->color_range = AVCOL_RANGE_MPEG;
    for (int y = 0; y < 48; ++y) {
        std::memset(frame->data[0] + y * frame->linesize[0], 81, 32);
        std::memset(frame->data[0] + y * frame->linesize[0] + 32, 41, 32);
    }
    for (int y = 0; y < 24; ++y) {
        std::memset(frame->data[1] + y * frame->linesize[1], 90, 16);
        std::memset(frame->data[1] + y * frame->linesize[1] + 16, 240, 16);
        std::memset(frame->data[2] + y * frame->linesize[2], 240, 16);
        std::memset(frame->data[2] + y * frame->linesize[2] + 16, 110, 16);
    }
    display.present(*frame);
    check(
        SDL_SetRenderLogicalPresentation(renderer, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED),
        "cannot select full window for color readback"
    );
    pixels = SDL_RenderReadPixels(renderer, nullptr);
    check(pixels != nullptr, "cannot read color pixels");
    check(
        SDL_ReadSurfacePixel(pixels, pixels->w / 3, pixels->h / 2, &r, &g, &b, &a) && r > 230
            && g < 20 && b < 20,
        "red YUV plane/matrix conversion failed"
    );
    check(
        SDL_ReadSurfacePixel(pixels, pixels->w * 2 / 3, pixels->h / 2, &r, &g, &b, &a) && b > 230
            && r < 20 && g < 20,
        "blue YUV plane/matrix conversion failed"
    );
    if (snapshot) {
        check(SDL_SaveBMP(pixels, snapshot), "cannot save render QA image");
    }
    SDL_DestroySurface(pixels);

    check(SDL_SetWindowSize(window, 800, 600), "cannot resize window");
    display.poll_events(0);
    fill_frame(*frame, 128, 64);  // 动态尺寸触发纹理重建。
    frame->sample_aspect_ratio = {2, 1};
    display.present(*frame);
    int logical_width, logical_height;
    SDL_RendererLogicalPresentation mode;
    check(
        SDL_GetRenderLogicalPresentation(renderer, &logical_width, &logical_height, &mode)
            && logical_width == 256 && logical_height == 64
            && mode == SDL_LOGICAL_PRESENTATION_LETTERBOX,
        "resolution/SAR/aspect ratio update failed"
    );
    frame->format = AV_PIX_FMT_NV12;
    bool rejected = false;
    try {
        display.present(*frame);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    check(rejected, "unsupported pixel format was silently accepted");
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    check(SDL_PushEvent(&event) && display.poll_events(0), "window close event not handled");
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        test_renderer(argc > 1 ? argv[1] : nullptr);
        std::cout << "PASS: YUV pixels, letterbox, resize, SAR and close event\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << ": " << SDL_GetError() << '\n';
        return 1;
    }
}
