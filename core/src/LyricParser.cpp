#include "wm/core/LyricParser.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace wm::core {
namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return s;
}

std::string Trim(const std::string& s) {
    auto isSpace = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && isSpace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isSpace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

long long ParseSignedMillis(const std::string& v) {
    try {
        return static_cast<long long>(std::llround(std::stod(v)));
    } catch (...) {
        return 0;
    }
}

/// [mm:ss.xx] | [mm:ss] | [mm:ss:xx] | [mm:ss.xxx]
bool ParseTimeBody(const std::string& body, std::int64_t& outMs) {
    if (body.empty() || body[0] == '-') {
        return false;
    }

    const std::size_t c1 = body.find(':');
    if (c1 == std::string::npos) {
        return false;
    }

    double minutes = 0.0;
    double seconds = 0.0;
    double fraction = 0.0;

    try {
        minutes = std::stod(body.substr(0, c1));
        const std::size_t c2 = body.find(':', c1 + 1);
        if (c2 == std::string::npos) {
            seconds = std::stod(body.substr(c1 + 1));
        } else {
            seconds = std::stod(body.substr(c1 + 1, c2 - c1 - 1));
            const std::string rest = body.substr(c2 + 1);
            // 2 digits -> centiseconds, 3+ -> milliseconds
            const double divisor = rest.size() <= 2 ? 100.0 : 1000.0;
            fraction = std::stod(rest) / divisor;
        }
    } catch (...) {
        return false;
    }

    const double total = std::llround((minutes * 60.0 + seconds + fraction) * 1000.0);
    if (total < 0) {
        return false;
    }
    outMs = static_cast<std::int64_t>(total);
    return true;
}

/// Removes enhanced word-level tags: <mm:ss.xx> and <mm:ss.xx>...<...>
std::string StripEnhancedTags(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size()) {
        if (in[i] == '<') {
            const std::size_t close = in.find('>', i);
            if (close == std::string::npos) {
                break;
            }
            i = close + 1;
        } else {
            out.push_back(in[i]);
            ++i;
        }
    }
    return out;
}

void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string Utf16ToUtf8(const std::string& raw, bool bigEndian) {
    std::string out;
    out.reserve(raw.size() / 2 * 3);
    const std::size_t n = raw.size() - (raw.size() % 2);

    for (std::size_t i = 0; i + 1 < n; i += 2) {
        const std::uint32_t lo = static_cast<unsigned char>(raw[i]);
        const std::uint32_t hi = static_cast<unsigned char>(raw[i + 1]);
        std::uint32_t unit = bigEndian ? ((lo << 8) | hi) : ((hi << 8) | lo);

        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < n) {
            const std::uint32_t lo2 = static_cast<unsigned char>(raw[i + 2]);
            const std::uint32_t hi2 = static_cast<unsigned char>(raw[i + 3]);
            const std::uint32_t low = bigEndian ? ((lo2 << 8) | hi2) : ((hi2 << 8) | lo2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        AppendUtf8(out, unit);
    }
    return out;
}

} // namespace

std::string LyricParser::DecodeToUtf8(const std::string& rawBytes) {
    if (rawBytes.size() >= 3) {
        const auto b0 = static_cast<unsigned char>(rawBytes[0]);
        const auto b1 = static_cast<unsigned char>(rawBytes[1]);
        const auto b2 = static_cast<unsigned char>(rawBytes[2]);
        if (b0 == 0xEF && b1 == 0xBB && b2 == 0xBF) {
            return rawBytes.substr(3); // already UTF-8, drop BOM
        }
    }
    if (rawBytes.size() >= 2) {
        const auto b0 = static_cast<unsigned char>(rawBytes[0]);
        const auto b1 = static_cast<unsigned char>(rawBytes[1]);
        if (b0 == 0xFF && b1 == 0xFE) {
            return Utf16ToUtf8(rawBytes.substr(2), false);
        }
        if (b0 == 0xFE && b1 == 0xFF) {
            return Utf16ToUtf8(rawBytes.substr(2), true);
        }
        // BOM-less UTF-16LE heuristic: plenty of NUL bytes at odd offsets.
        if (b1 == 0x00 && rawBytes.size() % 2 == 0 && rawBytes.size() >= 8) {
            std::size_t nulls = 0;
            for (std::size_t i = 1; i < std::min<std::size_t>(64, rawBytes.size()); i += 2) {
                if (rawBytes[i] == '\0') ++nulls;
            }
            if (nulls >= 8) {
                return Utf16ToUtf8(rawBytes, false);
            }
        }
    }
    return rawBytes;
}

