#pragma once

#include "config.hpp"
#include "http_server.hpp"

#include <string>

namespace mtp {

// True for /widgets and /widget/<id>.
bool widget_path(const std::string& path);

// GET /widgets (the list, CORS only for the WIDGET_FRAME_ANCESTORS origins) and
// GET /widget/transport?output=<n> (`page`, framed only by WIDGET_FRAME_ANCESTORS). SPECIFICATION.md §9.1.
void widget_response(const Config& cfg, const HttpRequest& req, const std::string& page, HttpResponse& res);

}  // namespace mtp
