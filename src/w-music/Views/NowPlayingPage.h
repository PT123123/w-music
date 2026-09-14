#pragma once

#include "NowPlayingPage.g.h"

#include "Controls/SpectrumView.h"

namespace winrt::w_music::implementation
{
    struct NowPlayingPage : NowPlayingPageT<NowPlayingPage>
    {
        NowPlayingPage();

        winrt::w_music::PlayerViewModel Player() const;

    private:
        void OnLoaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnUnloaded(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnLyricItemClick(winrt::Windows::Foundation::IInspectable const& sender,
                              winrt::Microsoft::UI::Xaml::Controls::ItemClickEventArgs const& args);
        void OnPlayerPropertyChanged(winrt::Windows::Foundation::IInspectable const& sender,
                                     winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs const& args);
        void OnProgressChanged(winrt::Windows::Foundation::IInspectable const& sender,
                               winrt::Microsoft::UI::Xaml::Controls::Primitives::RangeBaseValueChangedEventArgs const& args);
        void OnPrevClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnPlayPauseClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnNextClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnModeClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnFavoriteClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnSlowerClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnFasterClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);
        void OnResetOffsetClicked(winrt::Windows::Foundation::IInspectable const& sender, winrt::Microsoft::UI::Xaml::RoutedEventArgs const& args);

        void ScrollLyricIntoView(int32_t index);
        void UpdateTransport();

        wm::app::SpectrumView m_spectrumView;
        std::size_t m_spectrumToken = 0;
        bool m_updatingSlider = false;
        int32_t m_lastScrolled = -1;
    };
}

namespace winrt::w_music::factory_implementation
{
    struct NowPlayingPage : NowPlayingPageT<NowPlayingPage, implementation::NowPlayingPage>
    {
    };
}
