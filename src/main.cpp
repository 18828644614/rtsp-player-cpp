#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        std::cout << "rtsp_player: project scaffold\n"
                     "RTSP transport is not implemented yet. See DESIGN.md.\n";
        return 0;
    }
    std::cerr << "Playback is not implemented in this scaffold. Use --help.\n";
    return 2;
}
