#include "pch.h"

#include "ViewModels/RecommendViewModel.h"
#include "ViewModels/RecommendViewModel.g.cpp"

#include "Services/AppPaths.h"
#include "Services/DiscoverSettings.h"
#include "Services/LibraryService.h"
#include "Services/RecommendService.h"
#include "Services/Services.h"
#include "ViewModels/PlayerViewModel.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Microsoft::UI::Xaml;

namespace winrt::w_music::implementation
{
    namespace
    {
        /// The list feeds the player through ordinary TrackItems; the "rec:"
        /// prefix keeps engine rows out of the library id namespace (they can
        /// never collide with path-hash ids, and lyrics / play counts simply
        /// no-op for them).
        hstring QueueIdFor(winrt::w_music::RecommendItem const& row)
        {
            return hstring{ L"rec:" } + row.TrackId();
        }

        w_music::TrackItem TrackFromRecommend(winrt::w_music::RecommendItem const& row)
        {
            auto track = winrt::make<TrackItem>();
            track.Id(QueueIdFor(row));
            track.Title(row.Title());
            track.Artist(row.Artist());
            track.Album(row.Album());
            track.FilePath(row.FilePath());
            return track;
        }

        /// Rows per list. Also the cache key's limit component: a different
        /// limit is a different answer, and the page only ever asks for one.
        constexpr int32_t ListLimit = 30;

        using Answer = wm::app::RecommendService::Answer;

        /// One caveat line out of the engine's answer and the chip's caption.
        hstring JoinNotes(hstring const& first, hstring const& second)
        {
            if (first.empty())
            {
                return second;
            }
            return second.empty() ? first : first + hstring{ L"；" } + second;
        }

        /// The waiting hint is staged because the waits are not comparable: a
        /// cold engine takes seconds, a warm query tens of milliseconds, and an
        /// analysis runs minutes with everything else queued behind it.
        hstring WaitingTextFor(wm::app::RecommendService::Stage stage)
        {
            switch (stage)
            {
            case wm::app::RecommendService::Stage::Starting:
                return hstring{ L"正在启动本地推荐引擎（首次约 5～10 秒）…" };
            case wm::app::RecommendService::Stage::Analyzing:
                return hstring{ L"引擎正在分析曲库，这个请求排在它后面…" };
            default:
                return hstring{ L"正在向本地引擎查询…" };
            }
        }
    } // namespace

    RecommendViewModel::RecommendViewModel()
    {
        m_categories = winrt::single_threaded_observable_vector<w_music::CategoryItem>();
        m_items = winrt::single_threaded_observable_vector<w_music::RecommendItem>();
    }

    event_token RecommendViewModel::PropertyChanged(Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
    {
        return m_propertyChanged.add(handler);
    }

    void RecommendViewModel::RaisePropertyChanged(std::wstring_view const& name)
    {
        m_propertyChanged(*this, Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ hstring{ name } });
    }

    void RecommendViewModel::SetWaiting(bool busy)
    {
        m_isBusy = busy;
        // Re-raised even when the flag did not change: the stage can move on
        // while a request is still running, and that is worth a new sentence.
        m_busyText = busy ? WaitingTextFor(wm::app::Recommend().CurrentStage()) : hstring{};
        RaisePropertyChanged(L"IsBusy");
        RaisePropertyChanged(L"BusyText");
        RaisePropertyChanged(L"WaitingVisibility");
        // The empty state yields to the waiting bar.
        RaisePropertyChanged(L"EmptyVisibility");
    }

    void RecommendViewModel::SetStatus(hstring const& text)
    {
        m_statusText = text;
        RaisePropertyChanged(L"StatusText");
    }

    void RecommendViewModel::SetCategoryNote(hstring const& text)
    {
        m_categoryNote = text;
        RaisePropertyChanged(L"CategoryNote");
        RaisePropertyChanged(L"CategoryNoteVisibility");
    }

    // -----------------------------------------------------------------------
    // loading
    // -----------------------------------------------------------------------

    IAsyncAction RecommendViewModel::InitializeAsync()
    {
        // Every visit re-states the library size: tracks get added between
        // visits, and it is what namespaces the disk cache.
        wm::app::Recommend().SetLibrarySize(
            static_cast<int32_t>(wm::app::Library().Tracks().Size()));

        if (m_initialized)
        {
            co_return;
        }
        m_initialized = true;

        // Reaching this page at all is the signal that prewarming the engine at
        // startup is worth a background Python process next run.
        wm::app::Settings().RecommendPrewarm(true);

        // Everything the engine can answer from disk goes on screen first: the
        // live calls below may wait on a seconds-long cold start, and a blank
        // page cannot show a waiting hint.
        PaintCachedChips();
        LoadCategoriesAsync();
        co_await LoadFeedAsync(false);
        co_await RefreshFeedStateAsync();
    }

