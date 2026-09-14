#pragma once

#include "Lyric.h"

#include <string>

namespace wm::core {

/// Parser for the LRC (LyRiCs) format.
///
/// Supported:
///   [mm:ss.xx]text              multiple time tags per line
///   [offset:+/-1234]            global shift in milliseconds
///   [ti: / ar: / al: / by: / length: / re: / ve:]   metadata
///   <mm:ss.xx> enhanced word-level tags are stripped, text is preserved
///   UTF-8 and UTF-16 (with or without BOM) input, CRLF or LF
class LyricParser {
public:
    static LyricDocument Parse(const std::string& utf8Text);

    /// Serialize back to LRC text (enhanced tags are lost, offset preserved).
    static std::string Serialize(const LyricDocument& doc);

    /// Best-effort decode of a raw file buffer into UTF-8.
    static std::string DecodeToUtf8(const std::string& rawBytes);
};

} // namespace wm::core
