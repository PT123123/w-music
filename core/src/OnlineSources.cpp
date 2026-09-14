#include <wm/core/OnlineSources.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>

namespace wm::core
{
    namespace
    {
        // ---------------------------------------------------------------- json

        json::Value const* At(json::Value const& root, std::string const& path)
        {
            return JsonAt(root, path);
        }

        std::string Str(json::Value const* value, std::string const& fallback = {})
        {
            if (value == nullptr || !value->isString())
            {
                return fallback;
            }
            return value->asString();
        }

        std::string StrPath(json::Value const& root, std::string const& path)
        {
            return Str(At(root, path));
        }

        int IntPath(json::Value const& root, std::string const& path, int fallback = 0)
        {
            auto const* value = At(root, path);
            return value == nullptr ? fallback : static_cast<int>(value->asNumber(static_cast<double>(fallback)));
        }

        std::int64_t Int64Path(json::Value const& root, std::string const& path)
        {
            auto const* value = At(root, path);
            return value == nullptr ? 0 : value->asInt(0);
        }

        bool BoolPath(json::Value const& root, std::string const& path)
        {
            auto const* value = At(root, path);
            return value != nullptr && value->asBool(false);
        }

        /// Builds JSON text with compact separators, like org.json's toString().
        std::string Compact(json::Value const& value)
        {
            return json::Serialize(value, false);
        }

        // ------------------------------------------------------- text helpers

        std::string ToLower(std::string const& text)
        {
            std::string out = text;
            std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return out;
        }

        std::string TrimCopy(std::string const& text)
        {
            std::size_t begin = 0;
            std::size_t end = text.size();
            auto const space = [](unsigned char c) { return std::isspace(c) != 0; };
            while (begin < end && space(static_cast<unsigned char>(text[begin]))) ++begin;
            while (end > begin && space(static_cast<unsigned char>(text[end - 1]))) --end;
            return text.substr(begin, end - begin);
        }

        bool StartsWith(std::string const& text, char const* prefix)
        {
            std::size_t const n = std::strlen(prefix);
            return text.size() >= n && text.compare(0, n, prefix) == 0;
        }

        std::string ReplaceAll(std::string text, std::string const& from, std::string const& to)
        {
            if (from.empty())
            {
                return text;
            }
            std::size_t pos = 0;
            while ((pos = text.find(from, pos)) != std::string::npos)
            {
                text.replace(pos, from.size(), to);
                pos += to.size();
            }
            return text;
        }

        /// Splits |text| on every occurrence of |delim|, dropping empties.
        std::vector<std::string> SplitOn(std::string const& text, char delim)
        {
            std::vector<std::string> out;
            std::size_t start = 0;
            while (true)
            {
                std::size_t const pos = text.find(delim, start);
                if (pos == std::string::npos)
                {
                    out.push_back(text.substr(start));
                    break;
                }
                if (pos > start)
                {
                    out.push_back(text.substr(start, pos - start));
                }
                start = pos + 1;
            }
            return out;
        }

        /// UTF-8 aware "loose key" normalisation replicated from Net24Api.norm:
        /// case / punctuation / whitespace insensitive so the two catalogues
        /// line up on the same song. ASCII punctuation and the CJK punctuation
        /// below are stripped; everything else (including CJK text) is kept.
        std::string NormKey(std::string const& text)
        {
            static std::string const kStripMulti[] = {
                "\xEF\xBC\x88", "\xEF\xBC\x89",   // （ ）
                "\xE3\x80\x90", "\xE3\x80\x91",   // 【 】
                "\xE3\x80\x8A", "\xE3\x80\x8B",   // 《 》
                "\xEF\xBD\x9E",                   // ～
                "\xEF\xBC\x81", "\xEF\xBC\x9F",   // ！ ？
                "\xEF\xBC\x8C", "\xE3\x80\x82",   // ， 。
                "\xE3\x80\x81",                   // 、
                "\xE2\x80\x94",                   // —
                "\xEF\xBC\x9B",                   // ；
            };
            static auto const asciiStrip = [](unsigned char c) {
                switch (c)
                {
                case '(': case ')': case '[': case ']': case '~': case '!':
                case '?': case ',': case '.': case '-': case '_': case '\'':
                case '"': case '/': case '|':
                    return true;
                default:
                    return std::isspace(c) != 0;
                }
            };

            std::string out;
            out.reserve(text.size());
            std::size_t i = 0;
            while (i < text.size())
            {
                bool stripped = false;
                for (std::string const& multi : kStripMulti)
                {
                    if (text.compare(i, multi.size(), multi) == 0)
                    {
                        i += multi.size();
                        stripped = true;
                        break;
                    }
                }
                if (stripped)
                {
                    continue;
                }
                unsigned char const c = static_cast<unsigned char>(text[i]);
                if (c < 0x80)
                {
                    if (!asciiStrip(c))
                    {
                        out.push_back(static_cast<char>(std::tolower(c)));
                    }
                    i += 1;
                }
                else
                {
                    // Keep the whole multi-byte sequence untouched.
                    std::size_t length = 1;
                    if ((c & 0xE0) == 0xC0) length = 2;
                    else if ((c & 0xF0) == 0xE0) length = 3;
                    else if ((c & 0xF8) == 0xF0) length = 4;
                    out.append(text, i, std::min(length, text.size() - i));
                    i += length;
                }
            }
            return out;
        }

