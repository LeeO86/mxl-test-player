#pragma once

#include <string>
#include <string_view>

namespace mtp {

struct PlaceholderVars {
    std::string label;
    std::string timecode;
    std::string tai;
    std::string utc;
    std::string local;
    std::string frame;
    std::string item;
    std::string loop;
    std::string host;
    std::string flow;
};

std::string expand_placeholders(std::string_view text, const PlaceholderVars& v);

}  // namespace mtp
