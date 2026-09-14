/// Offline tests for the a-music replica sources (QqSource / Net24Source):
/// endpoint shapes, request headers, response parsing, catalogue merging,
/// detail-page scraping, quota + mismatch guards and the preview ranking --
/// all driven by an in-memory fake transport.

#include <wm/core/OnlineSources.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace
{
    int g_failed = 0;
    int g_checks = 0;

    void Check(bool condition, char const* what, int line)
    {
        ++g_checks;
        if (!condition)
        {
            ++g_failed;
            std::printf("FAIL line %d: %s\n", line, what);
        }
    }

#define CHECK(expr) Check((expr), #expr, __LINE__)
#define CHECK_CONTAINS(haystack, needle) Check(std::string(haystack).find(needle) != std::string::npos, std::string(needle).c_str(), __LINE__)

    using namespace wm::core;

    /// Canned site: every request is logged, responses are looked up by URL.
    struct FakeSite
    {
        std::vector<HttpRequest> log;
        std::map<std::string, HttpResponse> canned;
        /// Fallbacks matched by URL prefix (for URLs with a big query string).
        std::vector<std::pair<std::string, HttpResponse>> prefixCanned;

        HttpResponse operator()(HttpRequest const& request)
        {
            log.push_back(request);
            auto const it = canned.find(request.url);
            if (it != canned.end())
            {
                return it->second;
            }
            for (auto const& [prefix, response] : prefixCanned)
            {
                if (request.url.rfind(prefix, 0) == 0)
                {
                    return response;
                }
            }
            HttpResponse missing;
            missing.status = 404;
            return missing;
        }

        std::size_t CountUrl(std::string const& prefix) const
        {
            std::size_t n = 0;
            for (HttpRequest const& r : log)
            {
                if (r.url.rfind(prefix, 0) == 0)
                {
                    ++n;
                }
            }
            return n;
        }
    };

    // ------------------------------------------------------------ fixtures

    char const* kQqSearchJson = R"JSON({
      "code": 0,
      "data": { "song": { "list": [
        { "songmid": "0039MnYb0qxYhV", "songname": "晴天", "title": "ignored",
          "singer": [ { "name": "周杰伦" }, { "name": "杨瑞代" } ],
          "albumname": "叶惠美", "albummid": "002Neh8l0UCQ1z",
          "interval": 269, "size128": 4300000,
          "pay": { "payplay": 1 } },
        { "songmid": "002bBHWB3zLk1T", "songname": "稻香", "singer": [ { "name": "周杰伦" } ],
          "albumname": "魔杰座", "albummid": "002eFUFm2XYZ7z",
          "interval": 223, "size128": 3600000,
          "pay": { "payplay": 0 } }
      ] } }
    })JSON";

    char const* kQqVkeyJson = R"JSON({
      "code": 0,
      "req_0": { "code": 0, "data": {
        "sip": [ "http://aqqmusic.tc.qq.com/", "http://isp.aqqmusic.tc.qq.com/" ],
        "midurlinfo": [ { "songmid": "002bBHWB3zLk1T", "purl": "AM00003xxx.m4a?fromtag=0" } ]
      } }
    })JSON";

    /// The flight payload escapes quotes and ampersands exactly like the site.
    char const* kNet24DetailHtml =
        "<!DOCTYPE html><html><head>...today quota ok...</head><body>"
        "<script>self.__next_f.push([1,\"7:{\\\"itemMusic\\\":{\\\"url\\\":\\\"https://m804.music.example.net/2024/flac/abc123.flac\\\","
        "\\\"name\\\":\\\"晴天\\u0026Live\\\",\\\"player\\\":\\\"周杰伦\\\",\\\"quality\\\":\\\"无损音质\\\","
        "\\\"size\\\":\\\"198.47MB\\\",\\\"format\\\":\\\"FLAC\\\"}}\"])</script>"
        "</body></html>";

    char const* kNet24QuotaHtml = "<html><body>今日访问已达限额，可明日再来</body></html>";

    char const* kNet24EmptyHtml = "<html><body>song not found</body></html>";
} // namespace