        /// `周杰伦&温岚` -> `周杰伦`, so the two catalogues line up on the lead artist.
        std::string LeadArtist(std::string const& artist)
        {
            static std::string const kSeparators[] = {
                "&", "/", ";",
                "\xE3\x80\x81",  // 、
                "\xEF\xBC\x8C",  // ，
                "\xEF\xBC\x9B",  // ；
            };
            std::size_t cut = artist.size();
            for (std::string const& sep : kSeparators)
            {
                std::size_t const pos = artist.find(sep);
                if (pos != std::string::npos && pos < cut)
                {
                    cut = pos;
                }
            }
            return NormKey(artist.substr(0, cut));
        }
    } // namespace

    // =====================================================================
    // == QQ 音乐                                                          ==
    // =====================================================================

    std::string QqSong::CoverUrl() const
    {
        if (albumMid.empty())
        {
            return {};
        }
        return "https://y.qq.com/music/photo_new/T002R300x300M000" + albumMid + ".jpg";
    }

    std::string QqSong::DurationText() const
    {
        if (durationSec <= 0)
        {
            return "--:--";
        }
        char buffer[16]{};
        std::snprintf(buffer, sizeof(buffer), "%d:%02d", durationSec / 60, durationSec % 60);
        return buffer;
    }

    std::string QqSong::SizeText() const
    {
        if (sizeBytes <= 0)
        {
            return {};
        }
        if (sizeBytes >= 1024 * 1024)
        {
            char buffer[32]{};
            std::snprintf(buffer, sizeof(buffer), "%.1f MB",
                          static_cast<double>(sizeBytes) / 1024.0 / 1024.0);
            return buffer;
        }
        char buffer[32]{};
        std::snprintf(buffer, sizeof(buffer), "%.0f KB", static_cast<double>(sizeBytes) / 1024.0);
        return buffer;
    }

    std::string QqSong::DisplayName() const
    {
        return artist.empty() ? title : artist + " - " + title;
    }

    QqSource::QqSource(FetchFn fetch) : m_fetch(std::move(fetch))
    {
    }

    std::vector<std::string> const& QqSource::HotWords()
    {
        static std::vector<std::string> const words = {
            "周杰伦", "林俊杰", "陈奕迅", "邓紫棋", "薛之谦",
            "毛不易", "五月天", "华语流行", "轻音乐", "粤语经典",
        };
        return words;
    }

    std::map<std::string, std::string> QqSource::DownloadHeaders()
    {
        return {
            { "User-Agent", kUa },
            { "Referer", kReferer },
            { "Accept", "*/*" },
        };
    }

    std::string QqSource::HttpGet(std::string const& url, std::string const& referer) const
    {
        HttpRequest request;
        request.url = url;
        request.method = "GET";
        request.headers = {
            { "User-Agent", kUa },
            { "Referer", referer },
            { "Accept", "*/*" },
        };
        if (!m_session.empty())
        {
            request.headers["Cookie"] = m_session;
        }
        HttpResponse const response = m_fetch(request);
        return response.body;   // error bodies are parsed too, like a-music
    }

    std::vector<QqSong> QqSource::Search(std::string const& keyword, int page, int pageSize) const
    {
        std::string const keywordTrimmed = TrimCopy(keyword);
        if (keywordTrimmed.empty())
        {
            return {};
        }

        std::string const url = std::string(kSearchEndpoint) +
            "?w=" + UrlEncode(keywordTrimmed) +
            "&format=json&n=" + std::to_string(pageSize) +
            "&p=" + std::to_string(page) +
            "&cr=1&new_json=0&platform=wxforsong&needNewCode=0";

        std::string const body = HttpGet(url, kReferer);
        if (body.empty())
        {
            return {};
        }

        std::string error;
        auto root = json::Parse(body, &error);
        if (!root)
        {
            return {};
        }

        auto const* list = At(*root, "data.song.list");
        if (list == nullptr || !list->isArray())
        {
            return {};
        }

        std::vector<QqSong> out;
        out.reserve(list->asArray().size());
        for (json::Value const& entry : list->asArray())
        {
            if (!entry.isObject())
            {
                continue;
            }
            std::string const mid = StrPath(entry, "songmid");
            if (mid.empty())
            {
                continue;
            }
            QqSong song;
            song.mid = mid;
            song.title = StrPath(entry, "songname");
            if (song.title.empty())
            {
                song.title = StrPath(entry, "title");
            }
            song.album = StrPath(entry, "albumname");
            song.albumMid = StrPath(entry, "albummid");
            song.durationSec = IntPath(entry, "interval");
            song.sizeBytes = Int64Path(entry, "size128");
            song.vipOnly = IntPath(entry, "pay.payplay") == 1;
            if (auto const* singers = At(entry, "singer"); singers != nullptr && singers->isArray())
            {
                for (json::Value const& singer : singers->asArray())
                {
                    std::string const name = StrPath(singer, "name");
                    if (!name.empty())
                    {
                        if (!song.artist.empty())
                        {
                            song.artist += " / ";
                        }
                        song.artist += name;
                    }
                }
            }
            out.push_back(std::move(song));
        }
        return out;
    }

