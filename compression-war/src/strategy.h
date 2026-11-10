#pragma once
#include <string>
#include <vector>
#include <cstdint>
namespace cw::live {
struct Image;
struct Strategy {
    std::string name="baseline", label="原始双包彩色基线", family="baseline";
    std::string color="color", preset="slow";
    int width=192,height=152,gop=10,slices=4,aq=1,bitrate=0,threads=0;
    double fps=20,aq_strength=1.2,denoise=0,temporal=0,sharpen=0;
    int chroma_block=1,upscale=1;
    bool offline_metrics=false;
};
struct ProcessingState { std::vector<std::uint8_t> previous; int width=0,height=0; };
Strategy load_strategy(const std::string &path, bool receiver_only=false);
std::string strategy_text(const Strategy &s, bool receiver_only=false);
bool validate_strategy(const Strategy &s,std::string &error);
void preprocess_strategy(Image &image,const Strategy &s,ProcessingState &state);
// Receiver processing has no access to source images, IDs, timestamps or encoder state.
void reconstruct_pixels(std::vector<std::uint8_t> &bgr,int &width,int &height,const Strategy &s);
std::vector<std::uint8_t> rgba_pixels(const std::vector<std::uint8_t> &bgr);
}
