#pragma once

#include "OnlinePage.g.h"

#include <map>
#include <memory>
#include <set>
#include <string>

#include <wm/core/OnlineSources.h>

namespace winrt::w_music::implementation
{
    /// The dedicated tab for online discovery -- a one-to-one replica of
    /// a-music's 发现 tab: two interchangeable built-in sources (QQ音乐 /
    /// 无损音乐) behind a switcher plus the original pluggable adapter layer,
    /// with search history, hot words, quality-tier chips, resolve-then-
    /// confirm downloads and a status line.
    struct OnlinePage : OnlinePageT<OnlinePage>
    {
        OnlinePage();

        // Click / event handlers (referenced from XAML).
        void OnSourceTabClick(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnQueryBoxKeyDown(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::Input::KeyRoutedEventArgs const& args);
        void OnRescanClick(winrt::Windows::Foundation::IInspectable const& sender,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnQqSearchClick(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnQqLoginClick(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnQqResultClick(winrt::Windows::Foundation::IInspectable const& sender,
                             winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args);
        void OnQqDownloadClick(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnNet24SearchClick(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnNet24SaveClick(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnNet24ResultClick(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args);
        void OnQualityChipClick(winrt::Windows::Foundation::IInspectable const& sender,
                                winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnClearHistoryClick(winrt::Windows::Foundation::IInspectable const& sender,
                                 winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnSearchClick(winrt::Windows::Foundation::IInspectable const& sender,
                           winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnResultAction(winrt::Windows::Foundation::IInspectable const& sender,
                            winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        // ---- source switcher / common ----
        void ApplySourceSelection(std::wstring const& source);
        std::wstring CurrentSource() const;
        void RefreshLibraryStats();
        void RefreshSuggestions();
        // WinUI 3 has no WrapPanel; VariableSizedWrapGrid is the closest panel
        // that lays children out left-to-right and wraps (with
        // Orientation="Horizontal").
        void FillWordChips(winrt::Microsoft::UI::Xaml::Controls::VariableSizedWrapGrid const& panel,
                           std::vector<hstring> const& words,
                           bool history);
        void SubmitSearch(hstring const& word);
        winrt::fire_and_forget RescanAsync();
        void PostStatus(std::wstring const& text);

        // ---- adapter section (original pluggable layer) ----
        void LoadAdapters();
        void ReloadSources();
        void OnAdapterSelectionChanged();
        void UpdateSelectionUi();
        winrt::fire_and_forget RunSearch(hstring adapterId, hstring query);
        winrt::fire_and_forget PreviewAsync(winrt::w_music::OnlineTrackItem item);
        winrt::fire_and_forget DownloadItemsAsync(
            winrt::Windows::Foundation::Collections::IVectorView<winrt::Windows::Foundation::IInspectable> const& items);

        // ---- QQ 音乐 (replica of a-music's QqSection) ----
        void QqSubmit(hstring const& word);
        winrt::fire_and_forget RunQqSearch(hstring query);
        winrt::fire_and_forget RunQqPreview(winrt::w_music::OnlineTrackItem item);
        winrt::fire_and_forget RunQqDownload(winrt::w_music::OnlineTrackItem item,
                                             winrt::Microsoft::UI::Xaml::Controls::Button downloadButton);
        void RefreshQqLoginUi();
        winrt::fire_and_forget RunQqLoginDialog();
        winrt::fire_and_forget PollQqLogin(winrt::Microsoft::UI::Xaml::Controls::ContentDialog dialog,
                                           wm::core::QqLoginFlow flow,
                                           wm::core::QqLoginContext context,
                                           winrt::Microsoft::UI::Xaml::Controls::TextBlock status);

        // ---- 无损站 (replica of a-music's Net24Section) ----
        void Net24Submit(hstring const& word);
        void RebuildSources();
        void UpdateNet24ConfigVisibility();
        winrt::fire_and_forget RunNet24Search(hstring query);
        winrt::fire_and_forget RunNet24Preview(winrt::w_music::OnlineTrackItem item);
        winrt::fire_and_forget ResolveNet24Tier(winrt::w_music::OnlineTrackItem const& row,
                                                wm::core::Net24Quality quality,
                                                winrt::Microsoft::UI::Xaml::Controls::Button chip);
        winrt::fire_and_forget RunNet24Download(winrt::w_music::OnlineTrackItem const& row,
                                                wm::core::Net24Quality quality);

        /// Superseded searches must not touch the UI when they complete.
        std::uint32_t m_qqSearchToken = 0;
        std::uint32_t m_net24SearchToken = 0;
        std::uint32_t m_searchToken = 0;

        winrt::apartment_context m_ui;
        winrt::Microsoft::UI::Dispatching::DispatcherQueue m_dispatcher{ nullptr };

        std::wstring m_currentSource;

        // QQ busy/dedupe state (mirrors a-music's loading / resolving /
        // progress / downloaded flags).
        bool m_qqLoading = false;
        bool m_qqResolving = false;
        std::set<std::wstring> m_qqDownloading;
        std::set<std::wstring> m_qqDownloaded;

        // 无损站 state.
        std::unique_ptr<wm::core::Net24Source> m_net24;
        bool m_net24Loading = false;
        bool m_net24Resolving = false;
        bool m_net24Previewing = false;
        std::set<std::wstring> m_net24Downloading;
        std::set<std::wstring> m_net24Done;
        /// Chip key -> owning row, so quality-chip clicks find their song.
        std::map<std::wstring, winrt::w_music::OnlineTrackItem> m_net24ByChipKey;
        /// Chip key -> resolved download, so repeat clicks skip the round trip.
        std::map<std::wstring, wm::core::Net24Download> m_net24Sizes;

        /// Only one preview fetch at a time in the adapter section.
        bool m_previewBusy = false;
    };
}

namespace winrt::w_music::factory_implementation
{
    struct OnlinePage : OnlinePageT<OnlinePage, implementation::OnlinePage>
    {
    };
}