    std::string QqSource::DirectUrl(std::string const& mid) const
    {
        if (mid.empty())
        {
            return {};
        }

        json::Object document;
        {
            json::Object comm;
            comm["ct"] = json::Value(24);
            comm["cv"] = json::Value(0);
            document["comm"] = json::Value{ std::move(comm) };

            {
                json::Object reqParam;
                reqParam["guid"] = json::Value{ std::string(kGuid) };
                reqParam["calltype"] = json::Value(0);
                reqParam["userip"] = json::Value{ std::string{} };
                json::Object req;
                req["module"] = json::Value{ std::string("CDN.SrfCdnDispatchServer") };
                req["method"] = json::Value{ std::string("GetCdnDispatch") };
                req["param"] = json::Value{ std::move(reqParam) };
                document["req"] = json::Value{ std::move(req) };
            }
            {
                json::Array songMids;
                songMids.push_back(json::Value{ mid });
                json::Array songTypes;
                songTypes.push_back(json::Value(0));

                json::Object vkeyParam;
                vkeyParam["guid"] = json::Value{ std::string(kGuid) };
                vkeyParam["songmid"] = json::Value{ std::move(songMids) };
                vkeyParam["songtype"] = json::Value{ std::move(songTypes) };
                vkeyParam["uin"] = json::Value{ m_uin.empty() ? std::string("0") : m_uin };
                vkeyParam["loginflag"] = json::Value(m_uin.empty() ? 1 : 0);
                vkeyParam["platform"] = json::Value{ std::string("20") };

                json::Object req0;
                req0["module"] = json::Value{ std::string("vkey.GetVkeyServer") };
                req0["method"] = json::Value{ std::string("CgiGetVkey") };
                req0["param"] = json::Value{ std::move(vkeyParam) };
                document["req_0"] = json::Value{ std::move(req0) };
            }
        }

        std::string const url = std::string(kMusicuEndpoint) +
            "?format=json&data=" + UrlEncode(Compact(json::Value{ document }));
        std::string const body = HttpGet(url, kReferer);
        if (body.empty())
        {
            return {};
        }

        auto rootValue = json::Parse(body);
        if (!rootValue)
        {
            return {};
        }

        std::string purl = StrPath(*rootValue, "req_0.data.midurlinfo[0].purl");
        if (purl.empty())
        {
            return {};   // VIP-only or unavailable
        }
        if (StartsWith(purl, "http"))
        {
            return purl;
        }
        return StrPath(*rootValue, "req_0.data.sip[0]") + purl;
    }

    std::string QqSource::Lyric(std::string const& mid) const
    {
        if (mid.empty())
        {
            return {};
        }

        // nobase64=1 makes the endpoint return plain LRC instead of the base64
        // payload, so no decoding is needed on our side.
        std::string const url = std::string(kLyricEndpoint) +
            "?songmid=" + UrlEncode(mid) +
            "&g_tk=5381&format=json&inCharset=utf8&outCharset=utf-8&nobase64=1";
        std::string const body = HttpGet(url, kReferer);
        if (body.empty())
        {
            return {};
        }

        auto root = json::Parse(body);
        if (!root)
        {
            return {};
        }
        std::string const lyric = StrPath(*root, "lyric");
        if (lyric.empty())
        {
            // Some responses carry the payload under "lyric" only; an empty
            // string means the song simply has no lyric.
            return {};
        }
        return lyric;
    }

    // =====================================================================
    // == QQ 音乐 网页版扫码登录                                           ==
    // =====================================================================

    namespace
    {
        /// Splits a raw "k=v; k=v" cookie string into its individual pairs.
        std::vector<std::pair<std::string, std::string>> SplitCookies(std::string const& text)
        {
            std::vector<std::pair<std::string, std::string>> out;
            for (std::string const& part : SplitOn(text, ';'))
            {
                std::string const trimmed = TrimCopy(part);
                if (trimmed.empty())
                {
                    continue;
                }
                std::size_t const eq = trimmed.find('=');
                if (eq == std::string::npos)
                {
                    continue;
                }
                std::string const key = TrimCopy(trimmed.substr(0, eq));
                std::string const value = TrimCopy(trimmed.substr(eq + 1));
                if (!key.empty())
                {
                    out.emplace_back(key, value);
                }
            }
            return out;
        }

        /// Joins cookie pairs into a "k=v; k=v" string.
        std::string JoinCookies(std::vector<std::pair<std::string, std::string>> const& pairs)
        {
            std::string out;
            for (auto const& [key, value] : pairs)
            {
                if (!out.empty())
                {
                    out += "; ";
                }
                out += key + "=" + value;
            }
            return out;
        }

        /// Extracts the leading key=value token of a complete Set-Cookie line
        /// ("p_skey=abc; Path=/; HttpOnly" -> "p_skey=abc").
        void AppendSetCookieValue(std::string const& line,
                                  std::vector<std::pair<std::string, std::string>>& out)
        {
            std::string const trimmed = TrimCopy(line);
            if (trimmed.empty())
            {
                return;
            }
            std::size_t const semi = trimmed.find(';');
            std::string const pair = TrimCopy(semi == std::string::npos ? trimmed : trimmed.substr(0, semi));
            std::size_t const eq = pair.find('=');
            if (eq == std::string::npos)
            {
                return;
            }
            out.emplace_back(TrimCopy(pair.substr(0, eq)), TrimCopy(pair.substr(eq + 1)));
        }

