#pragma once

#include "LibraryPage.g.h"

namespace winrt::w_music::implementation
{
    struct LibraryPage : LibraryPageT<LibraryPage>
    {
        LibraryPage();

        winrt::w_music::LibraryViewModel ViewModel() const;

        /// Referenced from the track DataTemplate, so XAML resolves them through
        /// the runtimeclass metadata (declared in LibraryPage.idl) -- they must
        /// be part of the projected class, not private helpers.
        void OnFavoriteClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnAddToPlaylistClick(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

    private:
        void OnLoaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnPlaylistSelectionChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                        winrt::Microsoft::UI::Xaml::Controls::SelectionChangedEventArgs const& args);
        void OnTrackItemClick(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args);
        void OnShowAllClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnNewPlaylistClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnDeletePlaylistClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnPlayPlaylistClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

        winrt::fire_and_forget ShowNewPlaylistDialog();
        /// By value: this is fire-and-forget and the caller is a plain event
        /// handler, so a reference parameter would dangle after the first await.
        winrt::fire_and_forget ShowAddToPlaylistDialog(winrt::w_music::TrackItem track);

        winrt::w_music::PlaylistItem m_selectedPlaylist{ nullptr };
    };
}

namespace winrt::w_music::factory_implementation
{
    struct LibraryPage : LibraryPageT<LibraryPage, implementation::LibraryPage>
    {
    };
}
