// Unit tests for the platform-independent core layer.
// Build (Linux/WSL):  g++ -std=c++17 -I core/include core/src/*.cpp core/tests/test_core.cpp -o test_core
// Build (MSVC):       cl /std:c++17 /EHsc /I core\include core\src\*.cpp core\tests\test_core.cpp

#include "wm/core/Equalizer.h"
#include "wm/core/Fft.h"
#include "wm/core/Json.h"
#include "wm/core/LibraryStore.h"
#include "wm/core/LyricParser.h"
#include "wm/core/PlayQueue.h"
#include "wm/core/SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

namespace {

int gFailed = 0;
int gPassed = 0;

void Check(bool condition, const std::string& what, int line) {
    if (condition) {
        ++gPassed;
    } else {
        ++gFailed;
        std::cout << "  FAIL (line " << line << "): " << what << "\n";
    }
}

#define CHECK(cond) Check((cond), #cond, __LINE__)

constexpr double kPi = 3.14159265358979323846;

void TestFft() {
    std::cout << "[Fft]\n";

    constexpr std::size_t kN = 1024;
    wm::core::Fft fft(kN);

    // DC signal -> energy only in bin 0.
    {
        std::vector<std::complex<double>> data(kN, std::complex<double>(1.0, 0.0));
        fft.Transform(data);
        CHECK(std::abs(data[0].real() - static_cast<double>(kN)) < 1e-6);
        CHECK(std::abs(data[1]) < 1e-6);
        CHECK(std::abs(data[kN / 2]) < 1e-6);
    }

    // Pure sinusoid at bin k -> magnitude peak at bin k.
    {
        constexpr std::size_t k = 64;
        std::vector<double> real(kN);
        for (std::size_t i = 0; i < kN; ++i) {
            real[i] = std::sin(2.0 * kPi * static_cast<double>(k) * static_cast<double>(i) / kN);
        }
        const auto mag = wm::core::RealMagnitude(fft, real);
        std::size_t peak = 0;
        for (std::size_t i = 1; i < mag.size(); ++i) {
            if (mag[i] > mag[peak]) peak = i;
        }
        CHECK(peak == k);
        CHECK(mag[peak] > 0.4); // Hann-windowed full-scale sine => ~0.5
    }

    // Power of two guard.
    {
        bool threw = false;
        try {
            wm::core::Fft bad(1000);
            (void)bad;
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        CHECK(threw);
    }
}

void TestSpectrum() {
    std::cout << "[SpectrumAnalyzer]\n";

    wm::core::SpectrumConfig cfg;
    cfg.fftSize = 2048;
    cfg.barCount = 32;
    cfg.sampleRate = 48000;
    wm::core::SpectrumAnalyzer analyzer(cfg);

    // 8 kHz tone should light up the upper bars, not the bass bars.
    std::vector<float> frames(cfg.fftSize * 2); // stereo
    for (std::size_t i = 0; i < cfg.fftSize; ++i) {
        const double s = std::sin(2.0 * kPi * 8000.0 * static_cast<double>(i) / cfg.sampleRate);
        frames[i * 2] = static_cast<float>(s);
        frames[i * 2 + 1] = static_cast<float>(s);
    }

    std::vector<double> bars;
    for (int iter = 0; iter < 40; ++iter) {
        bars = analyzer.Process(frames.data(), cfg.fftSize, 2);
    }

    CHECK(bars.size() == cfg.barCount);
    double bass = 0.0;
    double treble = 0.0;
    for (std::size_t i = 0; i < 4; ++i) bass += bars[i];
    for (std::size_t i = bars.size() - 4; i < bars.size(); ++i) treble += bars[i];
    CHECK(treble > bass);

    // All bars stay within [0, 1].
    bool inRange = true;
    for (const double b : bars) {
        if (b < 0.0 || b > 1.0) inRange = false;
    }
    CHECK(inRange);

    // Silence decays towards zero.
    for (int iter = 0; iter < 200; ++iter) {
        analyzer.Silence();
    }
    bars = analyzer.Process(frames.data(), 0, 2);
    double sum = 0.0;
    for (const double b : bars) sum += b;
    CHECK(sum < 0.05);
}

void TestLyrics() {
    std::cout << "[LyricParser]\n";

    const std::string lrc =
        "[ti:晴天]\n"
        "[ar:周杰伦]\n"
        "[al:叶惠美]\n"
        "[by:w-music]\n"
        "[offset:-200]\n"
        "[00:00.00]故事的小黄花\n"
        "[00:05.50][00:31.00]从出生那年就飘着\n"
        "[00:12.25]<00:12.25>童年的<00:12.60>荡秋千</00:13.00>\n"
        "[01:02.5]刮风这天我试过握着你手\n";

    const auto doc = wm::core::LyricParser::Parse(lrc);
    CHECK(doc.valid);
    CHECK(doc.meta.title == "晴天");
    CHECK(doc.meta.artist == "周杰伦");
    CHECK(doc.offsetMs == -200);
    // 4 timed lines, one of which carries two time tags -> 5 entries.
    CHECK(doc.lines.size() == 5);

    CHECK(doc.lines[0].timeMs == 0);
    CHECK(doc.lines[0].text == "故事的小黄花");
    CHECK(doc.lines[1].timeMs == 5500);
    CHECK(doc.lines[2].timeMs == 12250);
    // Enhanced word tags stripped, text preserved.
    CHECK(doc.lines[2].text == "童年的荡秋千");
    CHECK(doc.lines[3].timeMs == 31000);
    // [01:02.5] -> 62.5s
    CHECK(doc.lines[4].timeMs == 62500);

    // Offset is applied when resolving the active line.
    CHECK(doc.IndexAt(0) == 0);       // 0 + (-200) <= 0
    CHECK(doc.IndexAt(5299) == 0);    // before line 1 (5500 - 200 = 5300)
    CHECK(doc.IndexAt(5300) == 1);
    CHECK(doc.IndexAt(60000) == 3);
    CHECK(doc.IndexAt(62300) == 4);

    // Round trip.
    const std::string again = wm::core::LyricParser::Serialize(doc);
    const auto doc2 = wm::core::LyricParser::Parse(again);
    CHECK(doc2.valid);
    CHECK(doc2.lines.size() == doc.lines.size());
    CHECK(doc2.offsetMs == doc.offsetMs);
    CHECK(doc2.lines[4].text == doc.lines[4].text);

    // Garbage input must not crash and must report invalid.
    const auto empty = wm::core::LyricParser::Parse("just some text\nno tags here\n");
    CHECK(!empty.valid);

    // UTF-16LE input with BOM.
    const std::string utf16 = [] {
        std::string raw;
        raw.push_back('\xFF');
        raw.push_back('\xFE');
        const char16_t text[] = u"[00:01.00]\x6C49\x5B57\n"; // "汉字"
        for (const char16_t u : text) {
            if (u == 0) break;
            raw.push_back(static_cast<char>(u & 0xFF));
            raw.push_back(static_cast<char>((u >> 8) & 0xFF));
        }
        return raw;
    }();
    const std::string utf8 = wm::core::LyricParser::DecodeToUtf8(utf16);
    const auto doc3 = wm::core::LyricParser::Parse(utf8);
    CHECK(doc3.valid);
    CHECK(doc3.lines.size() == 1);
    CHECK(doc3.lines[0].text == "\xE6\xB1\x89\xE5\xAD\x97"); // 汉字 in UTF-8
}

void TestJson() {
    std::cout << "[Json]\n";

    const std::string text = R"({
        "name": "w-music",
        "version": 1,
        "enabled": true,
        "ratio": 0.5,
        "items": [1, 2, 3],
        "nested": { "a": "b", "unicode": "汉字 \u00e9" },
        "empty": null
    })";