        /// The canonical u1 this flow logs in through (the web player's target).
        char const* const kU1 =
            "https%3A%2F%2Fy.qq.com%2Fportal%2Fwplayer.html";

        /// Extracts the QQ account number from a cookie set. The ptlogin
        /// "uin" cookie is stored as "o1234567" (leading 'o').
        std::string ExtractQqUin(std::string const& cookie)
        {
            for (auto const& [key, value] : SplitCookies(cookie))
            {
                if (key == "uin")
                {
                    return !value.empty() && value[0] == 'o' ? value.substr(1) : value;
                }
            }
            return {};
        }
    } // namespace

    QqLoginFlow::QqLoginFlow(FetchFn fetch) : m_fetch(std::move(fetch))
    {
    }

    std::map<std::string, std::string> QqLoginFlow::RequestHeaders()
    {
        return {
            { "User-Agent", kUa },
            { "Referer", "https://y.qq.com/" },
            { "Accept", "*/*" },
        };
    }

    std::string QqLoginFlow::HttpGet(std::string const& url) const
    {
        HttpRequest request;
        request.url = url;
        request.method = "GET";
        request.headers = RequestHeaders();
        if (!m_cookies.empty())
        {
            request.headers["Cookie"] = m_cookies;
        }
        HttpResponse const response = m_fetch(request);
        // Fold any new cookies back in (the success poll returns the session).
        // A single header entry may carry several newline-separated
        // Set-Cookie values when the transport merged them.
        for (auto const& [name, value] : response.headers)
        {
            if (_stricmp(name.c_str(), "set-cookie") != 0)
            {
                continue;
            }
            for (std::string const& line : SplitOn(value, '\n'))
            {
                std::vector<std::pair<std::string, std::string>> current = SplitCookies(m_cookies);
                AppendSetCookieValue(line, current);
                m_cookies = JoinCookies(current);
            }
        }
        return response.body;
    }

    std::string QqLoginFlow::Hash33(std::string const& qrsig)
    {
        // Mirrors the web player's hash33:
        //   n = (n * 33 + charCode) & 0x7fffffff
        std::int64_t n = 0;
        for (unsigned char const c : qrsig)
        {
            n = ((n << 5) + n + c) & 0x7FFFFFFF;
        }
        return std::to_string(n);
    }

    QqLoginContext QqLoginFlow::FetchQr() const
    {
        QqLoginContext ctx;
        HttpRequest request;
        request.url = kQrUrl;
        request.method = "GET";
        request.headers = RequestHeaders();
        HttpResponse const response = m_fetch(request);
        if (response.status == 0 || response.body.empty())
        {
            ctx.reason = response.status == 0
                ? "\xE7\xBD\x91\xE7\xBB\x9C\xE5\xBC\x82\xE5\xB8\xB8"   // 网络异常
                : "\xE6\x9C\xAA\xE8\x8E\xB7\xE5\x8F\x96\xE5\x88\xB0\xE4\xBA\x8C\xE7\xBB\xB4\xE7\xA0\x81\xE5\x9B\xBE\xE7\x89\x87"; // 未获取到二维码图片
            return ctx;
        }
        ctx.qrImage = response.body;

        std::string qrsig;
        for (auto const& [name, value] : response.headers)
        {
            if (_stricmp(name.c_str(), "set-cookie") != 0)
            {
                continue;
            }
            for (std::string const& line : SplitOn(value, '\n'))
            {
                std::string const lowered = ToLower(line);
                const std::size_t pos = lowered.find("qrsig=");
                if (pos == std::string::npos)
                {
                    continue;
                }
                std::size_t end = line.find(';', pos);
                if (end == std::string::npos)
                {
                    end = line.size();
                }
                qrsig = line.substr(pos + 6, end - pos - 6);
                break;
            }
            if (!qrsig.empty())
            {
                break;
            }
        }
        if (qrsig.empty())
        {
            ctx.reason = "\xE6\x9C\xAA\xE8\x8E\xB7\xE5\x8F\x96\xE5\x88\xB0\xE7\x99\xBB\xE5\xBD\x95\xE4\xBC\x9A\xE8\xAF\x9D"; // 未获取到登录会话
            return ctx;
        }
        ctx.ok = true;
        ctx.qrsig = qrsig;
        ctx.ptqrtoken = Hash33(qrsig);
        m_cookies = "qrsig=" + qrsig;
        return ctx;
    }

