#include "pch.h"

#include "Services/RecommendService.h"

#include "Services/AppPaths.h"
#include "Services/DiscoverSettings.h"

#include <shlobj.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Windows::Web::Http;

namespace wm::app
{
    namespace
    {
        /// Startup budget: uvicorn + librosa imports take a handful of seconds;
        /// 45 s leaves headroom for slower disks.
        constexpr int StartPolls = 90;
        constexpr std::chrono::milliseconds PollInterval{ 500 };

        /// The engine's README recommends --workers 6 for real libraries.
        constexpr int ScanWorkers = 6;

        hstring Wide(std::string const& utf8)
        {
            return hstring{ Utf16(utf8) };
        }

        /// "C:\a\b\歌名.wav" -> "歌名" (used when a file has no tags and the
        /// engine correctly refuses to invent a title).
        hstring FileNameStem(std::string const& utf8Path)
        {
            auto const slash = utf8Path.find_last_of("/\\");
            std::string name = slash == std::string::npos ? utf8Path : utf8Path.substr(slash + 1);
            auto const dot = name.find_last_of('.');
            if (dot != std::string::npos)
            {
                name.resize(dot);
            }
            return Wide(name);
        }

        int IntOf(wm::core::json::Value const* value)
        {
            return value != nullptr ? static_cast<int>(value->asInt()) : 0;
        }

        /// Chinese for the hard-filter names the engine can only estimate.
        /// Unknown keys pass through verbatim -- better a raw identifier than
        /// a silent guess about a condition we do not know.
        std::wstring FilterLabel(std::string const& key)
        {
            if (key == "instrumental") return L"纯音乐 / 无人声";
            if (key == "has_vocal") return L"有人声";
            if (key == "language_is") return L"语种";
            if (key == "genre_is") return L"曲风";
            return Utf16(key);
        }

        std::wstring UnmatchedReason(std::string const& reason)
        {
            if (reason == "unrecognized-descriptor") return L"没有任何本地音频测量能支撑这个词";
            if (reason == "no-language-tag-in-library") return L"曲库里没有语种标签可比对";
            if (reason == "no-genre-tag-in-library") return L"曲库里没有曲风标签可比对";
            return Utf16(reason);
        }

        /// Joined string array, or "" when absent / empty.
        std::wstring JoinStrings(wm::core::json::Value const* array)
        {
            std::wstring text;
            if (array == nullptr || !array->isArray())
            {
                return text;
            }
            for (auto const& entry : array->asArray())
            {
                if (!text.empty())
                {
                    text += L"、";
                }
                text += Utf16(entry.asString());
            }
            return text;
        }
    } // namespace

    RecommendService::RecommendService()
    {
        m_http = HttpClient{};
        // No explicit timeout: Windows.Web.Http has none and requests live as
        // long as the connection does, which is exactly what the minutes-long
        // /v1/library/scan needs. A dead engine surfaces as a faulted request.
    }

    RecommendService::~RecommendService()
    {
        Shutdown();
    }

