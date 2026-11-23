#pragma once

struct AVFrame;
struct SDL_Window;
struct SDL_Renderer;
struct SDL_Texture;

namespace rtsp {

// 所有方法都在主线程调用。资源的创建、使用和销毁放在同一个对象中。
class VideoRenderer {
public:
    VideoRenderer();
    ~VideoRenderer();
    VideoRenderer(const VideoRenderer&) = delete;
    VideoRenderer& operator=(const VideoRenderer&) = delete;

    bool poll_events(int timeout_ms = 10);  // true 表示关闭窗口。
    void present(const AVFrame& frame);
    void redraw();
    void hide();

private:
    void prepare_texture(const AVFrame& frame);
    void close();

    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* texture_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int colorspace_ = -1;
    int color_range_ = -1;
    bool dirty_ = true;
};

}  // namespace rtsp