    QqLoginResult QqLoginFlow::CheckStatus(QqLoginContext const& context) const
    {
        QqLoginResult result;
        if (!context.ok || context.qrsig.empty())
        {
            result.status = QqLoginStatus::Failed;
            result.reason = "\xE5\x85\x88\xE8\x8E\xB7\xE5\x8F\x96\xE4\xBA\x8C\xE7\xBB\xB4\xE7\xA0\x81"; // 先获取二维码
            return result;
        }

        m_cookies = "qrsig=" + context.qrsig;
        std::string const url = std::string(kLoginBase) +
            "?u1=" + kU1 +
            "&ptqrtoken=" + context.ptqrtoken +
            "&pt_aid=716027609&daid=383&pt_3rd_aid=100497308" +
            "&qrsig=" + context.qrsig +
            "&loginpt=qrlogin&aid=716027609&t=" + std::to_string(std::rand() % 10000);

        std::string const body = HttpGet(url);
        if (body.empty())
        {
            result.status = QqLoginStatus::Failed;
            result.reason = "\xE8\xAF\xB7\xE6\xB1\x82\xE5\xA4\xB1\xE8\xB4\xA5"; // 请求失败
            return result;
        }

        // Body shape: ptuiCB('code','uin','...','...','status_msg','nickname',...);
        // code: 65 waiting, 66 scanned, 67 expired, 0 success.
        auto const code = [&body]() -> int {
            std::regex const pattern(R"(ptuiCB\('(\d+)')");
            std::smatch match;
            if (!std::regex_search(body, match, pattern) || match.size() < 2)
            {
                return -1;
            }
            return std::atoi(match[1].str().c_str());
        }();

        if (code == 65)
        {
            result.status = QqLoginStatus::Waiting;
        }
        else if (code == 66)
        {
            result.status = QqLoginStatus::Scanned;
        }
        else if (code == 0)
        {
            result.status = QqLoginStatus::Success;
            result.cookie = m_cookies;
            result.uin = ExtractQqUin(m_cookies);
        }
        else
        {
            result.status = QqLoginStatus::Failed;
            result.reason = code == 67
                ? "\xE4\xBA\x8C\xE7\xBB\xB4\xE7\xA0\x81\xE5\xB7\xB2\xE8\xBF\x87\xE6\x9C\x9F"   // 二维码已过期
                : "\xE7\x99\xBB\xE5\xBD\x95\xE5\xA4\xB1\xE8\xB4\xA5";
        }
        return result;
    }

    // =====================================================================
    // == 无损站                                                           ==
    // =====================================================================

    char const* Net24OriginLabel(Net24Origin origin) noexcept
    {
        return origin == Net24Origin::Master ? "\xE6\xAF\x8D\xE5\xB8\xA6\xE6\xBA\x90"       // 母带源
                                             : "\xE6\x97\xA0\xE6\x8D\x9F\xE6\xBA\x90";     // 无损源
    }

    char const* Net24QualityType(Net24Quality quality) noexcept
    {
        switch (quality)
        {
        case Net24Quality::Master: return "a";
        case Net24Quality::Surround: return "c";
        case Net24Quality::Lossless: return "b";
        }
        return "b";
    }

    char const* Net24QualityLabel(Net24Quality quality) noexcept
    {
        switch (quality)
        {
        case Net24Quality::Master: return "\xE8\x87\xB3\xE8\x87\xBB\xE6\xAF\x8D\xE5\xB8\xA6";     // 至臻母带
        case Net24Quality::Surround: return "\xE9\xAB\x98\xE6\xB8\x85\xE7\x8E\xAF\xE7\xBB\x95\xE5\xA3\xB0"; // 高清环绕声
        case Net24Quality::Lossless: return "\xE6\x97\xA0\xE6\x8D\x9F\xE9\x9F\xB3\xE8\xB4\xA8";   // 无损音质
        }
        return "";
    }

    char const* Net24QualityShort(Net24Quality quality) noexcept
    {
        switch (quality)
        {
        case Net24Quality::Master: return "\xE6\xAF\x8D\xE5\xB8\xA6";     // 母带
        case Net24Quality::Surround: return "\xE7\x8E\xAF\xE7\xBB\x95";   // 环绕
        case Net24Quality::Lossless: return "\xE6\x97\xA0\xE6\x8D\x9F";   // 无损
        }
        return "";
    }

    Net24Origin Net24QualityOrigin(Net24Quality quality) noexcept
    {
        return quality == Net24Quality::Lossless ? Net24Origin::Lossless : Net24Origin::Master;
    }

    std::string Net24Song::Id() const
    {
        return !masterId.empty() ? masterId : losslessId;
    }

    std::string Net24Song::DisplayName() const
    {
        return artist.empty() ? title : artist + " - " + title;
    }

    std::vector<Net24Quality> Net24Song::Qualities() const
    {
        std::vector<Net24Quality> out;
        for (Net24Quality quality : { Net24Quality::Master, Net24Quality::Surround, Net24Quality::Lossless })
        {
            if (!QualityId(quality).empty())
            {
                out.push_back(quality);
            }
        }
        return out;
    }

    std::string Net24Song::QualityId(Net24Quality quality) const
    {
        switch (quality)
        {
        case Net24Quality::Master:
        case Net24Quality::Surround:
            return masterId;
        case Net24Quality::Lossless:
            return losslessId;
        }
        return {};
    }

    std::string Net24Song::Key(Net24Quality quality) const
    {
        return QualityId(quality) + ":" + Net24QualityType(quality);
    }

    std::string Net24Song::SourceLabel() const
    {
        bool const hasMaster = !masterId.empty();
        bool const hasLossless = !losslessId.empty();
        if (hasMaster && hasLossless)
        {
            // 母带源 + 无损源
            return std::string("\xE6\xAF\x8D\xE5\xB8\xA6\xE6\xBA\x90 + \xE6\x97\xA0\xE6\x8D\x9F\xE6\xBA\x90");
        }
        return Net24OriginLabel(hasMaster ? Net24Origin::Master : Net24Origin::Lossless);
    }

    void Net24Song::Merge(Net24Song const& other)
    {
        if (album.empty()) album = other.album;
        if (coverUrl.empty()) coverUrl = other.coverUrl;
        if (masterId.empty()) masterId = other.masterId;
        if (losslessId.empty()) losslessId = other.losslessId;
    }