    void RecommendViewModel::PaintCachedChips()
    {
        auto const cached = wm::app::Recommend().CachedChips();
        if (cached == nullptr || cached.Size() == 0)
        {
            // Nothing on disk yet: the chip rows stay empty and the live call
            // fills them, rather than showing a lone "为你推荐".
            return;
        }

        m_categories.Clear();
        auto feedChip = winrt::make<CategoryItem>();
        feedChip.Id(hstring{});
        feedChip.Label(hstring{ L"为你推荐" });
        m_categories.Append(std::move(feedChip));
        for (auto const& chip : cached)
        {
            m_categories.Append(chip);
        }

        m_discoveryText = wm::app::Recommend().CachedDiscoveryText();
        RaisePropertyChanged(L"DiscoveryText");
        RaisePropertyChanged(L"Categories");
    }

    IAsyncAction RecommendViewModel::LoadCategoriesAsync()
    {
        auto categories = co_await wm::app::Recommend().GetCategoriesAsync();
        auto discovery = co_await wm::app::Recommend().DiscoveryTextAsync();
        co_await wm::app::ResumeOnUi();

        // Swapped in as a whole, after the await: the cached chips stay up for
        // the duration of the request instead of leaving the rows empty.
        m_categories.Clear();

        // First chip is the pseudo-category "为你推荐" (= back to the feed).
        auto feedChip = winrt::make<CategoryItem>();
        feedChip.Id(hstring{});
        feedChip.Label(hstring{ L"为你推荐" });
        m_categories.Append(std::move(feedChip));
        for (auto const& chip : categories)
        {
            m_categories.Append(chip);
        }

        // An empty answer means the engine could not be reached: keep the
        // caption the cache painted instead of blanking the section.
        auto const discoveryError = wm::app::Recommend().LastError();
        if (discoveryError.empty())
        {
            m_discoveryText = discovery;
        }
        RaisePropertyChanged(L"DiscoveryText");
        RaisePropertyChanged(L"Categories");
        RaisePropertyChanged(L"SelectedCategoryId");
    }

    uint32_t RecommendViewModel::BeginList(
        hstring header,
        IVectorView<w_music::RecommendItem> const& cached,
        hstring const& cachedNote,
        hstring const& cachedAge)
    {
        auto const token = ++m_listToken;

        m_items.Clear();
        for (auto const& row : cached)
        {
            m_items.Append(row);
        }
        m_listHeader = std::move(header);
        SetCategoryNote(cachedNote);
        // Only worth saying when rows really came from disk: an empty list with
        // this caption would point at nothing.
        m_cacheCaption = m_items.Size() > 0 && !cachedAge.empty()
            ? hstring{ std::wstring{ L"以下是 " } + std::wstring{ cachedAge } +
                       L"的结果，正在向引擎要最新的" }
            : hstring{};
        SetWaiting(true);

        RaisePropertyChanged(L"ListHeader");
        RaisePropertyChanged(L"HasItems");
        RaisePropertyChanged(L"ItemsVisibility");
        RaisePropertyChanged(L"EmptyVisibility");
        RaisePropertyChanged(L"CacheCaption");
        RaisePropertyChanged(L"CacheCaptionVisibility");
        return token;
    }

    IAsyncAction RecommendViewModel::FinishList(uint32_t token,
                                                IAsyncOperation<IVectorView<w_music::RecommendItem>> pending,
                                                bool categoryAnswer,
                                                hstring chipNote)
    {
        auto rows = co_await pending;
        co_await wm::app::ResumeOnUi();

        // The user clicked something newer while this answer was on its way:
        // that action owns the list, and painting a stale answer over it would
        // look like the chip does nothing.
        if (token != m_listToken)
        {
            co_return;
        }

        auto& engine = wm::app::Recommend();
        auto const error = engine.LastError();
        if (error.empty())
        {
            m_items.Clear();
            for (auto const& row : rows)
            {
                m_items.Append(row);
            }
            // Live rows on screen: the "cached" caption has done its job.
            m_cacheCaption = hstring{};
            RaisePropertyChanged(L"CacheCaption");
            RaisePropertyChanged(L"CacheCaptionVisibility");

            if (categoryAnswer)
            {
                // Read after the await: the service keeps the caveats of the
                // answer it just returned, and the chip caption belongs to it.
                SetCategoryNote(JoinNotes(engine.LastCategoryNote(), chipNote));
            }
        }
        // On failure whatever is on screen (the cached rows, or nothing) stays,
        // and the caption keeps saying it is an older answer.
        wm::app::Diag("rec list bound n=" + std::to_string(static_cast<int>(m_items.Size())) +
                      std::string{ error.empty() ? "" : " cached" });

        RaisePropertyChanged(L"HasItems");
        RaisePropertyChanged(L"ItemsVisibility");
        RaisePropertyChanged(L"EmptyVisibility");

        if (!error.empty())
        {
            SetStatus(m_items.Size() > 0
                ? hstring{ std::wstring{ L"这次没有问到引擎：" } + std::wstring{ error } +
                           L"（上面是上次的结果）" }
                : error);
        }
        else if (m_items.Size() == 0)
        {
            SetStatus(hstring{ L"推荐列表为空：先点「分析曲库」让引擎认识你的音乐" });
        }
        else
        {
            SetStatus(m_feedStateText.empty()
                ? hstring{ L"推荐来自本地音频分析（听感相似度），不是平台热榜" }
                : m_feedStateText);
        }
        SetWaiting(false);
    }