    std::string error;
    const auto root = wm::core::json::Parse(text, &error);
    CHECK(root.has_value());
    if (!root) {
        std::cout << "  parse error: " << error << "\n";
        return;
    }

    CHECK(root->Find("name")->asString() == "w-music");
    CHECK(root->Find("version")->asInt() == 1);
    CHECK(root->Find("enabled")->asBool() == true);
    CHECK(std::fabs(root->Find("ratio")->asNumber() - 0.5) < 1e-9);
    CHECK(root->Find("items")->asArray().size() == 3);
    CHECK(root->Find("items")->asArray()[2].asInt() == 3);
    CHECK(root->Find("nested")->Find("a")->asString() == "b");
    CHECK(root->Find("nested")->Find("unicode")->asString() == "\xE6\xB1\x89\xE5\xAD\x97 \xC3\xA9");
    CHECK(root->Find("empty")->isNull());
    CHECK(root->Find("missing") == nullptr);

    // Round trip.
    const std::string serialized = wm::core::json::Serialize(*root, false);
    const auto again = wm::core::json::Parse(serialized, &error);
    CHECK(again.has_value());
    if (again) {
        CHECK(again->Find("name")->asString() == "w-music");
        CHECK(again->Find("items")->asArray().size() == 3);
        CHECK(again->Find("nested")->Find("unicode")->asString() == "\xE6\xB1\x89\xE5\xAD\x97 \xC3\xA9");
    }