LyricDocument LyricParser::Parse(const std::string& utf8Text) {
    LyricDocument doc;
    std::vector<LyricLine> lines;

    std::size_t pos = 0;
    while (pos < utf8Text.size()) {
        const std::size_t eol = utf8Text.find('\n', pos);
        std::string line = utf8Text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        std::vector<std::int64_t> times;
        std::size_t i = 0;
        while (i < line.size()) {
            // Skip leading blanks between tags.
            while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
            if (i >= line.size() || line[i] != '[') {
                break;
            }
            const std::size_t close = line.find(']', i);
            if (close == std::string::npos) {
                break;
            }

            const std::string body = line.substr(i + 1, close - i - 1);
            i = close + 1;

            std::int64_t ms = 0;
            if (ParseTimeBody(body, ms)) {
                times.push_back(ms);
                continue;
            }

            const std::size_t colon = body.find(':');
            if (colon != std::string::npos) {
                const std::string key = ToLower(Trim(body.substr(0, colon)));
                const std::string value = Trim(body.substr(colon + 1));
                if (key == "offset") {
                    doc.offsetMs = ParseSignedMillis(value);
                } else if (key == "ti") {
                    doc.meta.title = value;
                } else if (key == "ar") {
                    doc.meta.artist = value;
                } else if (key == "al") {
                    doc.meta.album = value;
                } else if (key == "by") {
                    doc.meta.by = value;
                }
            }
        }

        std::string content = StripEnhancedTags(line.substr(i));
        // Keep a single trailing/leading trim but preserve intentional spacing.
        if (!times.empty()) {
            const std::string trimmed = Trim(content);
            for (const std::int64_t t : times) {
                lines.push_back(LyricLine{t, trimmed});
            }
        }

        if (eol == std::string::npos) {
            break;
        }
        pos = eol + 1;
    }

    std::stable_sort(lines.begin(), lines.end(), [](const LyricLine& a, const LyricLine& b) {
        return a.timeMs < b.timeMs;
    });

    doc.lines = std::move(lines);
    doc.valid = !doc.lines.empty();
    return doc;
}

std::string LyricParser::Serialize(const LyricDocument& doc) {
    std::string out;
    out.reserve(doc.lines.size() * 32);

    auto meta = [&out](const char* tag, const std::string& value) {
        if (!value.empty()) {
            out += '[';
            out += tag;
            out += ':';
            out += value;
            out += "]\r\n";
        }
    };

    meta("ti", doc.meta.title);
    meta("ar", doc.meta.artist);
    meta("al", doc.meta.album);
    meta("by", doc.meta.by.empty() ? std::string("w-music") : doc.meta.by);
    if (doc.offsetMs != 0) {
        out += "[offset:" + std::to_string(doc.offsetMs) + "]\r\n";
    }

    for (const auto& line : doc.lines) {
        const std::int64_t total = line.timeMs;
        const std::int64_t minutes = total / 60000;
        const std::int64_t seconds = (total % 60000) / 1000;
        const std::int64_t hundredths = (total % 1000) / 10;

        char buf[32];
        std::snprintf(buf, sizeof(buf), "[%02lld:%02lld.%02lld]",
                      static_cast<long long>(minutes),
                      static_cast<long long>(seconds),
                      static_cast<long long>(hundredths));
        out += buf;
        out += line.text;
        out += "\r\n";
    }
    return out;
}

} // namespace wm::core