int main()
{
    using namespace std::string_literals;

    // ===================================================================
    // == QQ 音乐                                                       ==
    // ===================================================================
    {
        FakeSite site;
        site.canned["https://c.y.qq.com/soso/fcgi-bin/client_search_cp"
                    "?w=%E6%99%B4%E5%A4%A9&format=json&n=20&p=1&cr=1&new_json=0&platform=wxforsong&needNewCode=0"] =
            { 200, kQqSearchJson, {} };
        QqSource source{ [&](HttpRequest const& r) { return site(r); } };

        // Empty keywords never hit the wire.
        CHECK(source.Search("   ").empty());
        CHECK(site.log.empty());

        auto const songs = source.Search("晴天");
        CHECK(site.log.size() == 1);
        CHECK(site.log.front().method == "GET");
        CHECK_CONTAINS(site.log.front().url,
                       "https://c.y.qq.com/soso/fcgi-bin/client_search_cp?w=%E6%99%B4%E5%A4%A9&format=json&n=20&p=1");
        CHECK_CONTAINS(site.log.front().url, "cr=1&new_json=0&platform=wxforsong&needNewCode=0");
        CHECK(site.log.front().headers.at("User-Agent").find("Chrome/124.0") != std::string::npos);
        CHECK(site.log.front().headers.at("Referer") == "https://y.qq.com/");

        CHECK(songs.size() == 2);
        CHECK(songs[0].mid == "0039MnYb0qxYhV");
        CHECK(songs[0].title == "晴天");           // songname wins over title
        CHECK(songs[0].artist == "周杰伦 / 杨瑞代");
        CHECK(songs[0].album == "叶惠美");
        CHECK(songs[0].albumMid == "002Neh8l0UCQ1z");
        CHECK(songs[0].durationSec == 269);
        CHECK(songs[0].sizeBytes == 4300000);
        CHECK(songs[0].vipOnly);
        CHECK(songs[0].CoverUrl() == "https://y.qq.com/music/photo_new/T002R300x300M000002Neh8l0UCQ1z.jpg");
        CHECK(songs[0].DisplayName() == "周杰伦 / 杨瑞代 - 晴天");
        CHECK(songs[0].DurationText() == "4:29");
        CHECK(songs[0].SizeText() == "4.1 MB");

        CHECK(!songs[1].vipOnly);
        CHECK(songs[1].DurationText() == "3:43");

        // Direct URL: relative purl gets the sip prefix. The vkey payload is
        // matched by prefix because the JSON object key order is irrelevant.
        site.canned.clear();
        site.log.clear();
        site.prefixCanned.push_back({ "https://u.y.qq.com/cgi-bin/musicu.fcg?format=json&data=",
                                      { 200, kQqVkeyJson, {} } });
        std::string const direct = source.DirectUrl("002bBHWB3zLk1T");
        CHECK(direct == "http://aqqmusic.tc.qq.com/AM00003xxx.m4a?fromtag=0");
        CHECK(site.log.size() == 1);
        CHECK(site.log.front().headers.at("Referer") == "https://y.qq.com/");
        // The payload must carry the module/method pair and the songmid.
        CHECK_CONTAINS(site.log.front().url, UrlEncode("vkey.GetVkeyServer"));
        CHECK_CONTAINS(site.log.front().url, UrlEncode("CgiGetVkey"));
        CHECK_CONTAINS(site.log.front().url, UrlEncode("002bBHWB3zLk1T"));
        CHECK_CONTAINS(site.log.front().url, UrlEncode("\"songtype\":[0]"));

        // An already absolute purl is returned untouched.
        site.prefixCanned.front().second.body =
            R"JSON({"req_0":{"data":{"midurlinfo":[{"purl":"https://dl.example.com/x.m4a"}]}}})JSON";
        CHECK(source.DirectUrl("002bBHWB3zLk1T") == "https://dl.example.com/x.m4a");

        // Empty purl -> "" (VIP-only / unavailable).
        site.prefixCanned.front().second.body = R"JSON({"req_0":{"data":{"midurlinfo":[{"purl":""}]}}})JSON";
        CHECK(source.DirectUrl("002bBHWB3zLk1T").empty());
        CHECK(source.DirectUrl("").empty());

        // Malformed answers degrade to "" instead of throwing.
        site.prefixCanned.front().second.body = "not json at all";
        CHECK(source.DirectUrl("002bBHWB3zLk1T").empty());
        site.prefixCanned.clear();
        CHECK(source.DirectUrl("002bBHWB3zLk1T").empty());

        auto const headers = QqSource::DownloadHeaders();
        CHECK(headers.at("Referer") == "https://y.qq.com/");
        CHECK(headers.at("User-Agent").find("Chrome") != std::string::npos);
        CHECK(headers.at("Accept") == "*/*");

        CHECK(QqSource::HotWords().size() == 10);
        CHECK(QqSource::HotWords().front() == "周杰伦");
    }

    // ===================================================================
    // == QQ 音乐: lyrics + login session                               ==
    // ===================================================================
    {
        FakeSite site;
        QqSource source{ [&](HttpRequest const& r) { return site(r); } };

        // Lyric endpoint shape + payload decoding (nobase64=1 => plain LRC).
        site.canned["https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg"
                    "?songmid=002bBHWB3zLk1T&g_tk=5381&format=json&inCharset=utf8&outCharset=utf-8&nobase64=1"] =
            { 200, R"JSON({"retcode":0,"lyric":"[00:00.00]稻香"})JSON", {} };
        CHECK(source.Lyric("002bBHWB3zLk1T") == "[00:00.00]稻香");
        CHECK(site.log.front().headers.at("Referer") == "https://y.qq.com/");

        // Missing lyric -> empty, no throw.
        site.canned["https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg"
                    "?songmid=nolyric&g_tk=5381&format=json&inCharset=utf8&outCharset=utf-8&nobase64=1"] =
            { 200, R"JSON({"retcode":0,"lyric":""})JSON", {} };
        CHECK(source.Lyric("nolyric").empty());
        CHECK(source.Lyric("").empty());

        // Installed session cookie is forwarded on every request.
        site.canned.clear();
        site.log.clear();
        source.SetSession("uin=o12345; p_skey=abc; qrsig=xyz", "12345");
        site.prefixCanned.push_back({ "https://c.y.qq.com/lyric/fcgi-bin/fcg_query_lyric_new.fcg",
                                      { 200, R"JSON({"retcode":0,"lyric":"x"})JSON", {} } });
        CHECK(source.Lyric("002bBHWB3zLk1T") == "x");
        CHECK_CONTAINS(site.log.back().headers.at("Cookie"), "uin=o12345; p_skey=abc; qrsig=xyz");
        CHECK(source.Uin() == "12345");
    }

    // ===================================================================
    // == QQ 音乐: 扫码登录流程                                         ==
    // ===================================================================
    {
        // hash33 matches the known web player vectors.
        CHECK(QqLoginFlow::Hash33("") == "0");
        CHECK(QqLoginFlow::Hash33("qrsigtest") == std::to_string([] {
            std::int64_t n = 0;
            for (unsigned char const c : std::string("qrsigtest"))
            {
                n = ((n << 5) + n + c) & 0x7FFFFFFF;
            }
            return n;
        }()));
        // A value that overflows 32-bit accumulation several times.
        CHECK(QqLoginFlow::Hash33("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") ==
              std::to_string([] {
                  std::int64_t n = 0;
                  for (unsigned char const c : std::string("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))
                  {
                      n = ((n << 5) + n + c) & 0x7FFFFFFF;
                  }
                  return n;
              }()));

        FakeSite site;
        QqLoginFlow flow{ [&](HttpRequest const& r) { return site(r); } };

        // FetchQr: QR body + qrsig from Set-Cookie, even when multiple
        // Set-Cookie values are folded into one header with newlines.
        HttpResponse qr;
        qr.status = 200;
        qr.body = "JPG_BYTES_0123456789";
        qr.headers["Set-Cookie"] = "qrsig=abc123; Path=/; HttpOnly\nuin=o0; Path=/";
        site.canned["https://ssl.ptlogin2.qq.com/ptqrshow?appid=716027609&e=2&l=M&s=3&d=72&v=4&t=0.1&daid=383&pt_3rd_aid=100497308&u1=https%3A%2F%2Fy.qq.com%2Fportal%2Fwplayer.html"] = qr;

        auto const ctx = flow.FetchQr();
        CHECK(ctx.ok);
        CHECK(ctx.qrImage == "JPG_BYTES_0123456789");
        CHECK(ctx.qrsig == "abc123");
        CHECK(ctx.ptqrtoken == QqLoginFlow::Hash33("abc123"));

        // CheckStatus 65 -> Waiting.
        HttpResponse waiting;
        waiting.status = 200;
        waiting.body = "ptuiCB('65','0','https://ssl.ptlogin2.qq.com/check',0,'二维码未失效','',0,0,0);";
        site.prefixCanned.push_back({ "https://ssl.ptlogin2.qq.com/ptqrlogin?u1=", waiting });
        auto const w = flow.CheckStatus(ctx);
        CHECK(w.status == QqLoginStatus::Waiting);
        CHECK(w.cookie.empty());

        // CheckStatus 66 -> Scanned.
        site.prefixCanned.front().second.body =
            "ptuiCB('66','0','https://ssl.ptlogin2.qq.com/check',0,'二维码认证中','',0,0,0);";
        CHECK(flow.CheckStatus(ctx).status == QqLoginStatus::Scanned);

        // CheckStatus 0 -> Success with the accumulated login cookies.
        HttpResponse ok;
        ok.status = 200;
        ok.body = "ptuiCB('0','12345','https://ssl.ptlogin2.qq.com/check',0,'登录成功！','周杰伦',0,0,0);";
        ok.headers["Set-Cookie"] =
            "p_skey=PSKEY123; Path=/; HttpOnly\nuin=o12345; Path=/\nskey=@SKKEY456; Path=/";
        site.prefixCanned.front().second = ok;
        auto const okResult = flow.CheckStatus(ctx);
        CHECK(okResult.status == QqLoginStatus::Success);
        CHECK(okResult.uin == "12345");
        CHECK_CONTAINS(okResult.cookie, "p_skey=PSKEY123");
        CHECK_CONTAINS(okResult.cookie, "uin=o12345");
        CHECK_CONTAINS(okResult.cookie, "skey=@SKKEY456");

        // The polling request itself carries qrsig back.
        auto const pollUrl = site.log.back().url;
        CHECK_CONTAINS(pollUrl, "ptqrtoken=" + ctx.ptqrtoken);
        CHECK_CONTAINS(pollUrl, "qrsig=abc123");

        // 67 -> Failed with the expiry message.
        site.prefixCanned.front().second.body = "ptuiCB('67','0','','','二维码已失效','',0,0,0);";
        auto const expired = flow.CheckStatus(ctx);
        CHECK(expired.status == QqLoginStatus::Failed);
        CHECK_CONTAINS(expired.reason, "过期");

        // Unparseable / missing context never throws.
        site.prefixCanned.front().second.body = "garbage";
        CHECK(flow.CheckStatus(ctx).status == QqLoginStatus::Failed);
        QqLoginContext empty;
        CHECK(flow.CheckStatus(empty).status == QqLoginStatus::Failed);
    }

    // ===================================================================
    // == 无损站: search + catalogue merge                              ==
    // ===================================================================
    {
        FakeSite site;
        Net24Source source{ "https://lossless.example", [&](HttpRequest const& r) { return site(r); } };

        // Empty keyword / unconfigured base -> no requests.
        CHECK(source.Search("").empty());

        site.canned["https://lossless.example/api/player/searchOnlineMusicOne"] =
            { 200, R"JSON({"status":true,"result":[
                { "id": "9001", "cover": "https://img.example/9001.jpg", "name": "晴天 (Live)",
                  "player": "周杰伦&杨瑞代", "album": "叶惠美" },
                { "id": "9002", "cover": "", "name": "孤勇者", "player": "陈奕迅", "album": "" }
              ]})JSON", {} };
        site.canned["https://lossless.example/api/player/searchOnlineMusicTwo"] =
            { 200, R"JSON({"status":true,"result":[
                { "id": "77001", "cover": "https://img.example/77001.jpg", "name": "晴天(Live)",
                  "player": "周杰伦", "album": "演唱会" },
                { "id": "77002", "cover": "", "name": "孤勇者", "player": "陈奕迅", "album": "孤勇者单曲" }
              ]})JSON", {} };

        auto const songs = source.Search("晴天");
        CHECK(site.log.size() == 2);
        // Same JSON body on both endpoints, keyword percent-encoded *inside* it.
        for (HttpRequest const& r : site.log)
        {
            CHECK(r.method == "POST");
            CHECK_CONTAINS(r.body, "{\"keyword\":\"%E6%99%B4%E5%A4%A9\",\"page\":1}");
            CHECK(r.headers.at("Content-Type") == "application/json");
            CHECK(r.headers.at("Origin") == "https://lossless.example");
            CHECK(r.headers.at("Referer") == "https://lossless.example/");
        }
        // "晴天 (Live) / 周杰伦&杨瑞代" and "晴天(Live) / 周杰伦" fold into one row.
        CHECK(songs.size() == 2);
        CHECK(songs[0].masterId == "9001");
        CHECK(songs[0].losslessId == "77001");
        CHECK(songs[0].album == "叶惠美");                       // first seen wins
        CHECK(songs[0].coverUrl == "https://img.example/9001.jpg");
        CHECK(songs[0].SourceLabel() == "母带源 + 无损源");
        CHECK(songs[0].Qualities().size() == 3);
        CHECK(songs[0].Key(Net24Quality::Lossless) == "77001:b");
        CHECK(songs[0].Id() == "9001");

        CHECK(songs[1].masterId == "9002");
        CHECK(songs[1].losslessId == "77002");
        CHECK(songs[1].album == "孤勇者单曲");

        // status=false and malformed bodies yield nothing.
        site.canned["https://lossless.example/api/player/searchOnlineMusicOne"] =
            { 200, R"JSON({"status":false,"result":[]})JSON", {} };
        site.canned["https://lossless.example/api/player/searchOnlineMusicTwo"] = { 200, "garbage", {} };
        CHECK(source.Search("晴天").empty());
        site.canned.clear();
        CHECK(source.Search("晴天").empty());
    }

    // ===================================================================
    // == 无损站: detail resolve                                        ==
    // ===================================================================
    // The session cache is per Net24Source instance, so each scenario below
    // builds a fresh one around the same fake site.
    {
        FakeSite site;
        std::string const detailUrl = "https://lossless.example/music/b/77001";
        site.canned[detailUrl] = { 200, kNet24DetailHtml, {} };

        Net24Song song;
        song.title = "晴天";
        song.artist = "周杰伦";
        song.masterId = "9001";
        song.losslessId = "77001";

        {
            Net24Source source{ "https://lossless.example", [&](HttpRequest const& r) { return site(r); } };
            auto resolve = source.Resolve(song, Net24Quality::Lossless);
            CHECK(resolve.ok);
            CHECK(resolve.tier == Net24Quality::Lossless);
            CHECK(resolve.download.url == "https://m804.music.example.net/2024/flac/abc123.flac");
            CHECK(resolve.download.ext == "flac");            // lowercased format
            CHECK(resolve.download.quality == "无损音质");
            CHECK(resolve.download.sizeText == "198.47MB");
            CHECK(resolve.download.sizeBytes == static_cast<std::int64_t>(198.47 * 1024 * 1024));
            CHECK(resolve.download.fileName == "周杰伦 - 晴天.flac");

            // The detail request carries the site's browser-ish headers.
            CHECK(site.log.back().url == detailUrl);
            CHECK(site.log.back().headers.at("Referer") == "https://lossless.example/");
            CHECK(site.log.back().headers.at("User-Agent").find("Chrome/124.0") != std::string::npos);

            // Successful lookups are cached: the second ask costs no quota.
            (void)source.Resolve(song, Net24Quality::Lossless);
            CHECK(site.CountUrl("https://lossless.example/music/b/77001") == 1);
        }

        // Mismatched song names are refused (id pairing protection).
        {
            site.canned[detailUrl].body =
                R"HTML(itemMusic {"url":"https://cdn.example.net/x.flac","name":"夜曲","player":"周杰伦","quality":"无损音质","size":"31.20MB","format":"flac"})HTML";
            Net24Source source{ "https://lossless.example", [&](HttpRequest const& r) { return site(r); } };
            auto resolve = source.Resolve(song, Net24Quality::Lossless);
            CHECK(!resolve.ok);
            CHECK_CONTAINS(resolve.reason, "夜曲");
            CHECK_CONTAINS(resolve.reason, "该音质档没有这首歌的版本");
        }

        // Different artist is refused too.
        {
            site.canned[detailUrl].body =
                R"HTML(itemMusic {"url":"https://cdn.example.net/x.flac","name":"晴天","player":"林俊杰","quality":"无损音质","size":"31.20MB","format":"flac"})HTML";
            Net24Source source{ "https://lossless.example", [&](HttpRequest const& r) { return site(r); } };
            auto resolve = source.Resolve(song, Net24Quality::Lossless);
            CHECK(!resolve.ok);
            CHECK_CONTAINS(resolve.reason, "林俊杰");
            CHECK_CONTAINS(resolve.reason, "与这首歌不匹配");
        }

        // Quota page -> dedicated message, and not cached.
        {
            site.canned[detailUrl].body = kNet24QuotaHtml;
            Net24Source source{ "https://lossless.example", [&](HttpRequest const& r) { return site(r); } };
            auto const before = site.CountUrl(detailUrl);
            auto resolve = source.Resolve(song, Net24Quality::Lossless);
            CHECK(!resolve.ok);
            CHECK_CONTAINS(resolve.reason, "今日访问额度已用完");
            // Not cached: a retry is a fresh request.
            (void)source.Resolve(song, Net24Quality::Lossless);
            CHECK(site.CountUrl(detailUrl) - before == 2);
        }

        // A tier the row does not carry fails with the per-origin message.
        {
            Net24Source source{ "https://lossless.example", [&](HttpRequest const& r) { return site(r); } };
            Net24Song losslessOnly;
            losslessOnly.title = "孤勇者";
            losslessOnly.artist = "陈奕迅";
            losslessOnly.losslessId = "77002";
            auto resolve = source.Resolve(losslessOnly, Net24Quality::Master);
            CHECK(!resolve.ok);
            CHECK_CONTAINS(resolve.reason, "母带源");
            CHECK_CONTAINS(resolve.reason, "没有母带版本");
        }

        // Page without itemMusic -> no direct link.
        {
            site.canned["https://lossless.example/music/a/9001"] = { 200, kNet24EmptyHtml, {} };
            Net24Source source{ "https://lossless.example", [&](HttpRequest const& r) { return site(r); } };
            auto resolve = source.Resolve(song, Net24Quality::Master);
            CHECK(!resolve.ok);
            CHECK_CONTAINS(resolve.reason, "直链");
        }

        // Transport failure surfaces as 请求失败.
        {
            site.canned.clear();
            Net24Source source{ "https://lossless.example", [&](HttpRequest const& r) { return site(r); } };
            auto resolve = source.Resolve(song, Net24Quality::Lossless);
            CHECK(!resolve.ok);
            CHECK_CONTAINS(resolve.reason, "请求失败");
        }

        // Unconfigured base URL fails fast with a setup hint.
        {
            Net24Source unconfigured{ "", [&](HttpRequest const& r) { return site(r); } };
            auto resolve = unconfigured.Resolve(song, Net24Quality::Lossless);
            CHECK(!resolve.ok);
            CHECK_CONTAINS(resolve.reason, "未配置");
            CHECK(unconfigured.Search("晴天").empty());
        }
    }

    // ===================================================================
    // == 无损站: preview ranking                                       ==
    // ===================================================================
    {
        FakeSite site;
        // 无损 (b) is the lightest tier: it must be picked for preview.
        site.canned["https://lossless.example/music/b/77001"] = { 200, kNet24DetailHtml, {} };

        Net24Source source{ "https://lossless.example", [&](HttpRequest const& r) { return site(r); } };
        Net24Song song;
        song.title = "晴天";
        song.artist = "周杰伦";
        song.masterId = "9001";
        song.losslessId = "77001";

        auto preview = source.Preview(song);
        CHECK(preview.ok);
        CHECK(preview.tier == Net24Quality::Lossless);
        CHECK(site.CountUrl("https://lossless.example/music/b/77001") == 1);
        CHECK(site.CountUrl("https://lossless.example/music/a/9001") == 0);

        // Master-only rows fall through to 环绕 (c).
        site.canned["https://lossless.example/music/c/9001"] = { 200, kNet24DetailHtml, {} };
        Net24Song masterOnly;
        masterOnly.title = "晴天";
        masterOnly.masterId = "9001";
        preview = source.Preview(masterOnly);
        CHECK(preview.ok);
        CHECK(preview.tier == Net24Quality::Surround);

        // No tier at all -> explicit failure.
        Net24Song nothing;
        nothing.title = "未收录";
        preview = source.Preview(nothing);
        CHECK(!preview.ok);
        CHECK_CONTAINS(preview.reason, "暂无可用的音质版本");
    }

    // ===================================================================
    // == helpers                                                       ==
    // ===================================================================
    {
        CHECK(ParseNet24Size("198.47MB") == static_cast<std::int64_t>(198.47 * 1024 * 1024));
        CHECK(ParseNet24Size("21.57 MB") == static_cast<std::int64_t>(21.57 * 1024 * 1024));
        CHECK(ParseNet24Size("1.5GB") == static_cast<std::int64_t>(1.5 * 1024 * 1024 * 1024));
        CHECK(ParseNet24Size("800KB") == 800 * 1024);
        CHECK(ParseNet24Size("512B") == 512);
        CHECK(ParseNet24Size("") == 0);
        CHECK(ParseNet24Size("unknown") == 0);

        // net24 download headers key off the configured base URL.
        FakeSite site;
        Net24Source source{ "https://lossless.example", [&](HttpRequest const& r) { return site(r); } };
        auto const headers = source.DownloadHeaders();
        CHECK(headers.at("Referer") == "https://lossless.example/");
        CHECK(headers.at("User-Agent").find("Chrome") != std::string::npos);
        CHECK(headers.at("Accept") == "*/*");

        CHECK(Net24Source::HotWords().size() == 16);
        CHECK(Net24Source::HotWords()[9] == "邓丽君");

        // Trailing slashes are trimmed from the base URL.
        Net24Source slashed{ "https://lossless.example///", [&](HttpRequest const& r) { return site(r); } };
        CHECK(slashed.DownloadHeaders().at("Referer") == "https://lossless.example/");
    }

    std::printf("online sources: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
