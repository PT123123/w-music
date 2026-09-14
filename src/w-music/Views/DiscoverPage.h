#pragma once

#include "DiscoverPage.g.h"

namespace winrt::w_music::implementation
{
    struct DiscoverPage : DiscoverPageT<DiscoverPage>
    {
        DiscoverPage();

        winrt::w_music::LibraryViewModel ViewModel() const;

    private:
        void OnSearchTextChanged(winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBox const& sender,
                                 winrt::Microsoft::UI::Xaml::Controls::AutoSuggestBoxTextChangedEventArgs const& args);
        void PlayItem(winrt::Windows::Foundation::IInspectable const& item);
        void RefreshSearchResults(hstring const& query);
    };
}

namespace winrt::w_music::factory_implementation
{
    struct DiscoverPage : DiscoverPageT<DiscoverPage, implementation::DiscoverPage>
    {
    };
}
