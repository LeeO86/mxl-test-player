#include "placeholders.hpp"

namespace mtp {

std::string expand_placeholders(std::string_view text, const PlaceholderVars& v) {
    std::string out;
    out.reserve(text.size() + 32);
    for (std::size_t i = 0; i < text.size();) {
        if (text[i] == '{') {
            const auto end = text.find('}', i + 1);
            if (end != std::string_view::npos) {
                const auto key = text.substr(i + 1, end - i - 1);
                const std::string* rep = nullptr;
                if (key == "label") rep = &v.label;
                else if (key == "timecode") rep = &v.timecode;
                else if (key == "tai") rep = &v.tai;
                else if (key == "utc") rep = &v.utc;
                else if (key == "local") rep = &v.local;
                else if (key == "frame") rep = &v.frame;
                else if (key == "item") rep = &v.item;
                else if (key == "loop") rep = &v.loop;
                else if (key == "host") rep = &v.host;
                else if (key == "flow") rep = &v.flow;
                if (rep) {
                    out += *rep;
                    i = end + 1;
                    continue;
                }
            }
        }
        out.push_back(text[i]);
        ++i;
    }
    return out;
}

}  // namespace mtp