    std::wstring RecommendService::DefaultRepoDir()
    {
        // Resolved at runtime so the default is not tied to one user name.
        wchar_t* known = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &known)) && known)
        {
            std::wstring dir{ known };
            CoTaskMemFree(known);
            return dir + L"\\music-recommend";
        }
        return {};
    }

    std::wstring RecommendService::RepoDir() const
    {
        // 1) explicit env override  2) settings.json  3) default location
        wchar_t env[MAX_PATH]{};
        const DWORD len = GetEnvironmentVariableW(L"WMUSIC_RECOMMEND_DIR", env, MAX_PATH);
        if (len > 0 && len < MAX_PATH)
        {
            return env;
        }
        std::wstring configured = Settings().RecommendServerDir();
        if (!configured.empty())
        {
            return configured;
        }
        return DefaultRepoDir();
    }

    unsigned short RecommendService::Port() const
    {
        return Settings().RecommendServerPort();
    }

    std::wstring RecommendService::BaseUri() const
    {
        return L"http://127.0.0.1:" + std::to_wstring(Port());
    }

    // -----------------------------------------------------------------------
    // engine lifecycle
    // -----------------------------------------------------------------------

    IAsyncOperation<bool> RecommendService::HealthCheckAsync()
    {
        co_await resume_background();
        try
        {
            HttpRequestMessage message{ HttpMethod{ hstring{ L"GET" } },
                                        Uri{ hstring{ BaseUri() + L"/v1/health" } } };
            auto response = co_await m_http.SendRequestAsync(message);
            if (!response.IsSuccessStatusCode())
            {
                co_return false;
            }
            auto buffer = co_await response.Content().ReadAsBufferAsync();
            std::string body(reinterpret_cast<char const*>(buffer.data()),
                             static_cast<std::size_t>(buffer.Length()));
            auto parsed = wm::core::json::Parse(body);
            auto const* status = parsed ? parsed->Find("status") : nullptr;
            co_return status != nullptr && status->asString() == "ok";
        }
        catch (...)
        {
            co_return false;
        }
    }

    hstring RecommendService::SpawnEngine()
    {
        // Non-throwing filesystem probes only: this runs inside a fire-and-forget
        // coroutine chain, and a std::filesystem_error escaping it would unwind
        // into an abandoned IAsyncAction -- std::terminate -- taking the whole
        // UI down with the recommend feature. Every probe below reports through
        // the returned error string instead.
        const std::wstring repo = RepoDir();
        std::error_code ec;
        if (repo.empty() || !std::filesystem::exists(repo, ec) || ec)
        {
            return hstring{ L"未找到 music-recommend 引擎目录（github.com/PT123123/music-recommend），"
                            L"可用环境变量 WMUSIC_RECOMMEND_DIR 指定" };
        }

        std::filesystem::path const venvPython = std::filesystem::path{ repo } / L".venv\\Scripts\\python.exe";
        std::wstring command;
        if (std::filesystem::exists(venvPython, ec) && !ec)
        {
            command = L"\"" + venvPython.wstring() + L"\"";
        }
        else
        {
            // Fall back to whatever python is on PATH (engine needs 3.10-3.12).
            command = L"python";
        }
        command += L" -u \"" + repo + L"\\scripts\\run_server.py\" --host 127.0.0.1 --port " +
                   std::to_wstring(Port());

        // Kill-on-close job object: whenever w-music exits (even crashing) the
        // engine child dies with it.
        if (m_job == nullptr)
        {
            m_job = CreateJobObjectW(nullptr, nullptr);
            if (m_job != nullptr)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                SetInformationJobObject(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            }
        }

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        std::wstring cmdline{ command };
        // CREATE_SUSPENDED so the process lands in the job before it can spawn
        // anything of its own.
        const BOOL ok = CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE,
                                       CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
                                       repo.c_str(), &si, &pi);
        if (!ok)
        {
            return hstring{ L"无法启动推荐引擎进程（Win32 错误 " + std::to_wstring(GetLastError()) + L"）" };
        }
        if (m_job != nullptr)
        {
            AssignProcessToJobObject(m_job, pi.hProcess);
        }
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        if (m_process != nullptr)
        {
            CloseHandle(m_process);
        }
        m_process = pi.hProcess;
        m_spawnedHere = true;
        return {};
    }

    IAsyncOperation<hstring> RecommendService::EnsureStartedAsync()
    {
        if (m_ready)
        {
            co_return hstring{};
        }
        co_await resume_background();

        // Reuse an engine that is already up (previous app run, manual start).
        // Outside the spawn lock: a concurrent caller polls the same port and
        // both conclude the same thing.
        if (co_await HealthCheckAsync())
        {
            m_ready = true;
            m_lastError = {};
            co_return hstring{};
        }

        // The mutex guards only the synchronous spawn; no co_await while held.
        static std::mutex spawnMutex;
        bool spawned = false;
        {
            std::lock_guard lock{ spawnMutex };
            if (m_ready)
            {
                co_return hstring{};
            }
            if (m_process == nullptr || !m_spawnedHere)
            {
                m_ready = false;
                try
                {
                    m_lastError = SpawnEngine();
                }
                catch (...)
                {
                    // Same contract as above: a recommend-engine failure may
                    // only ever become a status line, never an unhandled fault.
                    m_lastError = hstring{ L"推荐引擎启动失败（进程创建异常）" };
                }
                if (!m_lastError.empty())
                {
                    co_return m_lastError;
                }
                spawned = true;
            }
            else
            {
                // Another caller of this instance already spawned the engine;
                // verify it is still alive, then fall through and poll too.
                DWORD exitCode = 0;
                if (GetExitCodeProcess(m_process, &exitCode) && exitCode != STILL_ACTIVE)
                {
                    m_lastError = hstring{ L"推荐引擎启动失败（进程已退出，详见引擎 logs\\errors.log；"
                                           L"也可能是端口 " + std::to_wstring(Port()) + L" 被占用）" };
                    co_return m_lastError;
                }
            }
        }

        for (int i = 0; i < StartPolls; ++i)
        {
            co_await resume_after(PollInterval);

            if (spawned && m_process != nullptr)
            {
                DWORD exitCode = 0;
                if (GetExitCodeProcess(m_process, &exitCode) && exitCode != STILL_ACTIVE)
                {
                    m_lastError = hstring{ L"推荐引擎启动失败（进程已退出，详见引擎 logs\\errors.log；"
                                           L"也可能是端口 " + std::to_wstring(Port()) + L" 被占用）" };
                    co_return m_lastError;
                }
            }
            if (co_await HealthCheckAsync())
            {
                m_ready = true;
                m_lastError = {};
                co_return hstring{};
            }
        }

        m_lastError = hstring{ L"推荐引擎启动超时（45 秒内未通过健康检查）" };
        co_return m_lastError;
    }

    void RecommendService::Shutdown()
    {
        m_ready = false;
        if (m_process != nullptr)
        {
            if (m_spawnedHere)
            {
                TerminateProcess(m_process, 0);
            }
            CloseHandle(m_process);
            m_process = nullptr;
        }
        if (m_job != nullptr)
        {
            CloseHandle(m_job);  // kill-on-close sweeps up any stragglers
            m_job = nullptr;
        }
    }

    // -----------------------------------------------------------------------
    // HTTP plumbing
    // -----------------------------------------------------------------------

    IAsyncOperation<hstring> RecommendService::RequestJsonAsync(hstring method, std::wstring path, std::string body)
    {
        co_await resume_background();
        try
        {
            HttpRequestMessage message{ HttpMethod{ method }, Uri{ hstring{ BaseUri() + path } } };
            if (!body.empty())
            {
                message.Content(HttpStringContent{ Wide(body) });
                message.Content().Headers().ContentType().MediaType(L"application/json");
            }

            auto response = co_await m_http.SendRequestAsync(message);
            auto buffer = co_await response.Content().ReadAsBufferAsync();
            std::string text(reinterpret_cast<char const*>(buffer.data()),
                             static_cast<std::size_t>(buffer.Length()));

            if (!response.IsSuccessStatusCode())
            {
                // FastAPI errors carry a "detail" field; show it when present.
                auto parsed = wm::core::json::Parse(text);
                auto const* detail = parsed ? parsed->Find("detail") : nullptr;
                std::wstring const reason = detail != nullptr && detail->isString()
                    ? std::wstring{ Utf16(detail->asString()) }
                    : std::to_wstring(static_cast<int>(response.StatusCode()));
                m_lastError = hstring{ L"推荐引擎请求失败：" + reason };
                co_return hstring{};
            }

            m_lastError = {};
            co_return hstring{ Utf16(text) };
        }
        catch (hresult_error const& e)
        {
            m_lastError = hstring{ L"推荐引擎请求异常：" + std::wstring{ e.message() } };
        }
        catch (...)
        {
            m_lastError = hstring{ L"推荐引擎请求异常（引擎可能未启动）" };
        }
        co_return hstring{};
    }

    IAsyncOperation<hstring> RecommendService::CallAfterStartAsync(hstring method, std::wstring path, std::string body)
    {
        hstring const startError = co_await EnsureStartedAsync();
        if (!startError.empty())
        {
            m_lastError = startError;
            co_return hstring{};
        }
        try
        {
            co_return co_await RequestJsonAsync(std::move(method), std::move(path), std::move(body));
        }
        catch (...)
        {
            // RequestJsonAsync already maps WinRT faults to m_lastError; this
            // catch is the outer wall that keeps ANY unexpected exception in
            // the recommend chain from escaping into a fire-and-forget caller
            // (an abandoned faulted coroutine fail-fasts the whole app).
            m_lastError = hstring{ L"推荐引擎请求异常（引擎可能未启动）" };
            co_return hstring{};
        }
    }

    // -----------------------------------------------------------------------
    // /v1 endpoints
    // -----------------------------------------------------------------------

    w_music::RecommendItem RecommendService::ParseRecommendRow(wm::core::json::Value const& row)
    {
        auto item = winrt::make<winrt::w_music::implementation::RecommendItem>();

        if (auto const* v = row.Find("track_id"); v != nullptr)
        {
            item.TrackId(Wide(v->asString()));
        }
        if (auto const* v = row.Find("score"); v != nullptr && v->isNumber())
        {
            double const score = std::clamp(v->asNumber(), 0.0, 1.0);
            item.Score(score);
            wchar_t buffer[8];
            swprintf(buffer, 8, L"%d%%", static_cast<int>(std::lround(score * 100.0)));
            item.ScoreText(hstring{ buffer });
        }
        else
        {
            // Cold start: no interest vector yet, so the engine returns no
            // score -- its picks are diversity-driven ("cold_start_diverse").
            item.ScoreText(hstring{ L"探索" });
            item.ScoreLabel(hstring{ L"冷启动" });
        }

        std::wstring reasons;
        if (auto const* list = row.Find("reasons"); list != nullptr && list->isArray())
        {
            for (auto const& reason : list->asArray())
            {
                if (!reasons.empty())
                {
                    reasons += L"、";
                }
                reasons += Utf16(reason.asString());
            }
        }
        else if (auto const* single = row.Find("reason"); single != nullptr && single->isString())
        {
            // Feed rows carry a singular reason marker instead of a list.
            std::string const marker = single->asString();
            if (marker == "cold_start_diverse")
            {
                reasons = L"冷启动期：按多样性探索全库";
            }
            else if (!marker.empty())
            {
                reasons = Utf16(marker);
            }
        }
        item.Reasons(hstring{ reasons });

        if (auto const* display = row.Find("display"); display != nullptr && display->isObject())
        {
            auto const* title = display->Find("title");
            auto const* artist = display->Find("artist");
            auto const* album = display->Find("album");
            auto const* filePath = display->Find("file_path");
            if (title != nullptr && title->isString() && !title->asString().empty())
            {
                item.Title(Wide(title->asString()));
            }
            else if (filePath != nullptr && filePath->isString() && !filePath->asString().empty())
            {
                item.Title(FileNameStem(filePath->asString()));
            }
            else if (auto const* fileName = row.Find("file_name");
                     fileName != nullptr && fileName->isString() && !fileName->asString().empty())
            {
                // The engine always knows the file name even when the row was
                // not found in the library database.
                item.Title(FileNameStem(fileName->asString()));
            }
            if (artist != nullptr && artist->isString())
            {
                item.Artist(Wide(artist->asString()));
            }
            if (album != nullptr && album->isString())
            {
                item.Album(Wide(album->asString()));
            }
            if (filePath != nullptr && filePath->isString())
            {
                item.FilePath(Wide(filePath->asString()));
            }

            // genre / language are the file's own claim (ADR-18): meta_source
            // says so, and without it they stay hidden rather than reading as
            // "analysed from the audio".
            auto const* metaSource = display->Find("meta_source");
            if (metaSource != nullptr && metaSource->isString() && metaSource->asString() != "none")
            {
                std::wstring tags;
                for (char const* key : { "genre", "language" })
                {
                    if (auto const* tag = display->Find(key);
                        tag != nullptr && tag->isString() && !tag->asString().empty())
                    {
                        if (!tags.empty())
                        {
                            tags += L" · ";
                        }
                        tags += Utf16(tag->asString());
                    }
                }
                if (!tags.empty())
                {
                    item.TagText(hstring{ L"文件标签：" + tags });
                }
            }
        }
        return item;
    }

    IAsyncOperation<IVectorView<w_music::CategoryItem>> RecommendService::GetCategoriesAsync()
    {
        auto text = co_await CallAfterStartAsync(hstring{ L"GET" }, L"/v1/categories", {});
        auto result = winrt::single_threaded_vector<w_music::CategoryItem>();
        auto parsed = text.empty() ? std::nullopt : wm::core::json::Parse(Utf8(text));
        if (parsed && parsed->isArray())
        {
            for (auto const& entry : parsed->asArray())
            {
                auto chip = winrt::make<winrt::w_music::implementation::CategoryItem>();
                if (auto const* v = entry.Find("id"); v != nullptr)
                {
                    chip.Id(Wide(v->asString()));
                }
                if (auto const* v = entry.Find("name"); v != nullptr)
                {
                    chip.Label(Wide(v->asString()));
                }
                if (auto const* v = entry.Find("source"); v != nullptr && v->isString())
                {
                    chip.Source(Wide(v->asString()));
                }
                if (auto const* v = entry.Find("note"); v != nullptr && v->isString())
                {
                    chip.Note(Wide(v->asString()));
                }
                if (auto const* v = entry.Find("support"); v != nullptr && v->isNumber())
                {
                    chip.Support(static_cast<int32_t>(v->asInt()));
                }
                if (chip.IsDiscovered() && chip.Support() > 0)
                {
                    // The name of an auto category is generated from its
                    // cluster; the member count is what makes it checkable.
                    chip.SupportText(hstring{ std::to_wstring(chip.Support()) + L" 首" });
                }
                if (!chip.Id().empty())
                {
                    result.Append(std::move(chip));
                }
            }
        }
        co_return result.GetView();
    }

    IAsyncOperation<hstring> RecommendService::DiscoveryTextAsync()
    {
        auto text = co_await CallAfterStartAsync(hstring{ L"GET" }, L"/v1/categories/discovery", {});
        if (text.empty())
        {
            co_return m_lastError;
        }
        auto parsed = wm::core::json::Parse(Utf8(text));
        auto const* meta = parsed ? parsed->Find("meta") : nullptr;
        if (meta == nullptr || !meta->isObject())
        {
            co_return hstring{ L"自动类别信息不可用" };
        }
        std::wstring const status = meta->Find("status") != nullptr
            ? std::wstring{ Utf16(meta->Find("status")->asString()) }
            : std::wstring{};
        int const library = IntOf(meta->Find("library_size"));
        int const k = IntOf(meta->Find("k"));
        auto const* silhouette = meta->Find("silhouette");

        if (status == L"ok" && silhouette != nullptr && silhouette->isNumber())
        {
            double const score = silhouette->asNumber();
            std::wstring note = L"在本库 " + std::to_wstring(library) + L" 首里聚出 " + std::to_wstring(k) +
                                L" 类，轮廓系数 " + [](double s)
            {
                wchar_t buffer[16];
                swprintf(buffer, 16, L"%.3f", s);
                return std::wstring{ buffer };
            }(score);
            // The engine itself calls anything under ~0.2 a fuzzy boundary; do
            // not let a k look like a confidence score.
            note += score < 0.25 ? L"（边界模糊，仅供参考）" : L"（簇间区分度尚可）";
            co_return hstring{ note };
        }
        if (status == L"library-too-small")
        {
            co_return hstring{ L"曲库只有 " + std::to_wstring(library) +
                               L" 首，不够做诚实的聚类，自动类别留空" };
        }
        if (status == L"no-sklearn")
        {
            co_return hstring{ L"引擎缺 sklearn，自动类别不可用（预设类别不受影响）" };
        }
        if (status == L"disabled")
        {
            co_return hstring{ L"引擎设置里关闭了自动发现" };
        }
        co_return hstring{ L"自动类别未产出结果（" + (status.empty() ? hstring{ L"未知状态" } : hstring{ status }) + L"）" };
    }

    IAsyncOperation<IVectorView<w_music::RecommendItem>>
    RecommendService::GetFeedAsync(int32_t limit, std::vector<hstring> excludeTrackIds)
    {
        wm::core::json::Value body = wm::core::json::Object{};
        body["user_id"] = "local-user";  // same user id the CLI scripts use
        body["limit"] = limit;
        if (!excludeTrackIds.empty())
        {
            wm::core::json::Value excludes = wm::core::json::Array{};
            for (auto const& id : excludeTrackIds)
            {
                excludes.Push(wm::core::json::Value{ Utf8(id) });
            }
            body["exclude_track_ids"] = std::move(excludes);
        }

        auto text = co_await CallAfterStartAsync(hstring{ L"POST" }, L"/v1/feed/next",
                                                 wm::core::json::Serialize(body, false));
        auto result = winrt::single_threaded_vector<w_music::RecommendItem>();
        auto parsed = text.empty() ? std::nullopt : wm::core::json::Parse(Utf8(text));
        if (parsed)
        {
            if (auto const* items = parsed->Find("items"); items != nullptr && items->isArray())
            {
                for (auto const& row : items->asArray())
                {
                    result.Append(ParseRecommendRow(row));
                }
            }
        }
        co_return result.GetView();
    }

    IAsyncOperation<IVectorView<w_music::RecommendItem>>
    RecommendService::GetSimilarByPathAsync(hstring filePath, int32_t limit)
    {
        wm::core::json::Value body = wm::core::json::Object{};
        body["file_path"] = Utf8(filePath);
        body["limit"] = limit;

        auto text = co_await CallAfterStartAsync(hstring{ L"POST" }, L"/v1/recommend/similar",
                                                 wm::core::json::Serialize(body, false));
        auto result = winrt::single_threaded_vector<w_music::RecommendItem>();
        auto parsed = text.empty() ? std::nullopt : wm::core::json::Parse(Utf8(text));
        if (parsed)
        {
            if (auto const* rows = parsed->Find("recommendations"); rows != nullptr && rows->isArray())
            {
                for (auto const& row : rows->asArray())
                {
                    result.Append(ParseRecommendRow(row));
                }
            }
        }
        co_return result.GetView();
    }

    std::wstring RecommendService::CategoryNoteOf(wm::core::json::Value const& answer)
    {
        std::vector<std::wstring> parts;

        // support is the one number that matters on a small library: it says
        // how many tracks really land near the target, not how many rows the
        // engine was willing to print.
        auto const* support = answer.Find("support");
        if (support != nullptr && support->isNumber())
        {
            std::wstring line = L"落点在库里覆盖 " + std::to_wstring(static_cast<int>(support->asInt()));
            if (auto const* pool = answer.Find("candidate_pool");
                pool != nullptr && pool->isNumber())
            {
                line += L"/" + std::to_wstring(static_cast<int>(pool->asInt())) + L" 首";
            }
            if (auto const* low = answer.Find("low_support"); low != nullptr && low->asBool())
            {
                line += L"，低于支持度门槛：这份列表是按目标凑出来的排名，不是「这类歌有这么多」";
            }
            parts.push_back(std::move(line));
        }

        if (auto const* ignored = answer.Find("ignored_dims");
            ignored != nullptr && ignored->isArray() && !ignored->asArray().empty())
        {
            parts.push_back(L"本地量不出、已忽略：" + JoinStrings(ignored));
        }
        if (auto const* unscored = answer.Find("unscored_dims");
            unscored != nullptr && unscored->isArray() && !unscored->asArray().empty())
        {
            parts.push_back(L"过滤后这些维度没有数据可排：" + JoinStrings(unscored));
        }
        if (auto const* usable = answer.Find("usable_dims");
            usable != nullptr && usable->isNumber() && usable->asInt() == 0)
        {
            parts.push_back(L"没有任何维度参与打分，结果只是过滤后的清单");
        }

        auto const* applied = answer.Find("filters_applied");
        if (applied != nullptr && applied->isArray() && !applied->asArray().empty())
        {
            std::wstring names;
            for (auto const& entry : applied->asArray())
            {
                if (!names.empty())
                {
                    names += L"、";
                }
                names += FilterLabel(entry.asString());
            }
            std::wstring line = L"生效的硬过滤：" + names;
            if (auto const* estimated = answer.Find("filters_estimated");
                estimated != nullptr && estimated->isArray() && !estimated->asArray().empty())
            {
                std::wstring est;
                for (auto const& entry : estimated->asArray())
                {
                    if (!est.empty())
                    {
                        est += L"、";
                    }
                    est += FilterLabel(entry.asString());
                }
                line += L"，其中 " + est + L" 来自阈值化的估计值（人声判定实测并不可靠）";
            }
            parts.push_back(std::move(line));
        }

        if (auto const* unknown = answer.Find("unknown_filters");
            unknown != nullptr && unknown->isArray() && !unknown->asArray().empty())
        {
            parts.push_back(L"未识别的过滤条件（只报告，未生效）：" + JoinStrings(unknown));
        }

        // genre / vocal proxies: the engine answered a tag question with a
        // listening approximation because the library carries no such tag.
        for (char const* key : { "genre_proxies", "vocal_proxies" })
        {
            auto const* proxies = answer.Find(key);
            if (proxies == nullptr || !proxies->isArray())
            {
                continue;
            }
            for (auto const& proxy : proxies->asArray())
            {
                auto const* termField = proxy.Find("term");
                auto const* guessField = proxy.Find("guess");
                std::wstring const term = termField != nullptr ? Utf16(termField->asString()) : std::wstring{};
                std::wstring const guess = guessField != nullptr && !guessField->asString().empty()
                    ? Utf16(guessField->asString())
                    : (key == std::string{ "genre_proxies" } ? std::wstring{ L"按听感近似" }
                                                             : std::wstring{ L"按人声音区近似" });
                parts.push_back(L"「" + term + L"」没有标签可依，按听感近似：" + guess + L"（不是识别结果）");
            }
        }

        if (auto const* missing = answer.Find("tag_missing_tracks"); missing != nullptr)
        {
            if (missing->isNumber())
            {
                if (int const count = static_cast<int>(missing->asInt()); count > 0)
                {
                    parts.push_back(L"库里 " + std::to_wstring(count) + L" 首没有可用于该条件的标签");
                }
            }
            else if (missing->isArray() && !missing->asArray().empty())
            {
                parts.push_back(L"缺标签：" + JoinStrings(missing));
            }
        }

        // Free-text entry: report what the words actually mapped to, and
        // refuse to hide the ones the engine declined to answer.
        if (auto const* matched = answer.Find("matched_terms");
            matched != nullptr && matched->isArray() && !matched->asArray().empty())
        {
            std::wstring line = L"理解到的词：";
            bool first = true;
            for (auto const& term : matched->asArray())
            {
                auto const* word = term.Find("word");
                if (word == nullptr || !word->isString())
                {
                    continue;
                }
                if (!first)
                {
                    line += L"、";
                }
                first = false;
                std::wstring const side = term.Find("side") != nullptr
                    ? Utf16(term.Find("side")->asString())
                    : std::wstring{};
                line += L"「" + Utf16(term.Find("term") != nullptr ? term.Find("term")->asString() : "") +
                        L"」→ " + Utf16(word->asString());
                if (side == L"low")
                {
                    line += L"（取低侧）";
                }
            }
            if (!first)
            {
                parts.push_back(std::move(line));
            }
        }
        if (auto const* unmatched = answer.Find("unmatched_terms");
            unmatched != nullptr && unmatched->isArray() && !unmatched->asArray().empty())
        {
            std::wstring line = L"答不了的词：";
            bool first = true;
            for (auto const& term : unmatched->asArray())
            {
                std::wstring const word = Utf16(term.Find("term") != nullptr ? term.Find("term")->asString() : "");
                std::wstring const reason = UnmatchedReason(
                    term.Find("reason") != nullptr ? term.Find("reason")->asString() : std::string{});
                if (!first)
                {
                    line += L"、";
                }
                first = false;
                line += L"「" + word + L"」" + reason;
            }
            parts.push_back(std::move(line));
        }

        std::wstring note;
        for (auto const& part : parts)
        {
            if (!note.empty())
            {
                note += L"；";
            }
            note += part;
        }
        return note;
    }

    IAsyncOperation<IVectorView<w_music::RecommendItem>>
    RecommendService::GetCategoryAsync(hstring categoryId, int32_t limit)
    {
        wm::core::json::Value body = wm::core::json::Object{};
        body["category_id"] = Utf8(categoryId);
        body["limit"] = limit;
        co_return co_await RequestCategoryAsync(wm::core::json::Serialize(body, false));
    }

    IAsyncOperation<IVectorView<w_music::RecommendItem>>
    RecommendService::SearchByTextAsync(hstring text, int32_t limit)
    {
        wm::core::json::Value body = wm::core::json::Object{};
        body["text"] = Utf8(text);
        body["limit"] = limit;
        co_return co_await RequestCategoryAsync(wm::core::json::Serialize(body, false));
    }

    IAsyncOperation<IVectorView<w_music::RecommendItem>>
    RecommendService::RequestCategoryAsync(std::string body)
    {
        auto rows = winrt::single_threaded_vector<w_music::RecommendItem>();
        m_categoryNote = {};

        auto text = co_await CallAfterStartAsync(hstring{ L"POST" }, L"/v1/recommend/category", std::move(body));
        auto parsed = text.empty() ? std::nullopt : wm::core::json::Parse(Utf8(text));
        if (parsed)
        {
            if (auto const* list = parsed->Find("recommendations"); list != nullptr && list->isArray())
            {
                for (auto const& row : list->asArray())
                {
                    rows.Append(ParseRecommendRow(row));
                }
            }
            m_categoryNote = hstring{ CategoryNoteOf(*parsed) };
        }
        co_return rows.GetView();
    }

    IAsyncAction RecommendService::SendFeedbackAsync(hstring trackId, hstring eventId)
    {
        if (!m_ready || trackId.empty())
        {
            co_return;
        }
        wm::core::json::Value body = wm::core::json::Object{};
        body["user_id"] = "local-user";
        body["track_id"] = Utf8(trackId);
        body["event"] = Utf8(eventId);
        co_await RequestJsonAsync(hstring{ L"POST" }, L"/v1/feed/feedback",
                                  wm::core::json::Serialize(body, false));
    }

    IAsyncAction RecommendService::ResetTasteAsync()
    {
        if (!m_ready)
        {
            co_return;
        }
        co_await RequestJsonAsync(hstring{ L"POST" }, L"/v1/feed/reset?scope=all", "{}");
    }

    IAsyncOperation<hstring> RecommendService::AnalyzeFoldersAsync(std::vector<std::wstring> folders)
    {
        if (folders.empty())
        {
            co_return hstring{ L"本地曲库为空：请先在「发现音乐」添加音乐文件夹" };
        }

        hstring const startError = co_await EnsureStartedAsync();
        if (!startError.empty())
        {
            co_return startError;
        }

        int totalFound = 0;
        int totalIndexed = 0;
        int totalSkipped = 0;
        int totalFailed = 0;
        std::wstring errors;
        for (auto const& folder : folders)
        {
            wm::core::json::Value body = wm::core::json::Object{};
            body["root"] = Utf8(hstring{ folder });
            body["recursive"] = true;
            body["workers"] = ScanWorkers;
            auto text = co_await RequestJsonAsync(hstring{ L"POST" }, L"/v1/library/scan",
                                                  wm::core::json::Serialize(body, false));
            auto parsed = text.empty() ? std::nullopt : wm::core::json::Parse(Utf8(text));
            if (!parsed)
            {
                if (!errors.empty())
                {
                    errors += L"；";
                }
                errors += std::wstring{ folder } + L"：" + std::wstring{ m_lastError };
                continue;
            }
            totalFound += IntOf(parsed->Find("found"));
            totalIndexed += IntOf(parsed->Find("indexed"));
            totalSkipped += IntOf(parsed->Find("skipped"));
            totalFailed += IntOf(parsed->Find("failed"));
        }

        if (totalFound + totalSkipped == 0 && !errors.empty())
        {
            co_return hstring{ errors };
        }

        // Only .mp3/.wav enter the engine, so "found" can legitimately trail
        // the w-music library size (flac etc. stay w-music-only).
        std::wstring summary = L"曲库分析完成：本次新增 " + std::to_wstring(totalIndexed) +
                               L" 首，跳过已分析 " + std::to_wstring(totalSkipped) +
                               L" 首，失败 " + std::to_wstring(totalFailed) + L" 首";
        if (!errors.empty())
        {
            summary += L"；" + errors;
        }
        co_return hstring{ summary };
    }

    IAsyncOperation<hstring> RecommendService::FeedStateTextAsync()
    {
        auto text = co_await CallAfterStartAsync(hstring{ L"GET" },
                                                 L"/v1/feed/state?user_id=local-user", {});
        if (text.empty())
        {
            co_return m_lastError;
        }
        auto parsed = wm::core::json::Parse(Utf8(text));
        if (!parsed)
        {
            co_return m_lastError;
        }
        auto const* hasHistory = parsed->Find("has_history");
        if (hasHistory == nullptr || !hasHistory->asBool())
        {
            co_return hstring{ L"还没有听歌行为：多听 / 点喜欢，推荐会越来越懂你" };
        }
        // Scope vectors are high-dimensional; summarize honestly without
        // pretending to know which "mood" they encode.
        int present = 0;
        for (char const* key : { "short_term", "medium_term", "long_term" })
        {
            if (auto const* scope = parsed->Find(key);
                scope != nullptr && scope->isObject() && !scope->asObject().empty())
            {
                ++present;
            }
        }
        co_return hstring{ L"口味画像已积累（" + std::to_wstring(present) + L"/3 个时间尺度）" };
    }
} // namespace wm::app