    std::int64_t ParseNet24Size(std::string const& text)
    {
        static std::regex const pattern("([0-9]+(?:\\.[0-9]+)?)\\s*(B|KB|MB|GB)",
                                        std::regex::icase | std::regex::optimize);
        std::smatch match;
        if (!std::regex_search(text, match, pattern))
        {
            return 0;
        }
        double const value = std::atof(match[1].str().c_str());
        std::string const unit = ToLower(match[2].str());
        if (unit == "gb") return static_cast<std::int64_t>(value * 1024.0 * 1024.0 * 1024.0);
        if (unit == "mb") return static_cast<std::int64_t>(value * 1024.0 * 1024.0);
        if (unit == "kb") return static_cast<std::int64_t>(value * 1024.0);
        return static_cast<std::int64_t>(value);
    }

    Net24Source::Net24Source(std::string baseUrl, FetchFn fetch)
        : m_baseUrl(std::move(baseUrl)), m_fetch(std::move(fetch))
    {
        while (!m_baseUrl.empty() && m_baseUrl.back() == '/')
        {
            m_baseUrl.pop_back();
        }
    }

    void Net24Source::SetBaseUrl(std::string baseUrl)
    {
        while (!baseUrl.empty() && baseUrl.back() == '/')
        {
            baseUrl.pop_back();
        }
        m_baseUrl = std::move(baseUrl);
    }

    std::vector<std::string> const& Net24Source::HotWords()
    {
        static std::vector<std::string> const words = {
            "周杰伦", "陈奕迅", "Beyond", "张学友", "蔡琴",
            "王菲", "邓紫棋", "谭咏麟", "林俊杰", "邓丽君",
            "富士山下", "渡口", "海阔天空", "晴天", "青花瓷", "稻香",
        };
        return words;
    }

    std::map<std::string, std::string> Net24Source::DownloadHeaders() const
    {
        return {
            { "User-Agent", kUa },
            { "Referer", m_baseUrl + "/" },
            { "Accept", "*/*" },
        };
    }

    std::string Net24Source::HttpPost(std::string const& url, std::string const& body) const
    {
        HttpRequest request;
        request.url = url;
        request.method = "POST";
        request.body = body;
        request.headers = {
            { "User-Agent", kUa },
            { "Referer", m_baseUrl + "/" },
            { "Origin", m_baseUrl },
            { "Content-Type", "application/json" },
            { "Accept", "application/json, text/plain, */*" },
        };
        HttpResponse const response = m_fetch(request);
        return response.body;
    }

    namespace
    {
        struct Net24ListRow
        {
            std::string id;
            std::string title;
            std::string artist;
            std::string album;
            std::string cover;
        };

        std::vector<Net24ListRow> ParseNet24SearchBody(std::string const& body)
        {
            std::vector<Net24ListRow> out;
            if (TrimCopy(body).empty())
            {
                return out;
            }
            auto root = json::Parse(body);
            if (!root || !BoolPath(*root, "status"))
            {
                return out;
            }
            auto const* result = At(*root, "result");
            if (result == nullptr || !result->isArray())
            {
                return out;
            }
            for (json::Value const& entry : result->asArray())
            {
                Net24ListRow row;
                row.id = StrPath(entry, "id");
                if (row.id.empty())
                {
                    continue;
                }
                row.title = StrPath(entry, "name");
                row.artist = StrPath(entry, "player");
                row.album = StrPath(entry, "album");
                row.cover = StrPath(entry, "cover");
                out.push_back(std::move(row));
            }
            return out;
        }
    } // namespace

    std::vector<Net24Song> Net24Source::Search(std::string const& keyword, int page) const
    {
        std::string const keywordTrimmed = TrimCopy(keyword);
        if (keywordTrimmed.empty())
        {
            return {};
        }
        if (m_baseUrl.empty())
        {
            return {};   // not configured; the UI shows the setup hint
        }

        // The keyword is sent *percent-encoded inside the JSON body* (the site
        // uses encodeURIComponent), not as a raw string.
        json::Object body;
        body["keyword"] = json::Value{ UrlEncode(keywordTrimmed) };
        body["page"] = json::Value(page);
        std::string const payload = Compact(json::Value{ body });

        std::string const one = HttpPost(m_baseUrl + "/api/player/searchOnlineMusicOne", payload);
        std::string const two = HttpPost(m_baseUrl + "/api/player/searchOnlineMusicTwo", payload);

        // Fold the two listings into one row per song, so a track present in
        // both catalogues gets all three quality buttons instead of two
        // half-rows. Rows keep their first-seen order (LinkedHashMap semantics).
        std::vector<Net24Song> merged;
        std::vector<std::string> keys;
        auto append = [&merged, &keys](std::vector<Net24ListRow> const& rows, Net24Origin origin) {
            for (Net24ListRow const& row : rows)
            {
                Net24Song song;
                song.title = row.title;
                song.artist = row.artist;
                song.album = row.album;
                song.coverUrl = row.cover;
                if (origin == Net24Origin::Master) song.masterId = row.id;
                else song.losslessId = row.id;

                std::string const key = NormKey(song.title) + std::string(1, '\0') + LeadArtist(song.artist);
                std::size_t index = 0;
                for (; index < keys.size(); ++index)
                {
                    if (keys[index] == key)
                    {
                        break;
                    }
                }
                if (index == keys.size())
                {
                    keys.push_back(key);
                    merged.push_back(std::move(song));
                }
                else
                {
                    merged[index].Merge(song);
                }
            }
        };
        append(ParseNet24SearchBody(one), Net24Origin::Master);
        append(ParseNet24SearchBody(two), Net24Origin::Lossless);
        return merged;
    }