    IAsyncAction RecommendViewModel::LoadFeedAsync(bool excludeCurrent)
    {
        std::vector<hstring> excludes;
        if (excludeCurrent)
        {
            for (auto const& row : m_items)
            {
                excludes.push_back(row.TrackId());
            }
        }
        m_selectedCategoryId.clear();
        RaisePropertyChanged(L"SelectedCategoryId");

        // "换一批" is the same answer shape, so the cached batch (which is what
        // is on screen) paints again under the same header.
        auto& engine = wm::app::Recommend();
        auto const token = BeginList(hstring{ L"为你推荐" },
                                     engine.CachedRows(Answer::Feed, hstring{}, ListLimit),
                                     hstring{},
                                     engine.CachedAgeText(Answer::Feed, hstring{}, ListLimit));
        auto pending = engine.GetFeedAsync(ListLimit, std::move(excludes));
        co_await FinishList(token, std::move(pending), false, hstring{});
    }

    IAsyncAction RecommendViewModel::RefreshFeedStateAsync()
    {
        auto state = co_await wm::app::Recommend().FeedStateTextAsync();
        co_await wm::app::ResumeOnUi();
        // On failure FeedStateTextAsync returns the error text; keep whatever
        // the status line already shows instead of duplicating it.
        if (!wm::app::Recommend().LastError().empty())
        {
            co_return;
        }
        m_feedStateText = state;
        RaisePropertyChanged(L"FeedStateText");
    }

    IAsyncAction RecommendViewModel::RefreshFeedAsync()
    {
        co_await LoadFeedAsync(false);
    }

    IAsyncAction RecommendViewModel::ShuffleFeedAsync()
    {
        co_await LoadFeedAsync(true);
    }

    IAsyncAction RecommendViewModel::SelectCategoryAsync(w_music::CategoryItem category)
    {
        // |category| is a by-value parameter, so the coroutine frame keeps its
        // own interface reference. That matters: the IDL signature reaches this
        // method through the generated produce-side shim, which passes a
        // temporary. A `const&` parameter then stores a reference to that
        // temporary, and touching it after the first co_await reads freed
        // memory -- the 0xC0000005 that killed the app on an auto-* chip.
        hstring const id = category != nullptr ? category.Id() : hstring{};
        hstring const label = category != nullptr ? category.Label() : hstring{};
        hstring const note = category != nullptr ? category.Note() : hstring{};
        if (id.empty())
        {
            co_await LoadFeedAsync(false);
            co_return;
        }

        m_selectedCategoryId = std::wstring{ id };
        RaisePropertyChanged(L"SelectedCategoryId");
        wm::app::Diag("rec category select id=" + wm::app::Utf8(std::wstring{ id }));

        auto& engine = wm::app::Recommend();
        // The chip's own caption belongs to this answer either way, cached or
        // live -- it says what the category is, not what the engine thinks of
        // the rows.
        auto const token = BeginList(hstring{ L"曲风：" } + label,
                                     engine.CachedRows(Answer::Category, id, ListLimit),
                                     JoinNotes(engine.CachedNote(Answer::Category, id, ListLimit), note),
                                     engine.CachedAgeText(Answer::Category, id, ListLimit));
        auto pending = engine.GetCategoryAsync(id, ListLimit);
        co_await FinishList(token, std::move(pending), true, note);
        wm::app::Diag("rec category select done");
    }

    IAsyncAction RecommendViewModel::SearchByTextAsync(hstring text)
    {
        std::wstring const query{ text };
        if (query.empty())
        {
            SetStatus(hstring{ L"用一句话描述想听的听感，例如「安静又明亮的纯音乐」" });
            co_return;
        }

        m_selectedCategoryId = L"__text__";
        RaisePropertyChanged(L"SelectedCategoryId");

        auto& engine = wm::app::Recommend();
        auto const token = BeginList(hstring{ L"文本条件：" } + text,
                                     engine.CachedRows(Answer::Text, text, ListLimit),
                                     engine.CachedNote(Answer::Text, text, ListLimit),
                                     engine.CachedAgeText(Answer::Text, text, ListLimit));
        auto pending = engine.SearchByTextAsync(text, ListLimit);
        co_await FinishList(token, std::move(pending), true, hstring{});
    }

