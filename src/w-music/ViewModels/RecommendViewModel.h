#pragma once

#include "RecommendViewModel.g.h"

#include "Models/CategoryItem.h"
#include "Models/RecommendItem.h"

namespace winrt::w_music::implementation
{
    /// Drives the 个性推荐 page on top of the local MIR engine (the separate
    /// music-recommend project, https://github.com/PT123123/music-recommend).
    ///
    /// One list (Items) shows whichever slice was loaded last:
    ///   - the personalized feed (/v1/feed/next, default),
    ///   - one fixed category (/v1/recommend/category),
    ///   - songs similar to the currently playing track (/v1/recommend/similar).
    /// The like / dislike buttons and plain playback feed /v1/feed/feedback,
    /// which the engine folds into its time-decay interest model.
    struct RecommendViewModel : RecommendViewModelT<RecommendViewModel>
    {
        RecommendViewModel();

        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::CategoryItem>
            Categories() const noexcept { return m_categories; }
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::RecommendItem>
            Items() const noexcept { return m_items; }

        hstring ListHeader() const noexcept { return m_listHeader; }
        hstring StatusText() const noexcept { return m_statusText; }
        hstring FeedStateText() const noexcept { return m_feedStateText; }
        hstring DiscoveryText() const noexcept { return m_discoveryText; }
        hstring CategoryNote() const noexcept { return m_categoryNote; }
        winrt::Microsoft::UI::Xaml::Visibility CategoryNoteVisibility() const noexcept
        {
            return m_categoryNote.empty()
                ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                : winrt::Microsoft::UI::Xaml::Visibility::Visible;
        }
        winrt::hstring SelectedCategoryId() const noexcept { return hstring{ m_selectedCategoryId }; }
        bool IsBusy() const noexcept { return m_isBusy; }
        bool HasItems() const noexcept { return m_items.Size() > 0; }
        /// So XAML can hide the empty-state text without a converter.
        winrt::Microsoft::UI::Xaml::Visibility ItemsVisibility() const noexcept
        {
            return HasItems()
                ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
        }
        winrt::Microsoft::UI::Xaml::Visibility EmptyVisibility() const noexcept
        {
            return HasItems()
                ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                : winrt::Microsoft::UI::Xaml::Visibility::Visible;
        }

        /// First page entry: engine handshake + categories + feed. Subsequent
        /// calls are no-ops; the refresh buttons reload on demand.
        winrt::Windows::Foundation::IAsyncAction InitializeAsync();
        winrt::Windows::Foundation::IAsyncAction RefreshFeedAsync();
        /// 换一批: refetches the feed excluding the rows currently shown.
        winrt::Windows::Foundation::IAsyncAction ShuffleFeedAsync();
        /// Empty id (the "为你推荐" chip) goes back to the feed.
        winrt::Windows::Foundation::IAsyncAction SelectCategoryAsync(winrt::w_music::CategoryItem const& category);
        /// The free Chinese entry (POST /v1/recommend/category with `text`).
        /// An empty |text| returns to the feed.
        winrt::Windows::Foundation::IAsyncAction SearchByTextAsync(hstring text);
        /// Seeds /v1/recommend/similar with the file of the playing track.
        winrt::Windows::Foundation::IAsyncAction LoadSimilarNowAsync();
        /// Lets the engine analyze the w-music library folders (long).
        winrt::Windows::Foundation::IAsyncAction AnalyzeLibraryAsync();
        /// Clears the interest model, then refetches the feed.
        winrt::Windows::Foundation::IAsyncAction ResetTasteAsync();

        void PlayItem(winrt::w_music::RecommendItem const& item);
        void LikeItem(winrt::w_music::RecommendItem const& item);
        /// Dislike = feedback + row disappears from the list.
        void DislikeItem(winrt::w_music::RecommendItem const& item);

        winrt::event_token PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept { m_propertyChanged.remove(token); }

    private:
        void RaisePropertyChanged(std::wstring_view const& name);
        void SetBusy(bool value);
        void SetStatus(hstring const& text);
        /// Replaces the caveats line under the list header.
        void SetCategoryNote(hstring const& text);
        /// Runs |pending| on a worker thread and swaps the result into Items.
        winrt::Windows::Foundation::IAsyncAction LoadListAsync(
            winrt::Windows::Foundation::IAsyncOperation<
                winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::RecommendItem>> pending,
            hstring header);
        winrt::Windows::Foundation::IAsyncAction LoadFeedAsync(bool excludeCurrent);
        /// Refetch chips + the discovery caption. Called on first entry and
        /// after a library analysis, since auto-* categories are a function
        /// of the library and change with it.
        winrt::Windows::Foundation::IAsyncAction LoadCategoriesAsync();
        /// Refreshes the "口味画像" status line (best effort, never errors out).
        winrt::Windows::Foundation::IAsyncAction RefreshFeedStateAsync();
        /// Converts the current list into a playable queue and starts |item|.
        void PlayFromList(winrt::w_music::RecommendItem const& item);

        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::CategoryItem> m_categories{ nullptr };
        winrt::Windows::Foundation::Collections::IObservableVector<winrt::w_music::RecommendItem> m_items{ nullptr };

        hstring m_listHeader{ L"为你推荐" };
        hstring m_statusText;
        hstring m_feedStateText;
        hstring m_discoveryText;
        hstring m_categoryNote;
        std::wstring m_selectedCategoryId;
        bool m_isBusy = false;
        bool m_initialized = false;

        winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;
    };
}

namespace winrt::w_music::factory_implementation
{
    struct RecommendViewModel : RecommendViewModelT<RecommendViewModel, implementation::RecommendViewModel>
    {
    };
}
