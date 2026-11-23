// 仅用于回环测试：定时注入 SDL 关闭事件或外部停止，不给正式程序增加测试参数。
#include "player/player.h"
#define SDL_MAIN_HANDLED
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

Uint32 SDLCALL close_window(void*, SDL_TimerID, Uint32) {
    SDL_Event event{};
    event.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    SDL_PushEvent(&event);
    return 0;
}

int main(int argc, char* argv[]) {
    try {
        if (argc != 4) {
            throw std::runtime_error("expected URL, delay-ms, event|stop");
        }
        const auto delay = std::chrono::milliseconds(std::stoi(argv[2]));
        const auto started = std::chrono::steady_clock::now();
        const bool event = std::string(argv[3]) == "event";
        SDL_SetMainReady();
        if (!SDL_Init(0)
            || (event
                && !SDL_AddTimer(static_cast<Uint32>(delay.count()), close_window, nullptr))) {
            throw std::runtime_error(SDL_GetError());
        }
        rtsp::ClientOptions options;
        options.timeout_ms = 600000;  // 关闭不能被十分钟超时拖住。
        rtsp::Player player(options, [=] {
            return !event && std::chrono::steady_clock::now() - started >= delay;
        });
        player.run(argv[1]);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[ERROR] " << error.what() << '\n';
        return 1;
    }
}
