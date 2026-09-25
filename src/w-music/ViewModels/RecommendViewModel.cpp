#include "pch.h"

#include "ViewModels/RecommendViewModel.h"
#include "ViewModels/RecommendViewModel.g.cpp"

#include "Services/AppPaths.h"
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

    void RecommendViewModel::SetBusy(bool value)
    {
        if (m_isBusy == value)
        {
            return;
        }
        m_isBusy = value;
        RaisePropertyChanged(L"IsBusy");
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
        if (m_initialized)
        {
            co_return;
        }
        m_initialized = true;

        SetStatus(L"正在启动本地推荐引擎…");
        SetBusy(true);
        co_await LoadCategoriesAsync();
        SetBusy(false);

        co_await RefreshFeedStateAsync();
        co_await LoadFeedAsync(false);
    }

    IAsyncAction RecommendViewModel::LoadCategoriesAsync()
    {
        auto categories = co_await wm::app::Recommend().GetCategoriesAsync();
        auto discovery = co_await wm::app::Recommend().DiscoveryTextAsync();
        co_await wm::app::ResumeOnUi();

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

        m_discoveryText = discovery;
        RaisePropertyChanged(L"DiscoveryText");
        RaisePropertyChanged(L"Categories");
        RaisePropertyChanged(L"SelectedCategoryId");
    }

    IAsyncAction RecommendViewModel::LoadListAsync(IAsyncOperation<IVectorView<w_music::RecommendItem>> pending,
                                                   hstring header)
    {
        auto rows = co_await pending;
        co_await wm::app::ResumeOnUi();

        m_items.Clear();
        for (auto const& row : rows)
        {
            m_items.Append(row);
        }

        m_listHeader = std::move(header);
        RaisePropertyChanged(L"ListHeader");
        RaisePropertyChanged(L"HasItems");
        RaisePropertyChanged(L"ItemsVisibility");
        RaisePropertyChanged(L"EmptyVisibility");

        auto const error = wm::app::Recommend().LastError();
        if (!error.empty())
        {
            SetStatus(error);
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
        SetBusy(false);
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
        // The feed has no category caveats of its own.
        SetCategoryNote(hstring{});

        SetBusy(true);
        auto pending = wm::app::Recommend().GetFeedAsync(30, std::move(excludes));
        co_await LoadListAsync(std::move(pending), hstring{ L"为你推荐" });
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

    IAsyncAction RecommendViewModel::SelectCategoryAsync(w_music::CategoryItem const& category)
    {
        hstring const id = category != nullptr ? category.Id() : hstring{};
        if (id.empty())
        {
            co_await LoadFeedAsync(false);
            co_return;
        }

        m_selectedCategoryId = std::wstring{ id };
        RaisePropertyChanged(L"SelectedCategoryId");
        SetBusy(true);
        auto pending = wm::app::Recommend().GetCategoryAsync(id, 30);
        hstring header = hstring{ L"曲风：" } + category.Label();
        // The note describes exactly this answer, so it is read after the
        // await (the service keeps the caveats of the last category call).
        co_await LoadListAsync(std::move(pending), std::move(header));
        SetCategoryNote(wm::app::Recommend().LastCategoryNote());
        if (!category.Note().empty())
        {
            SetCategoryNote(m_categoryNote.empty()
                ? category.Note()
                : m_categoryNote + hstring{ L"；" } + category.Note());
        }
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
        SetBusy(true);
        auto pending = wm::app::Recommend().SearchByTextAsync(text, 30);
        hstring header = hstring{ L"文本条件：" } + text;
        co_await LoadListAsync(std::move(pending), std::move(header));
        SetCategoryNote(wm::app::Recommend().LastCategoryNote());
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
        SetCategoryNote(hstring{});
        SetBusy(true);
        auto pending = wm::app::Recommend().GetSimilarByPathAsync(seed.FilePath(), 30);
        hstring header = hstring{ L"与《" } + seed.Title() + hstring{ L"》听感相似" };
        co_await LoadListAsync(std::move(pending), std::move(header));
    }

    IAsyncAction RecommendViewModel::AnalyzeLibraryAsync()
    {
        SetBusy(true);
        SetStatus(hstring{ L"正在分析本地曲库（首次可能需要几分钟，完成后自动刷新）…" });
        auto summary = co_await wm::app::Recommend().AnalyzeFoldersAsync(wm::app::Library().FolderPaths());
        co_await wm::app::ResumeOnUi();
        SetStatus(summary);
        SetBusy(false);

        co_await RefreshFeedStateAsync();
        // auto-* categories are a function of the library: re-cluster after
        // the analysis instead of showing chips for the old library.
        co_await LoadCategoriesAsync();
        co_await LoadFeedAsync(false);
    }

    IAsyncAction RecommendViewModel::ResetTasteAsync()
    {
        SetBusy(true);
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