    IAsyncAction RecommendViewModel::LoadSimilarNowAsync()
    {
        auto seed = wm::app::Player().CurrentTrack();
        if (seed == nullptr)
        {
            SetStatus(hstring{ L"先播放一首本地歌曲，再找与它听感相似的音乐" });
            co_return;
        }
        std::wstring const path{ seed.FilePath() };
        if (path.rfind(L"http://", 0) == 0 || path.rfind(L"https://", 0) == 0)
        {
            SetStatus(hstring{ L"在线试听的曲目无法做相似推荐，播放本地歌曲后试试" });
            co_return;
        }

        m_selectedCategoryId = L"__similar__";
        RaisePropertyChanged(L"SelectedCategoryId");
        // Not cached: the answer belongs to whichever track is playing now, so
        // the list goes empty while the engine works (BeginList clears it).
        auto const token = BeginList(hstring{ L"与《" } + seed.Title() + hstring{ L"》听感相似" },
                                     winrt::single_threaded_vector<w_music::RecommendItem>().GetView(),
                                     hstring{}, hstring{});
        auto pending = wm::app::Recommend().GetSimilarByPathAsync(seed.FilePath(), ListLimit);
        co_await FinishList(token, std::move(pending), false, hstring{});
    }

    IAsyncAction RecommendViewModel::AnalyzeLibraryAsync()
    {
        SetWaiting(true);
        SetStatus(hstring{ L"正在分析本地曲库（首次可能需要几分钟，完成后自动刷新）…" });
        auto summary = co_await wm::app::Recommend().AnalyzeFoldersAsync(wm::app::Library().FolderPaths());
        co_await wm::app::ResumeOnUi();
        SetStatus(summary);
        SetWaiting(false);

        co_await RefreshFeedStateAsync();
        // auto-* categories are a function of the library: re-cluster after
        // the analysis instead of showing chips for the old library. The
        // analysis cleared the disk cache, so this is a live call.
        co_await LoadCategoriesAsync();
        co_await LoadFeedAsync(false);
    }

    IAsyncAction RecommendViewModel::ResetTasteAsync()
    {
        SetWaiting(true);
        co_await wm::app::Recommend().ResetTasteAsync();
        co_await wm::app::ResumeOnUi();

        co_await RefreshFeedStateAsync();
        co_await LoadFeedAsync(false);
    }

    // -----------------------------------------------------------------------
    // per-row actions
    // -----------------------------------------------------------------------

    void RecommendViewModel::PlayItem(w_music::RecommendItem const& item)
    {
        PlayFromList(item);
    }

    void RecommendViewModel::LikeItem(w_music::RecommendItem const& item)
    {
        // Fire and forget: the engine folds the event into its interest model.
        wm::app::Recommend().SendFeedbackAsync(item.TrackId(), hstring{ L"like" });
        SetStatus(hstring{ L"已喜欢《" } + item.Title() + hstring{ L"》，推荐会向你这类听感倾斜" });
    }

    void RecommendViewModel::DislikeItem(w_music::RecommendItem const& item)
    {
        wm::app::Recommend().SendFeedbackAsync(item.TrackId(), hstring{ L"dislike" });
        for (uint32_t i = m_items.Size(); i > 0; --i)
        {
            if (m_items.GetAt(i - 1).TrackId() == item.TrackId())
            {
                m_items.RemoveAt(i - 1);
            }
        }
        RaisePropertyChanged(L"HasItems");
        RaisePropertyChanged(L"ItemsVisibility");
        RaisePropertyChanged(L"EmptyVisibility");
        SetStatus(hstring{ L"已减少《" } + item.Title() + hstring{ L"》这类歌曲的推荐" });
    }

    void RecommendViewModel::PlayFromList(w_music::RecommendItem const& item)
    {
        auto queue = winrt::single_threaded_vector<w_music::TrackItem>();
        int32_t index = -1;
        for (uint32_t i = 0; i < m_items.Size(); ++i)
        {
            auto const& row = m_items.GetAt(i);
            if (row.TrackId() == item.TrackId() && index < 0)
            {
                index = static_cast<int32_t>(i);
            }
            if (!row.FilePath().empty())
            {
                queue.Append(TrackFromRecommend(row));
            }
        }
        if (index < 0)
        {
            return;
        }

        auto player = wm::app::Player();
        player.SetQueue(queue, index);
        player.PlayTrack(queue.GetAt(static_cast<uint32_t>(index)));
        wm::app::Recommend().SendFeedbackAsync(item.TrackId(), hstring{ L"play" });
    }
}
