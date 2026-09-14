#include <wm/core/ProviderEngine.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <string_view>

namespace wm::core
{
    namespace
    {
        constexpr char kUnreserved[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~";

        bool IsUnreserved(unsigned char c)
        {
            return std::string_view(kUnreserved).find(static_cast<char>(c)) != std::string_view::npos;
        }

        std::string ToLower(std::string const& value)
        {
            std::string out(value);
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        std::string Trim(std::string const& value)
        {
            const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) { return std::isspace(c); });
            const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) { return std::isspace(c); }).base();
            if (begin >= end)
            {
                return {};
            }
            return std::string(begin, end);
        }

        std::string JoinUrl(std::string const& base, std::string const& path)
        {
            if (path.empty())
            {
                return base;
            }
            if (path.rfind("http://", 0) == 0 || path.rfind("https://", 0) == 0)
            {
                return path;
            }
            std::string left = base;
            while (!left.empty() && left.back() == '/')
            {
                left.pop_back();
            }
            if (!path.empty() && path.front() != '/')
            {
                left += '/';
            }
            return left + path;
        }
    } // namespace

    std::string UrlEncode(std::string const& value)
    {
        static constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(value.size() * 3);
        for (unsigned char c : value)
        {
            if (IsUnreserved(c))
            {
                out.push_back(static_cast<char>(c));
            }
            else
            {
                out.push_back('%');
                out.push_back(hex[(c >> 4) & 0xF]);
                out.push_back(hex[c & 0xF]);
            }
        }
        return out;
    }

    std::string RenderTemplate(std::string const& tpl, std::map<std::string, std::string> const& vars)
    {
        std::string out;
        out.reserve(tpl.size());
        for (std::size_t i = 0; i < tpl.size();)
        {
            const char c = tpl[i];
            if (c != '{')
            {
                out.push_back(c);
                ++i;
                continue;
            }
            const std::size_t close = tpl.find('}', i);
            if (close == std::string::npos)
            {
                out.push_back(c);
                ++i;
                continue;
            }
            const std::string name = tpl.substr(i + 1, close - i - 1);
            auto it = vars.find(name);
            if (it != vars.end())
            {
                out += it->second;
            }
            i = close + 1;
        }
        return out;
    }

    json::Value const* JsonAt(json::Value const& root, std::string const& path)
    {
        if (path == "$")
        {
            return &root;
        }
        if (path.rfind("$.", 0) == 0)
        {
            return JsonAt(root, path.substr(2));
        }
        if (path.empty())
        {
            return &root;
        }
        json::Value const* current = &root;
        std::size_t pos = 0;
        while (pos < path.size() && current != nullptr)
        {
            if (path[pos] == '.')
            {
                ++pos;
                continue;
            }
            std::size_t end = path.find_first_of(".[", pos);
            std::string key = path.substr(pos, end == std::string::npos ? std::string::npos : end - pos);

            if (key.empty())
            {
                // Path starts with "[" -> array index on the current node.
                end = path.find(']', pos);
                if (end == std::string::npos)
                {
                    return nullptr;
                }
                const long index = std::strtol(path.substr(pos + 1, end - pos - 1).c_str(), nullptr, 10);
                if (!current->isArray() || index < 0)
                {
                    return nullptr;
                }
                auto const& arr = current->asArray();
                if (static_cast<std::size_t>(index) >= arr.size())
                {
                    return nullptr;
                }
                current = &arr[static_cast<std::size_t>(index)];
                pos = end + 1;
                continue;
            }

            if (!current->isObject())
            {
                return nullptr;
            }
            current = current->Find(key);
            pos = (end == std::string::npos) ? path.size() : end;
        }
        return current;
    }

    std::string JsonText(json::Value const& value)
    {
        if (value.isString())
        {
            return value.asString();
        }
        if (value.isBool())
        {
            return value.asBool() ? "true" : "false";
        }
        if (value.isArray())
        {
            // Sites return scalar metadata as single-element arrays (creator,
            // tags, ...); join the stringable members instead of dropping them.
            std::string out;
            for (json::Value const& item : value.asArray())
            {
                const std::string part = JsonText(item);
                if (part.empty())
                {
                    continue;
                }
                if (!out.empty())
                {
                    out += ", ";
                }
                out += part;
            }
            return out;
        }
        if (value.isNumber())
        {
            const double d = value.asNumber();
            if (std::fabs(d - std::round(d)) < 1e-9)
            {
                return std::to_string(static_cast<long long>(std::llround(d)));
            }
            std::ostringstream oss;
            oss << d;
            return oss.str();
        }
        return {};
    }

    std::vector<std::string> RegexAll(std::string const& text, std::string const& pattern, int group)
    {
        std::vector<std::string> out;
        if (pattern.empty())
        {
            return out;
        }
        try
        {
            const std::regex re(pattern, std::regex::ECMAScript | std::regex::icase);
            auto begin = std::sregex_iterator(text.begin(), text.end(), re);
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it)
            {
                const std::size_t index = static_cast<std::size_t>(group < 0 ? 0 : group);
                if (index < it->size())
                {
                    out.push_back((*it)[index].str());
                }
            }
        }
        catch (std::regex_error const&)
        {
            return {};
        }
        return out;
    }

    std::string RegexFirst(std::string const& text, std::string const& pattern, int group)
    {
        auto all = RegexAll(text, pattern, group);
        return all.empty() ? std::string{} : all.front();
    }

    std::string HtmlDecode(std::string const& text)
    {
        static const std::map<std::string, std::string> named = {
            { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
            { "apos", "'" }, { "nbsp", " " }, { "#39", "'" }, { "#34", "\"" },
        };
        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            if (text[i] != '&')
            {
                out.push_back(text[i]);
                continue;
            }
            const std::size_t semi = text.find(';', i);
            if (semi == std::string::npos || semi - i > 12)
            {
                out.push_back(text[i]);
                continue;
            }
            std::string token = text.substr(i + 1, semi - i - 1);
            if (!token.empty() && token[0] == '#')
            {
                const int base = (token.size() > 1 && (token[1] == 'x' || token[1] == 'X')) ? 16 : 10;
                const char* digits = base == 16 ? token.c_str() + 2 : token.c_str() + 1;
                const long code = std::strtol(digits, nullptr, base);
                if (code > 0 && code < 0x110000)
                {
                    // UTF-8 encode; anything outside ASCII is rare in metadata.
                    if (code < 0x80)
                    {
                        out.push_back(static_cast<char>(code));
                    }
                    else if (code < 0x800)
                    {
                        out.push_back(static_cast<char>(0xC0 | (code >> 6)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    else
                    {
                        out.push_back(static_cast<char>(0xE0 | (code >> 12)));
                        out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
                        out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
                    }
                    i = semi;
                    continue;
                }
            }
            auto it = named.find(ToLower(token));
            if (it != named.end())
            {
                out += it->second;
                i = semi;
                continue;
            }
            out.push_back(text[i]);
        }
        return out;
    }

    std::string AbsoluteUrl(std::string const& base, std::string const& maybe)
    {
        std::string url = Trim(maybe);
        if (url.empty())
        {
            return {};
        }
        if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0)
        {
            return url;
        }
        if (url.rfind("//", 0) == 0)
        {
            const std::string scheme = base.rfind("https://", 0) == 0 ? "https:" : "http:";
            return scheme + url;
        }
        std::string root = base;
        const std::size_t schemeEnd = root.find("://");
        if (schemeEnd == std::string::npos)
        {
            return JoinUrl(root, url);
        }
        if (url.front() == '/')
        {
            const std::size_t hostEnd = root.find('/', schemeEnd + 3);
            if (hostEnd != std::string::npos)
            {
                root = root.substr(0, hostEnd);
            }
            return root + url;
        }
        const std::size_t query = root.find_first_of("?#");
        if (query != std::string::npos)
        {
            root = root.substr(0, query);
        }
        const std::size_t slash = root.rfind('/');
        if (slash != std::string::npos)
        {
            root = root.substr(0, slash + 1);
        }
        return root + url;
    }

    int ParseDurationSeconds(std::string const& text)
    {
        std::string value = Trim(text);
        if (value.empty())
        {
            return 0;
        }
        int total = 0;
        int factor = 1;
        // Walk "h:mm:ss" / "mm:ss" from the right.
        for (int pass = 0; pass < 3 && !value.empty(); ++pass)
        {
            const std::size_t sep = value.rfind(':');
            std::string part = (sep == std::string::npos) ? value : value.substr(sep + 1);
            value = (sep == std::string::npos) ? std::string{} : value.substr(0, sep);
            const int number = std::atoi(part.c_str());
            total += number * factor;
            factor *= 60;
            if (sep == std::string::npos)
            {
                break;
            }
        }
        return total;
    }

    std::string FormatDuration(int seconds)
    {
        if (seconds < 0)
        {
            seconds = 0;
        }
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%02d:%02d", seconds / 60, seconds % 60);
        return std::string(buffer);
    }

    // ---------------------------------------------------------------- engine

    ProviderEngine::ProviderEngine(ProviderAdapter adapter, FetchFn fetch)
        : m_adapter(std::move(adapter))
        , m_fetch(std::move(fetch))
    {
    }

    std::string ProviderEngine::BuildUrl(RequestStep const& step, std::map<std::string, std::string> const& vars) const
    {
        const std::string rendered = RenderTemplate(step.url, vars);
        return JoinUrl(m_adapter.baseUrl, rendered);
    }

    std::optional<OnlineTrack> ProviderEngine::ParseObject(RequestStep const& step,
                                                           std::string const& body,
                                                           std::string const& chunk,
                                                           json::Value const* jsonRoot,
                                                           std::string const& baseUrl,
                                                           json::Value const* fallbackRoot) const
    {
        OnlineTrack track;
        track.sourceId = m_adapter.id;
        bool complete = true;
        int filled = 0;

        for (ExtractRule const& rule : step.fields)
        {
            std::string value;
            if (rule.source == "static")
            {
                value = rule.value;
            }
            else if (rule.source == "json")
            {
                json::Value const* node = nullptr;
                if (jsonRoot != nullptr)
                {
                    node = JsonAt(*jsonRoot, rule.pattern);
                }
                if (node == nullptr && fallbackRoot != nullptr)
                {
                    node = JsonAt(*fallbackRoot, rule.pattern);
                }
                if (node != nullptr)
                {
                    value = JsonText(*node);
                }
            }
            else
            {
                value = RegexFirst(chunk.empty() ? body : chunk, rule.pattern, rule.group);
                if (m_adapter.charset != "raw")
                {
                    value = HtmlDecode(Trim(value));
                }
            }

            if (value.empty() && rule.required)
            {
                complete = false;
            }
            if (!value.empty())
            {
                ++filled;
            }

            const std::string key = ToLower(rule.key);
            if (key == "id") { track.id = value; }
            else if (key == "title") { track.title = value; }
            else if (key == "artist") { track.artist = value; }
            else if (key == "album") { track.album = value; }
            else if (key == "duration") { track.durationText = value; track.durationSec = ParseDurationSeconds(value); }
            else if (key == "cover") { track.coverUrl = AbsoluteUrl(baseUrl, value); }
            else if (key == "play") { track.playUrl = AbsoluteUrl(baseUrl, value); }
            else if (key == "download") { track.downloadUrl = AbsoluteUrl(baseUrl, value); }
            else if (key == "detail") { track.detailUrl = AbsoluteUrl(baseUrl, value); }
            else { track.extra[rule.key] = value; }
        }

        if (!complete || filled == 0)
        {
            return std::nullopt;
        }
        if (track.durationSec > 0)
        {
            track.durationText = FormatDuration(track.durationSec);
        }
        return track;
    }

    std::vector<OnlineTrack> ProviderEngine::ParseList(RequestStep const& step,
                                                       std::string const& body,
                                                       std::string const& baseUrl) const
    {
        std::vector<OnlineTrack> out;

        if (!step.itemPattern.empty())
        {
            for (std::string const& chunk : RegexAll(body, step.itemPattern, 0))
            {
                if (auto track = ParseObject(step, body, chunk, nullptr, baseUrl))
                {
                    out.push_back(std::move(*track));
                }
            }
            return out;
        }

        std::string parseError;
        auto parsed = json::Parse(body, &parseError);

        if (!step.listPath.empty())
        {
            if (!parsed.has_value())
            {
                return out;
            }
            json::Value const* node = JsonAt(*parsed, step.listPath);
            if (node == nullptr)
            {
                return out;
            }

            // One record = one element of the list node (or the node itself).
            // With flattenPath set, each record additionally expands into one
            // output row per element of the nested array; fields read the
            // nested element first and fall back to the parent record.
            auto const emit = [&](json::Value const& record) {
                if (!step.flattenPath.empty())
                {
                    json::Value const* nested = JsonAt(record, step.flattenPath);
                    if (nested != nullptr && nested->isArray() && !nested->asArray().empty())
                    {
                        for (json::Value const& element : nested->asArray())
                        {
                            if (auto track = ParseObject(step, body, {}, &element, baseUrl, &record))
                            {
                                if (!track->title.empty())
                                {
                                    out.push_back(std::move(*track));
                                }
                            }
                        }
                        return;
                    }
                }
                if (auto track = ParseObject(step, body, {}, &record, baseUrl))
                {
                    if (!track->title.empty())
                    {
                        out.push_back(std::move(*track));
                    }
                }
            };

            if (node->isArray())
            {
                for (json::Value const& item : node->asArray())
                {
                    emit(item);
                }
            }
            else
            {
                emit(*node);
            }
            return out;
        }

        // No list split: the whole body is one record. JSON rules read the
        // parsed document, regex rules scan the raw text.
        auto single = parsed.has_value()
            ? ParseObject(step, body, {}, &*parsed, baseUrl)
            : ParseObject(step, body, body, nullptr, baseUrl);
        if (single.has_value())
        {
            out.push_back(std::move(*single));
        }
        return out;
    }

    ProviderResult ProviderEngine::Search(std::string const& query, int limit)
    {
        ProviderResult result;
        result.stage = "search";
        if (!m_fetch)
        {
            result.message = "no HTTP transport configured";
            return result;
        }

        std::map<std::string, std::string> vars;
        vars["query"] = query;
        vars["query_enc"] = UrlEncode(query);

        HttpRequest request;
        request.url = BuildUrl(m_adapter.search, vars);
        request.method = m_adapter.search.method.empty() ? "GET" : m_adapter.search.method;
        request.body = RenderTemplate(m_adapter.search.body, vars);
        for (auto const& [name, value] : m_adapter.headers)
        {
            request.headers[name] = RenderTemplate(value, vars);
        }
        for (auto const& [name, value] : m_adapter.search.headers)
        {
            request.headers[name] = RenderTemplate(value, vars);
        }

        const HttpResponse response = m_fetch(request);
        if (!response.ok())
        {
            result.stage = "fetch";
            result.message = "HTTP " + std::to_string(response.status);
            return result;
        }

        result.tracks = ParseList(m_adapter.search, response.body, request.url);
        if (limit > 0 && static_cast<int>(result.tracks.size()) > limit)
        {
            result.tracks.resize(static_cast<std::size_t>(limit));
        }
        result.ok = !result.tracks.empty();
        if (!result.ok)
        {
            result.stage = "parse";
            result.message = "no records matched the adapter rules";
        }
        return result;
    }

    ProviderResult ProviderEngine::Resolve(OnlineTrack const& track)
    {
        ProviderResult result;
        result.stage = "detail";
        result.tracks.push_back(track);
        if (!m_adapter.detail.enabled)
        {
            result.ok = !track.playUrl.empty() || !track.downloadUrl.empty();
            if (!result.ok)
            {
                result.message = "adapter has no detail step and no url was parsed";
            }
            return result;
        }
        if (!m_fetch)
        {
            result.message = "no HTTP transport configured";
            return result;
        }

        std::map<std::string, std::string> vars;
        vars["id"] = track.id;
        vars["title"] = track.title;
        vars["artist"] = track.artist;
        vars["album"] = track.album;
        vars["detail"] = track.detailUrl;
        vars["query_enc"] = UrlEncode(track.title);
        for (auto const& [key, value] : track.extra)
        {
            vars["$" + key] = value;
        }

        HttpRequest request;
        request.url = BuildUrl(m_adapter.detail, vars);
        request.method = m_adapter.detail.method.empty() ? "GET" : m_adapter.detail.method;
        request.body = RenderTemplate(m_adapter.detail.body, vars);
        for (auto const& [name, value] : m_adapter.headers)
        {
            request.headers[name] = RenderTemplate(value, vars);
        }
        for (auto const& [name, value] : m_adapter.detail.headers)
        {
            request.headers[name] = RenderTemplate(value, vars);
        }

        const HttpResponse response = m_fetch(request);
        if (!response.ok())
        {
            result.stage = "fetch";
            result.message = "HTTP " + std::to_string(response.status);
            return result;
        }

        std::vector<OnlineTrack> parsed = ParseList(m_adapter.detail, response.body, request.url);
        if (parsed.empty())
        {
            result.message = "detail page did not match the adapter rules";
            return result;
        }

        OnlineTrack& target = result.tracks.front();
        OnlineTrack const& detail = parsed.front();
        if (!detail.playUrl.empty()) { target.playUrl = detail.playUrl; }
        if (!detail.downloadUrl.empty()) { target.downloadUrl = detail.downloadUrl; }
        if (!detail.coverUrl.empty()) { target.coverUrl = detail.coverUrl; }
        if (!detail.durationText.empty()) { target.durationText = detail.durationText; }
        if (detail.durationSec > 0) { target.durationSec = detail.durationSec; }
        if (!detail.album.empty()) { target.album = detail.album; }
        if (!detail.artist.empty()) { target.artist = detail.artist; }
        for (auto const& [key, value] : detail.extra)
        {
            target.extra[key] = value;
        }

        result.ok = !target.playUrl.empty() || !target.downloadUrl.empty();
        if (!result.ok)
        {
            result.message = "detail step produced no url";
        }
        return result;
    }

    ProviderText ProviderEngine::Lyric(OnlineTrack const& track)
    {
        ProviderText result;
        if (!m_adapter.lyric.enabled)
        {
            result.message = "adapter has no lyric step";
            return result;
        }
        if (!m_fetch)
        {
            result.message = "no HTTP transport configured";
            return result;
        }

        std::map<std::string, std::string> vars;
        vars["id"] = track.id;
        vars["title"] = track.title;
        vars["artist"] = track.artist;
        vars["detail"] = track.detailUrl;
        vars["query_enc"] = UrlEncode(track.title);
        for (auto const& [key, value] : track.extra)
        {
            vars["$" + key] = value;
        }

        HttpRequest request;
        request.url = BuildUrl(m_adapter.lyric, vars);
        request.method = m_adapter.lyric.method.empty() ? "GET" : m_adapter.lyric.method;
        for (auto const& [name, value] : m_adapter.headers)
        {
            request.headers[name] = RenderTemplate(value, vars);
        }
        for (auto const& [name, value] : m_adapter.lyric.headers)
        {
            request.headers[name] = RenderTemplate(value, vars);
        }

        const HttpResponse response = m_fetch(request);
        if (!response.ok())
        {
            result.message = "HTTP " + std::to_string(response.status);
            return result;
        }
        result.text = response.body;
        result.ok = !result.text.empty();
        return result;
    }
} // namespace wm::core