    Net24Resolve Net24Source::Resolve(Net24Song const& song, Net24Quality quality) const
    {
        Net24Resolve result;
        result.tier = quality;

        std::string const id = song.QualityId(quality);
        if (id.empty())
        {
            // 这首歌在<origin>里没有<short>版本
            result.reason = std::string("\xE8\xBF\x99\xE9\xA6\x96\xE6\xAD\x8C\xE5\x9C\xA8") +
                Net24OriginLabel(Net24QualityOrigin(quality)) +
                "\xE9\x87\x8C\xE6\xB2\xA1\xE6\x9C\x89" + Net24QualityShort(quality) +
                "\xE7\x89\x88\xE6\x9C\xAC";
            return result;
        }

        Detail const detail = FetchDetail(Net24QualityType(quality), id);
        if (detail.url.empty())
        {
            // FetchDetail fills |reason| for fetch / quota problems; a bare
            // empty answer means the page parsed but carried no link.
            result.reason = !detail.reason.empty()
                ? detail.reason
                : "\xE7\xAB\x99\xE7\x82\xB9\xE6\xB2\xA1\xE6\x9C\x89\xE7\xBB\x99\xE5\x87\xBA\xE8\xBF\x99\xE9\xA6\x96\xE6\xAD\x8C\xE7\x9A\x84\xE7\x9B\xB4\xE9\x93\xBE\xEF\xBC\x8C\xE5\x8F\xAF\xE8\x83\xBD\xE5\xB7\xB2\xE4\xB8\x8B\xE6\x9E\xB6";
            return result;
        }

        // Name-check the answer against the row before offering it.
        std::string const wantTitle = NormKey(song.title);
        std::string const gotTitle = NormKey(detail.name);
        if (!wantTitle.empty() && !gotTitle.empty())
        {
            bool const titleOk = wantTitle == gotTitle ||
                                 wantTitle.find(gotTitle) != std::string::npos ||
                                 gotTitle.find(wantTitle) != std::string::npos;
            if (!titleOk)
            {
                result.reason = "\xE7\xAB\x99\xE7\x82\xB9\xE8\xBF\x94\xE5\x9B\x9E\xE7\x9A\x84\xE6\x98\xAF\xE3\x80\x8A" + detail.name +
                    "\xE3\x80\x8B\xEF\xBC\x8C\xE8\xAF\xA5\xE9\x9F\xB3\xE8\xB4\xA8\xE6\xA1\xA3\xE6\xB2\xA1\xE6\x9C\x89\xE8\xBF\x99\xE9\xA6\x96\xE6\xAD\x8C\xE7\x9A\x84\xE7\x89\x88\xE6\x9C\xAC";
                return result;
            }
            std::string const wantArtist = LeadArtist(song.artist);
            std::string const gotArtist = LeadArtist(detail.player);
            if (!wantArtist.empty() && !gotArtist.empty() &&
                wantArtist != gotArtist &&
                gotArtist.find(wantArtist) == std::string::npos &&
                wantArtist.find(gotArtist) == std::string::npos)
            {
                result.reason = "\xE7\xAB\x99\xE7\x82\xB9\xE8\xBF\x94\xE5\x9B\x9E\xE7\x9A\x84\xE6\x98\xAF " + detail.player +
                    " \xE7\x9A\x84\xE7\x89\x88\xE6\x9C\xAC\xEF\xBC\x8C\xE4\xB8\x8E\xE8\xBF\x99\xE9\xA6\x96\xE6\xAD\x8C\xE4\xB8\x8D\xE5\x8C\xB9\xE9\x85\x8D";
                return result;
            }
        }

        result.ok = true;
        result.download.url = detail.url;
        result.download.ext = detail.ext;
        result.download.quality = !detail.quality.empty()
            ? detail.quality
            : Net24QualityLabel(quality);
        result.download.sizeText = TrimCopy(detail.sizeText);
        result.download.sizeBytes = ParseNet24Size(result.download.sizeText);

        // fileNameFor: sanitised display name + extension, capped at 110 chars.
        std::string base;
        for (char const c : song.DisplayName())
        {
            bool const forbidden = std::strchr("\\/:*?\"<>|\r\n", c) != nullptr && c != '\0';
            base.push_back(forbidden ? '_' : c);
        }
        base = TrimCopy(base);
        if (base.size() > 110) base.resize(110);
        if (base.empty()) base = song.Id();
        result.download.fileName = base + "." + (!detail.ext.empty() ? detail.ext : "flac");
        return result;
    }