    // Malformed input must fail cleanly.
    CHECK(!wm::core::json::Parse("{\"a\": }", &error).has_value());
    CHECK(!wm::core::json::Parse("[1, 2", &error).has_value());
    CHECK(!wm::core::json::Parse("{\"a\":1} trailing", &error).has_value());

    // Builder API.
    wm::core::json::Value built;
    built["hello"] = wm::core::json::Value("world");
    wm::core::json::Array arr;
    arr.push_back(wm::core::json::Value(1));
    arr.push_back(wm::core::json::Value(2));
    built["list"] = wm::core::json::Value(std::move(arr));
    CHECK(wm::core::json::Serialize(built, false) == "{\"hello\":\"world\",\"list\":[1,2]}");
}

void TestLibrary() {
    std::cout << "[LibraryStore]\n";

    const std::string path = std::string(std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp") + "/wm_library_test.json";
    std::remove(path.c_str());

    {
        wm::core::LibraryStore store;
        wm::core::TrackRecord t;
        t.id = wm::core::LibraryStore::MakeTrackId("C:\\Music\\a.mp3");
        t.title = "晴天";
        t.artist = "周杰伦";
        t.album = "叶惠美";
        t.filePath = "C:\\Music\\a.mp3";
        t.durationMs = 269000;
        t.dateAdded = 1700000000000;
        store.UpsertTrack(t);

        // Stable id regardless of case.
        CHECK(wm::core::LibraryStore::MakeTrackId("C:\\Music\\a.mp3") == t.id);

        store.SetFavorite(t.id, true);
        CHECK(store.FindTrack(t.id)->favorite);
        CHECK(store.FindPlaylist("builtin:1") != nullptr);
        CHECK(store.FindPlaylist("builtin:1")->trackIds.size() == 1);

        store.MarkPlayed(t.id, 1700000001000);
        store.MarkPlayed(t.id, 1700000002000);
        CHECK(store.FindTrack(t.id)->playCount == 2);
        CHECK(store.FindPlaylist("builtin:2")->trackIds.size() == 1);

        auto& pl = store.CreatePlaylist("开车听的歌");
        store.AddToPlaylist(pl.id, t.id);
        CHECK(store.FindPlaylist(pl.id)->trackIds.size() == 1);

        // A rescan must not wipe play counts or favorites.
        wm::core::TrackRecord rescanned = t;
        rescanned.playCount = 0;
        rescanned.favorite = false;
        rescanned.durationMs = 270000;
        store.UpsertTrack(rescanned);
        CHECK(store.FindTrack(t.id)->playCount == 2);
        CHECK(store.FindTrack(t.id)->favorite);
        CHECK(store.FindTrack(t.id)->durationMs == 270000);

        std::string error;
        CHECK(store.Save(path, &error));
    }

    {
        wm::core::LibraryStore store;
        std::string error;
        CHECK(store.Load(path, &error));
        CHECK(store.Data().tracks.size() == 1);
        CHECK(store.Data().tracks[0].title == "晴天");
        CHECK(store.Data().tracks[0].playCount == 2);
        CHECK(store.Data().playlists.size() == 3); // favorites + recent + user
        CHECK(store.TopPlayed(10).size() == 1);
        CHECK(store.RecentlyAdded(10).size() == 1);

        // Built-in playlists cannot be deleted.
        CHECK(!store.DeletePlaylist("builtin:1"));

        std::string userPlaylistId;
        for (const auto& p : store.Data().playlists) {
            if (p.kind == wm::core::PlaylistKind::User) userPlaylistId = p.id;
        }
        CHECK(!userPlaylistId.empty());
        CHECK(store.DeletePlaylist(userPlaylistId));
    }

    std::remove(path.c_str());
}

void TestPlayQueue() {
    std::cout << "[PlayQueue]\n";

    wm::core::PlayQueue queue;

    // Sequential stops at the end.
    queue.SetMode(wm::core::PlayMode::Sequential);
    queue.SetTracks({"a", "b", "c"}, 0);
    CHECK(queue.Current() == std::optional<std::string>("a"));
    CHECK(queue.Next(true) == std::optional<std::string>("b"));
    CHECK(queue.Next(true) == std::optional<std::string>("c"));
    CHECK(queue.Next(true) == std::nullopt);

    // LoopAll wraps around.
    queue.SetMode(wm::core::PlayMode::LoopAll);
    queue.SetTracks({"a", "b", "c"}, 2);
    CHECK(queue.Next(true) == std::optional<std::string>("a"));
    CHECK(queue.Previous() == std::optional<std::string>("c"));

    // RepeatOne: auto-advance repeats, explicit next moves on.
    queue.SetMode(wm::core::PlayMode::RepeatOne);
    queue.SetTracks({"a", "b", "c"}, 1);
    CHECK(queue.Next(true) == std::optional<std::string>("b"));
    CHECK(queue.Next(false) == std::optional<std::string>("c"));

    // Shuffle visits every track exactly once per cycle.
    queue.SetMode(wm::core::PlayMode::Shuffle);
    queue.SetTracks({"a", "b", "c", "d", "e"}, 0);
    std::vector<std::string> seen;
    for (int i = 0; i < 4; ++i) {
        const auto next = queue.Next(true);
        CHECK(next.has_value());
        if (next) seen.push_back(*next);
    }
    CHECK(seen.size() == 4);
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    CHECK(seen.size() == 4);

    // Jump by id keeps the queue consistent.
    CHECK(queue.JumpToId("d") == std::optional<std::string>("d"));
    CHECK(queue.Current() == std::optional<std::string>("d"));

    // Empty queue is safe.
    queue.Clear();
    CHECK(queue.Next(true) == std::nullopt);
    CHECK(queue.Previous() == std::nullopt);
    CHECK(queue.Current() == std::nullopt);
}

void TestEqualizer() {
    std::cout << "[Equalizer]\n";

    constexpr double kRate = 48000.0;
    constexpr std::size_t kFrames = 32768;

    // Amplitude of the tail of a processed sine (transient filtered out by the
    // skip). 0.5 head-room keeps Process()'s output clamp out of the maths.
    auto probe = [&](double freqHz, const std::array<double, wm::core::Equalizer::BandCount>& gains,
                     double preampDb) {
        wm::core::Equalizer eq;
        eq.SetSampleRate(kRate);
        eq.SetGains(gains, preampDb);
        std::vector<float> buf(kFrames);
        for (std::size_t i = 0; i < kFrames; ++i) {
            buf[i] = static_cast<float>(0.5 * std::sin(2.0 * kPi * freqHz * static_cast<double>(i) / kRate));
        }
        // Mono: the filter still expects an interleaved buffer, one channel is fine.
        eq.Process(buf.data(), kFrames, 1);
        double sum = 0.0;
        constexpr std::size_t kTail = kFrames / 4;
        for (std::size_t i = kFrames - kTail; i < kFrames; ++i) {
            sum += static_cast<double>(buf[i]) * buf[i];
        }
        return std::sqrt(sum / static_cast<double>(kTail)) / (std::sqrt(0.5)); // normalise to amplitude
    };

    const auto zeroGains = [] {
        std::array<double, wm::core::Equalizer::BandCount> g{};
        return g;
    }();

    // Flat config passes a sine through untouched.
    {
        const double amp = probe(1000.0, zeroGains, 0.0);
        CHECK(std::abs(amp - 0.5) < 0.01);
    }

    // -12 dB at the probed centre frequency: amplitude ~ 10^(-12/20).
    {
        auto gains = zeroGains;
        gains[5] = -12.0; // 1 kHz band
        const double amp = probe(1000.0, gains, 0.0);
        CHECK(std::abs(amp - 0.5 * std::pow(10.0, -12.0 / 20.0)) < 0.02);
    }

    // +6 dB one band up leaves the probed frequency nearly alone.
    {
        auto gains = zeroGains;
        gains[7] = 6.0; // 4 kHz band, probing 1 kHz
        const double amp = probe(1000.0, gains, 0.0);
        CHECK(std::abs(amp - 0.5) < 0.03);
    }

    // Preamp scales linearly.
    {
        const double amp = probe(500.0, zeroGains, 6.0206); // ~2x
        CHECK(std::abs(amp - 1.0) < 0.05);
    }

    // Gains are clamped to the +/-12 dB range.
    {
        wm::core::Equalizer eq;
        eq.SetSampleRate(kRate);
        auto gains = zeroGains;
        gains[0] = 99.0;
        gains[9] = -99.0;
        eq.SetGains(gains);
        const auto read = eq.GainsDb();
        CHECK(read[0] == wm::core::Equalizer::MaxGainDb);
        CHECK(read[9] == wm::core::Equalizer::MinGainDb);
    }

    // Stereo processing keeps channels independent and finite.
    {
        wm::core::Equalizer eq;
        eq.SetSampleRate(kRate);
        auto gains = zeroGains;
        gains[3] = 6.0;
        eq.SetGains(gains);
        std::vector<float> stereo(kFrames * 2);
        for (std::size_t i = 0; i < kFrames; ++i) {
            stereo[i * 2] = static_cast<float>(std::sin(2.0 * kPi * 250.0 * i / kRate));
            stereo[i * 2 + 1] = static_cast<float>(std::sin(2.0 * kPi * 250.0 * i / kRate));
        }
        eq.Process(stereo.data(), kFrames, 2);
        bool finite = true;
        for (float v : stereo) {
            if (!std::isfinite(v)) finite = false;
        }
        CHECK(finite);
        double diff = 0.0;
        for (std::size_t i = kFrames - 1000; i < kFrames; ++i) {
            diff += std::abs(stereo[i * 2] - stereo[i * 2 + 1]);
        }
        CHECK(diff < 1e-3); // identical channels stay identical
    }

    // IsActive tracks the configuration so callers can bypass cleanly.
    {
        wm::core::Equalizer eq;
        eq.SetSampleRate(kRate);
        auto gains = zeroGains;
        gains[5] = 12.0;
        eq.SetGains(gains);
        CHECK(eq.IsActive());
        eq.SetGains(zeroGains);
        CHECK(!eq.IsActive());
    }
}

} // namespace

int main() {
    TestFft();
    TestSpectrum();
    TestEqualizer();
    TestLyrics();
    TestJson();
    TestLibrary();
    TestPlayQueue();

    std::cout << "\n" << gPassed << " passed, " << gFailed << " failed\n";
    return gFailed == 0 ? 0 : 1;
}
