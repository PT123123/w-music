/// Offline tests for the 个性推荐 disk cache: freshness / fingerprint matching,
/// the stale-but-displayable fallback, payload and entry bounds, persistence
/// round-trip and recovery from a corrupt file.

#include <wm/core/RecommendCache.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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

    constexpr std::int64_t kNow = 1'777'000'000'000; // fixed clock: tests must be deterministic
    constexpr std::int64_t kTtl = 5 * 60 * 1000;         // the ttl the page happens to ask with
    constexpr char const* kFile = "test_recommend_cache.json";

    std::string TempPath()
    {
        return (std::filesystem::current_path() / kFile).string();
    }

    void TestFreshness()
    {
        std::printf("[Freshness]\n");
        RecommendCache cache;
        CHECK(cache.Size() == 0, "starts empty");
        CHECK(cache.Find("cat:rock", "lib:1", kNow, kTtl) == nullptr, "miss on empty");
        CHECK(cache.FindStale("cat:rock", "lib:1", kNow) == nullptr, "stale miss on empty");

        cache.Put("cat:rock", "lib:1", R"({"recommendations":[]})", kNow);
        CHECK(cache.Size() == 1, "one entry after Put");

        auto const* hit = cache.Find("cat:rock", "lib:1", kNow, kTtl);
        CHECK(hit != nullptr, "fresh hit inside ttl");
        CHECK(hit != nullptr && hit->payload == R"({"recommendations":[]})", "payload kept verbatim");
        CHECK(hit != nullptr && hit->fetchedAt == kNow, "fetchedAt kept");

        auto const pastTtl = kNow + kTtl + 1;
        CHECK(cache.Find("cat:rock", "lib:1", pastTtl, kTtl) == nullptr, "expired for Find");
        CHECK(cache.FindStale("cat:rock", "lib:1", pastTtl) != nullptr, "still paintable as stale");

        auto const ancient = kNow + RecommendCache::kMaxUsableMs + 1;
        CHECK(cache.FindStale("cat:rock", "lib:1", ancient) == nullptr, "too old to paint");

        // A clock in the future relative to the entry must not look negative-fresh.
        cache.Put("clock", "lib:1", "{}", kNow);
        CHECK(cache.Find("clock", "lib:1", kNow - kTtl - 5, kTtl) != nullptr,
              "entry from the future still counts as fresh");
    }

    void TestFingerprint()
    {
        std::printf("[Fingerprint]\n");
        RecommendCache cache;
        cache.Put("categories", "lib:93:k4", "[1,2,3]", kNow);
        CHECK(cache.Find("categories", "lib:93:k4", kNow, kTtl) != nullptr, "same library hits");
        CHECK(cache.Find("categories", "lib:120:k5", kNow, kTtl) == nullptr,
              "re-analyzed library misses");
        CHECK(cache.FindStale("categories", "lib:120:k5", kNow) == nullptr,
              "an answer for another library is never painted");
    }

    void TestBounds()
    {
        std::printf("[Bounds]\n");
        RecommendCache cache;
        cache.Put("empty", "fp", "", kNow);
        CHECK(cache.Size() == 0, "empty payload is not stored");

        cache.Put("huge", "fp", std::string(RecommendCache::kMaxPayloadBytes + 1, 'x'), kNow);
        CHECK(cache.Size() == 0, "oversized payload is not stored");

        for (std::size_t i = 0; i < RecommendCache::kMaxEntries + 10; ++i)
        {
            cache.Put("q" + std::to_string(i), "fp", "{\"n\":" + std::to_string(i) + "}",
                      kNow + static_cast<std::int64_t>(i));
        }
        CHECK(cache.Size() == RecommendCache::kMaxEntries, "entry count is capped");
        CHECK(cache.FindStale("q0", "fp", kNow + 1'000'000) == nullptr, "oldest entries were evicted");
        CHECK(cache.FindStale("q" + std::to_string(RecommendCache::kMaxEntries + 9), "fp", kNow + 1'000'000) != nullptr,
              "the newest entry survived eviction");

        // Re-putting a key replaces it instead of growing the file.
        auto const before = cache.Size();
        cache.Put("q1", "fp", "{\"replaced\":true}", kNow);
        CHECK(cache.Size() == before, "Put on an existing key does not grow");
        auto const* replaced = cache.Find("q1", "fp", kNow, kTtl);
        CHECK(replaced != nullptr && replaced->payload == "{\"replaced\":true}", "value replaced");
    }

    void TestPersistence()
    {
        std::printf("[Persistence]\n");
        auto const path = TempPath();
        std::error_code ec;
        std::filesystem::remove(path, ec);

        RecommendCache cache;
        cache.Put("cat:rock", "lib:93:k4", R"({"category":"摇滚","recommendations":[]})", kNow);
        cache.Put("discovery", "lib:93:k4", "{\"meta\":{\"k\":4}}", kNow - 1000);
        CHECK(cache.Save(path), "save ok");

        RecommendCache loaded;
        std::string error;
        CHECK(loaded.Load(path, &error), "load ok");
        CHECK(error.empty(), "load reported no error");
        CHECK(loaded.Size() == 2, "both entries came back");
        auto const* hit = loaded.Find("cat:rock", "lib:93:k4", kNow, kTtl);
        CHECK(hit != nullptr, "fresh hit after reload");
        CHECK(hit != nullptr && hit->payload == R"({"category":"摇滚","recommendations":[]})", "UTF-8 payload survived the round trip");

        // Corrupt file: no throw, empty cache, and the app can still save over it.
        {
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            out << "{\"version\":1,\"entries\":[{\"key\":";
        }
        RecommendCache broken;
        CHECK(!broken.Load(path), "corrupt file loads as empty");
        CHECK(broken.Size() == 0, "corrupt file leaves no partial entries");
        broken.Put("x", "fp", "{}", kNow);
        CHECK(broken.Save(path), "save works over a corrupt file");

        RecommendCache repaired;
        CHECK(repaired.Load(path), "repaired file loads");
        CHECK(repaired.Size() == 1, "only the new entry is present");

        std::filesystem::remove(path, ec);
        RecommendCache missing;
        CHECK(!missing.Load(path, &error), "missing file is not an error worth reporting");
        CHECK(missing.Size() == 0, "missing file gives an empty cache");
    }
} // namespace

int main()
{
    TestFreshness();
    TestFingerprint();
    TestBounds();
    TestPersistence();
    std::printf("recommend cache: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
