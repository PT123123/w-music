/// Offline tests for the pluggable online-source layer: adapter parsing /
/// validation, template rendering, JSON paths, regex extraction and the full
/// search -> resolve -> lyric pipeline driven by an in-memory fake transport.

#include <wm/core/ProviderAdapter.h>
#include <wm/core/ProviderEngine.h>

#include <cstdio>
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

    using namespace wm::core;

    // A tiny static site served by the fake transport.
    char const* kSearchHtml = R"HTML(
      <ul class="songs">
        <li class="song" data-id="101"><a class="t" href="/track/101">Blue Sky</a><span class="ar">Ada</span><span class="du">03:20</span></li>
        <li class="song" data-id="102"><a class="t" href="/track/102">Red Sea &amp; Sun</a><span class="ar">Bob</span><span class="du">4:05</span></li>
      </ul>
    )HTML";

    char const* kDetailHtml = R"HTML(
      <div id="player"><source src="//cdn.example.com/f/101.flac" type="audio/flac"></div>
      <a id="dl" href="/download/101/flac">下载</a>
    )HTML";

    char const* kSearchJson = R"JSON({
      "code": 0,
      "data": {
        "list": [
          { "id": 201, "name": "Neon", "artist": { "name": "Cy" }, "duration": 212, "cover": "/img/201.jpg" },
          { "id": 202, "name": "Mono", "artist": { "name": "Dee" }, "duration": 180, "cover": "/img/202.jpg" }
        ]
      }
    })JSON";

    char const* kAdapterHtml = R"JSON({
      "id": "demo-html",
      "name": "Demo HTML source",
      "baseUrl": "https://demo.example",
      "headers": { "User-Agent": "w-music/1.0", "Referer": "https://demo.example/" },
      "charset": "utf-8",
      "minIntervalMs": 500,
      "search": {
        "url": "/search?q={query_enc}",
        "itemPattern": "<li class=\"song\"[^>]*>(.*?)</li>",
        "fields": [
          { "key": "id", "source": "regex", "pattern": "data-id=\"(\\d+)\"", "required": true },
          { "key": "title", "source": "regex", "pattern": "class=\"t\"[^>]*>([^<]+)<" },
          { "key": "detail", "source": "regex", "pattern": "href=\"([^\"]+)\"" },
          { "key": "artist", "source": "regex", "pattern": "class=\"ar\">([^<]+)<" },
          { "key": "duration", "source": "regex", "pattern": "class=\"du\">([^<]+)<" }
        ]
      },
      "detail": {
        "url": "{detail}",
        "fields": [
          { "key": "play", "source": "regex", "pattern": "<source src=\"([^\"]+)\"" },
          { "key": "download", "source": "regex", "pattern": "id=\"dl\" href=\"([^\"]+)\"" }
        ]
      },
      "detailRequired": true
    })JSON";

    char const* kAdapterJson = R"JSON({
      "id": "demo-json",
      "name": "Demo JSON API",
      "baseUrl": "https://api.example/v1",
      "search": {
        "url": "/search?keyword={query_enc}&limit=20",
        "listPath": "data.list",
        "fields": [
          { "key": "id", "source": "json", "pattern": "id", "required": true },
          { "key": "title", "source": "json", "pattern": "name" },
          { "key": "artist", "source": "json", "pattern": "artist.name" },
          { "key": "duration", "source": "json", "pattern": "duration" },
          { "key": "cover", "source": "json", "pattern": "cover" },
          { "key": "download", "source": "static", "value": "https://api.example/file/{id}" }
        ]
      }
    })JSON";

    // A site whose search returns a top-level array of releases, each holding a
    // nested "files" array with one entry per downloadable format (zip archive,
    // full flac, short mp3 preview). Non-audio files carry no duration.
    char const* kNestJson = R"JSON([
      {
        "upload_id": 71177, "user_name": "Ada", "upload_name": "Anxiety",
        "tags": ["ambient", "jazz"],
        "files": [
          { "file_id": 1, "file_nicname": "zip", "file_format_info": { "media-type": "archive" },
            "download_url": "https://cc.example/content/a.zip" },
          { "file_id": 2, "file_nicname": "flac", "file_format_info": { "media-type": "audio", "ps": "0:32" },
            "download_url": "https://cc.example/content/a.flac" },
          { "file_id": 3, "file_nicname": "Preview", "file_format_info": { "media-type": "audio", "ps": "0:32" },
            "download_url": "https://cc.example/content/a.mp3" }
        ]
      },
      {
        "upload_id": 71178, "user_name": ["Bob", "Cyd"], "upload_name": "Infinity",
        "tags": ["electronic"],
        "files": [
          { "file_id": 4, "file_nicname": "flac", "file_format_info": { "media-type": "audio", "ps": "212" },
            "download_url": "https://cc.example/content/b.flac" }
        ]
      }
    ])JSON";

    char const* kAdapterNest = R"JSON({
      "id": "demo-nest",
      "name": "Demo nested source",
      "baseUrl": "https://cc.example",
      "preview": "cache",
      "headers": { "User-Agent": "w-music/1.0" },
      "search": {
        "url": "/api/query?tags={query_enc}",
        "listPath": "$",
        "fields": [
          { "key": "id",     "source": "json", "pattern": "upload_id", "required": true },
          { "key": "title",  "source": "json", "pattern": "upload_name" },
          { "key": "artist", "source": "json", "pattern": "user_name" },
          { "key": "tags",   "source": "json", "pattern": "tags" }
        ]
      },
      "detail": {
        "url": "/api/query?ids={id}",
        "listPath": "$",
        "flattenPath": "files",
        "fields": [
          { "key": "id",       "source": "json", "pattern": "file_id" },
          { "key": "title",    "source": "json", "pattern": "upload_name" },
          { "key": "kind",     "source": "json", "pattern": "file_nicname" },
          { "key": "duration", "source": "json", "pattern": "file_format_info.ps", "required": true },
          { "key": "download", "source": "json", "pattern": "download_url" },
          { "key": "play",     "source": "json", "pattern": "download_url" }
        ]
      },
      "detailRequired": true
    })JSON";

    HttpResponse Route(HttpRequest const& request)
    {
        HttpResponse response;
        response.status = 200;
        if (request.url.find("cc.example") != std::string::npos)
        {
            response.body = kNestJson;
        }
        else if (request.url.find("/search?") != std::string::npos)
        {
            response.body = request.url.find("api.example") != std::string::npos ? kSearchJson : kSearchHtml;
        }
        else if (request.url.find("/track/") != std::string::npos)
        {
            response.body = kDetailHtml;
        }
        else
        {
            response.status = 404;
        }
        return response;
    }

    void TestHelpers()
    {
        CHECK(UrlEncode("a b&c") == "a%20b%26c");
        CHECK(UrlEncode("周杰伦") == "%E5%91%A8%E6%9D%B0%E4%BC%A6");

        std::map<std::string, std::string> vars{ { "query", "a b" }, { "id", "7" } };
        CHECK(RenderTemplate("/s?q={query}&id={id}", vars) == "/s?q=a b&id=7");
        CHECK(RenderTemplate("/s?q={missing}", vars) == "/s?q=");
        CHECK(RenderTemplate("plain", vars) == "plain");

        CHECK(ParseDurationSeconds("03:45") == 225);
        CHECK(ParseDurationSeconds("4:05") == 245);
        CHECK(ParseDurationSeconds("1:02:03") == 3723);
        CHECK(ParseDurationSeconds("212") == 212);
        CHECK(FormatDuration(225) == "03:45");

        CHECK(HtmlDecode("Red Sea &amp; Sun") == "Red Sea & Sun");
        CHECK(HtmlDecode("a&#39;b") == "a'b");
        CHECK(HtmlDecode("no entities") == "no entities");

        CHECK(AbsoluteUrl("https://demo.example/search?q=1", "/track/101") == "https://demo.example/track/101");
        CHECK(AbsoluteUrl("https://demo.example/search?q=1", "//cdn.example.com/f.flac") == "https://cdn.example.com/f.flac");
        CHECK(AbsoluteUrl("https://demo.example/a/b/c.html", "x.flac") == "https://demo.example/a/b/x.flac");
        CHECK(AbsoluteUrl("https://demo.example", "https://other.example/x") == "https://other.example/x");

        CHECK(RegexFirst("data-id=\"101\"", "data-id=\"(\\d+)\"", 1) == "101");
        CHECK(RegexAll("<li>a</li><li>b</li>", "<li>(.*?)</li>", 1).size() == 2);
        CHECK(RegexAll("abc", "([0-9]+)", 1).empty());
    }

    void TestJsonPath()
    {
        std::string error;
        auto parsed = json::Parse(kSearchJson, &error);
        CHECK(parsed.has_value());
        if (!parsed.has_value())
        {
            return;
        }
        json::Value const* list = JsonAt(*parsed, "data.list");
        CHECK(list != nullptr && list->isArray() && list->asArray().size() == 2);
        json::Value const* first = JsonAt(*parsed, "data.list[0].artist.name");
        CHECK(first != nullptr && JsonText(*first) == "Cy");
        json::Value const* missing = JsonAt(*parsed, "data.list[9].name");
        CHECK(missing == nullptr);

        // "$" addresses the document root (top-level array responses).
        json::Value const* root = JsonAt(*parsed, "$");
        CHECK(root == &*parsed);
        auto arrayDoc = json::Parse(R"([{"n": 1}, {"n": 2}])", &error);
        CHECK(arrayDoc.has_value());
        json::Value const* rootArray = JsonAt(*arrayDoc, "$");
        CHECK(rootArray != nullptr && rootArray->isArray() && rootArray->asArray().size() == 2);
        json::Value const* dotted = JsonAt(*arrayDoc, "$.[1].n");
        CHECK(dotted != nullptr && JsonText(*dotted) == "2");

        // Array values flatten to a readable list instead of disappearing.
        auto joined = json::Parse(R"(["x", 1, {}, ["deep"]])", &error);
        CHECK(joined.has_value() && JsonText(*joined) == "x, 1, deep");
    }

    void TestAdapterParsing()
    {
        std::string error;
        auto html = ParseAdapter(kAdapterHtml, &error);
        CHECK(html.has_value());
        if (html.has_value())
        {
            CHECK(html->id == "demo-html");
            CHECK(html->baseUrl == "https://demo.example");
            CHECK(html->headers.at("User-Agent") == "w-music/1.0");
            CHECK(html->minIntervalMs == 500);
            CHECK(html->search.enabled && html->detail.enabled);
            CHECK(html->detailRequired);
            CHECK(html->search.fields.size() == 5);
        }

        auto jsonApi = ParseAdapter(kAdapterJson, &error);
        CHECK(jsonApi.has_value());
        if (jsonApi.has_value())
        {
            CHECK(jsonApi->search.listPath == "data.list");
            CHECK(jsonApi->search.fields.size() == 6);
        }

        CHECK(!ParseAdapter("{ not json", &error).has_value());
        CHECK(!ParseAdapter(R"({ "id": "x" })", &error).has_value());          // no baseUrl
        CHECK(error.find("baseUrl") != std::string::npos);
        CHECK(!ParseAdapter(R"({ "id": "x", "baseUrl": "https://a" })", &error).has_value()); // no search
        CHECK(!ParseAdapter(R"({ "id": "x", "baseUrl": "https://a", "search": { "url": "/s", "fields": [ { "key": "t", "source": "bogus" } ] } })", &error).has_value());
    }

    void TestHtmlPipeline()
    {
        std::string error;
        auto adapter = ParseAdapter(kAdapterHtml, &error);
        CHECK(adapter.has_value());
        if (!adapter.has_value())
        {
            return;
        }

        std::vector<HttpRequest> seen;
        ProviderEngine engine(*adapter, [&seen](HttpRequest const& request) {
            seen.push_back(request);
            return Route(request);
        });

        auto search = engine.Search("blue sky", 10);
        CHECK(search.ok);
        CHECK(search.tracks.size() == 2);
        CHECK(!seen.empty());
        CHECK(seen.front().url == "https://demo.example/search?q=blue%20sky");
        CHECK(seen.front().headers.at("Referer") == "https://demo.example/");

        if (search.tracks.size() == 2)
        {
            CHECK(search.tracks[0].id == "101");
            CHECK(search.tracks[0].title == "Blue Sky");
            CHECK(search.tracks[0].artist == "Ada");
            CHECK(search.tracks[0].durationSec == 200);
            CHECK(search.tracks[0].detailUrl == "https://demo.example/track/101");
            // HTML entities must be decoded.
            CHECK(search.tracks[1].title == "Red Sea & Sun");
            CHECK(search.tracks[1].durationSec == 245);
        }

        auto resolved = engine.Resolve(search.tracks[0]);
        CHECK(resolved.ok);
        CHECK(resolved.tracks.size() == 1);
        CHECK(resolved.tracks[0].playUrl == "https://cdn.example.com/f/101.flac");
        CHECK(resolved.tracks[0].downloadUrl == "https://demo.example/download/101/flac");
        CHECK(seen.size() == 2);
        CHECK(seen.back().url == "https://demo.example/track/101");

        auto lyric = engine.Lyric(search.tracks[0]);
        CHECK(!lyric.ok);   // adapter has no lyric step
    }

    void TestJsonPipeline()
    {
        std::string error;
        auto adapter = ParseAdapter(kAdapterJson, &error);
        CHECK(adapter.has_value());
        if (!adapter.has_value())
        {
            return;
        }

        ProviderEngine engine(*adapter, Route);
        auto search = engine.Search("neon", 1);
        CHECK(search.ok);
        CHECK(search.tracks.size() == 1);   // limit honoured
        if (!search.tracks.empty())
        {
            CHECK(search.tracks[0].id == "201");
            CHECK(search.tracks[0].title == "Neon");
            CHECK(search.tracks[0].artist == "Cy");
            CHECK(search.tracks[0].durationSec == 212);
            CHECK(search.tracks[0].durationText == "03:32");
            CHECK(search.tracks[0].coverUrl == "https://api.example/img/201.jpg");
            CHECK(search.tracks[0].sourceId == "demo-json");
        }

        // No detail step: resolve keeps whatever the search produced.
        auto resolved = engine.Resolve(search.tracks[0]);
        CHECK(resolved.ok);
    }

    void TestNestedPipeline()
    {
        std::string error;
        auto adapter = ParseAdapter(kAdapterNest, &error);
        CHECK(adapter.has_value());
        if (!adapter.has_value())
        {
            return;
        }
        CHECK(adapter->preview == "cache");
        CHECK(adapter->search.listPath == "$");
        CHECK(adapter->detail.flattenPath == "files");

        ProviderEngine engine(*adapter, Route);

        // Search rows come from the top-level array: one per release.
        auto search = engine.Search("jazz", 10);
        CHECK(search.ok);
        CHECK(search.tracks.size() == 2);
        if (search.tracks.size() == 2)
        {
            CHECK(search.tracks[0].id == "71177");
            CHECK(search.tracks[0].title == "Anxiety");
            CHECK(search.tracks[0].artist == "Ada");
            CHECK(search.tracks[0].extra.at("tags") == "ambient, jazz");
            CHECK(search.tracks[1].artist == "Bob, Cyd");   // string array joined
            CHECK(search.tracks[0].playUrl.empty());        // urls live in the nested files
        }

        // Resolve expands the nested files array; the archive entry has no
        // duration so it is skipped, and the flac (listed first) wins.
        auto resolved = engine.Resolve(search.tracks[0]);
        CHECK(resolved.ok);
        CHECK(resolved.tracks.size() == 1);
        CHECK(resolved.tracks[0].downloadUrl == "https://cc.example/content/a.flac");
        CHECK(resolved.tracks[0].playUrl == "https://cc.example/content/a.flac");
        CHECK(resolved.tracks[0].durationSec == 32);
        CHECK(resolved.tracks[0].durationText == "00:32");
        CHECK(resolved.tracks[0].extra.at("kind") == "flac");
        CHECK(resolved.tracks[0].id == "71177");            // file_id must not leak into the row id
        CHECK(resolved.tracks[0].title == "Anxiety");       // parent fallback supplies the title
    }

    void TestFailurePaths()
    {
        std::string error;
        auto adapter = ParseAdapter(kAdapterJson, &error);
        CHECK(adapter.has_value());
        if (!adapter.has_value())
        {
            return;
        }

        ProviderEngine noTransport(*adapter, nullptr);
        CHECK(!noTransport.Search("x").ok);

        ProviderEngine broken(*adapter, [](HttpRequest const&) { HttpResponse r; r.status = 503; return r; });
        auto failed = broken.Search("x");
        CHECK(!failed.ok);
        CHECK(failed.stage == "fetch");
        CHECK(failed.message == "HTTP 503");

        ProviderEngine empty(*adapter, [](HttpRequest const&) { HttpResponse r; r.status = 200; r.body = "{}"; return r; });
        auto nothing = empty.Search("x");
        CHECK(!nothing.ok);
        CHECK(nothing.stage == "parse");
    }
} // namespace

int main()
{
    TestHelpers();
    TestJsonPath();
    TestAdapterParsing();
    TestHtmlPipeline();
    TestJsonPipeline();
    TestNestedPipeline();
    TestFailurePaths();

    std::printf("provider: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