    Net24Resolve Net24Source::Preview(Net24Song const& song) const
    {
        // Lossless (1) < Surround (2) < Master (3): lightest tier first.
        auto const rank = [](Net24Quality quality) {
            switch (quality)
            {
            case Net24Quality::Lossless: return 1;
            case Net24Quality::Surround: return 2;
            case Net24Quality::Master: return 3;
            }
            return 9;
        };

        Net24Quality best = Net24Quality::Master;
        bool found = false;
        for (Net24Quality const quality : song.Qualities())
        {
            if (!found || rank(quality) < rank(best))
            {
                best = quality;
                found = true;
            }
        }
        if (!found)
        {
            Net24Resolve fail;
            fail.reason = "\xE8\xBF\x99\xE9\xA6\x96\xE6\xAD\x8C\xE6\x9A\x82\xE6\x97\xA0\xE5\x8F\xAF\xE7\x94\xA8\xE7\x9A\x84\xE9\x9F\xB3\xE8\xB4\xA8\xE7\x89\x88\xE6\x9C\xAC\xEF\xBC\x8C"
                          "\xE7\xAB\x99\xE7\x82\xB9\xE5\x8F\xAA\xE6\x8F\x90\xE4\xBE\x9B\xE6\x9C\xAA\xE6\x94\xB6\xE5\xBD\x95\xE7\x9A\x84\xE5\x85\xB6\xE5\xAE\x83\xE6\xA1\xA3";
            return fail;
        }
        return Resolve(song, best);
    }

    Net24Source::Detail Net24Source::FetchDetail(std::string const& type, std::string const& id) const
    {
        std::string const key = type + ":" + id;
        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            auto const found = m_cache.find(key);
            if (found != m_cache.end())
            {
                return found->second;
            }
        }

        Detail detail;   // empty url == failure; the reason is not cached
        if (m_baseUrl.empty())
        {
            detail.reason = "\xE6\x9C\xAA\xE9\x85\x8D\xE7\xBD\xAE\xE6\x97\xA0\xE6\x8D\x9F\xE7\xAB\x99\xE5\x9C\xB0\xE5\x9D\x80"; // 未配置无损站地址
            return detail;
        }

        HttpResponse const response = m_fetch([&] {
            HttpRequest request;
            request.url = m_baseUrl + "/music/" + type + "/" + id;
            request.method = "GET";
            request.headers = {
                { "User-Agent", kUa },
                { "Referer", m_baseUrl + "/" },
                { "Accept", "text/html,application/xhtml+xml,*/*" },
            };
            return request;
        }());
        if (response.status == 0)
        {
            detail.reason = "\xE8\xAF\xB7\xE6\xB1\x82\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x9A\xE7\xBD\x91\xE7\xBB\x9C\xE5\xBC\x82\xE5\xB8\xB8"; // 请求失败：网络异常
            return detail;
        }
        if (response.status < 200 || response.status > 299)
        {
            detail.reason = "\xE8\xAF\xB7\xE6\xB1\x82\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x9AHTTP " + std::to_string(response.status);
            return detail;
        }
        std::string const& html = response.body;
        if (html.empty())
        {
            detail.reason = "\xE8\xAF\xB7\xE6\xB1\x82\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x9A\xE7\xBD\x91\xE7\xBB\x9C\xE5\xBC\x82\xE5\xB8\xB8";
            return detail;
        }
        if (html.find(QuotaMark()) != std::string::npos)
        {
            // Daily quota exhausted; not cached, tomorrow it works again.
            detail.reason = "\xE6\x97\xA0\xE6\x8D\x9F\xE7\xAB\x99\xE4\xBB\x8A\xE6\x97\xA5\xE8\xAE\xBF\xE9\x97\xAE\xE9\xA2\x9D\xE5\xBA\xA6\xE5\xB7\xB2\xE7\x94\xA8\xE5\xAE\x8C\xEF\xBC\x8C"
                            "\xE6\x98\x8E\xE5\xA4\xA9\xE5\x86\x8D\xE6\x9D\xA5\xEF\xBC\x88\xE6\x88\x96\xE7\x99\xBB\xE5\xBD\x95\xE7\xAB\x99\xE7\x82\xB9\xE8\xB4\xA6\xE5\x8F\xB7\xEF\xBC\x89";
            return detail;
        }

        // The site is a Next.js SPA: its server-rendered page embeds a React
        // flight payload with an `itemMusic` object. The payload escapes
        // quotes (\") and line feeds (\n); unescape before reading fields.
        std::size_t const anchor = html.find("itemMusic");
        if (anchor == std::string::npos)
        {
            return detail;
        }
        std::string const frag = ReplaceAll(html.substr(anchor, std::min<std::size_t>(2200, html.size() - anchor)),
                                            "\\\"", "\"");

        auto const field = [&frag](char const* name) {
            std::regex const pattern(std::string("\"") + name + "\":\"([^\"]*)\"");
            std::smatch match;
            if (!std::regex_search(frag, match, pattern))
            {
                return std::string{};
            }
            return match[1].str();
        };

        auto const unescape = [](std::string const& text) {
            // The flight payload escapes & as \u0026 and / as \/.
            std::string out = ReplaceAll(text, "\\u0026", "&");
            return ReplaceAll(out, "\\/", "/");
        };

        std::string const url = unescape(field("url"));
        if (url.empty() || !StartsWith(url, "http"))
        {
            return detail;   // nothing usable -- probably delisted
        }
        std::string const format = field("format");
        detail.url = url;
        detail.name = unescape(field("name"));
        detail.player = unescape(field("player"));
        detail.quality = field("quality");
        detail.sizeText = TrimCopy(field("size"));
        detail.ext = ToLower(!format.empty() ? format : "flac");

        {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_cache[key] = detail;
        }
        return detail;
    }
} // namespace wm::core
