#include "pch.h"

#include "Views/DiscoverPage.h"
#include "Views/DiscoverPage.g.cpp"

#include "Services/Services.h"
#include "ViewModels/LibraryViewModel.h"
#include "ViewModels/PlayerViewModel.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::w_music::implementation
{
    namespace
    {
        std::wstring Lower(std::wstring_view value)
        {
            std::wstring result(value);
            std::transform(result.begin(), result.end(), result.begin(), [](wchar_t c) {
                return static_cast<wchar_t>(towlower(c));
            });
            return result;
        }
    } // namespace

    DiscoverPage::DiscoverPage()
    {
        InitializeComponent();

        AddFolderButton().Click([](auto&&, auto&&) { wm::app::LibraryVm().AddFolderAsync(); });
        RescanButton().Click([](auto&&, auto&&) { wm::app::LibraryVm().RescanAsync(); });
        PlayAllButton().Click([](auto&&, auto&&) { wm::app::LibraryVm().PlayAll(); });
        SearchBox().TextChanged({ this, &DiscoverPage::OnSearchTextChanged });

        ShuffleList().ItemClick([this](auto&&, auto&& args) { PlayItem(args.ClickedItem()); });
        RecentList().ItemClick([this](auto&&, auto&& args) { PlayItem(args.ClickedItem()); });
        TopList().ItemClick([this](auto&&, auto&& args) { PlayItem(args.ClickedItem()); });
        SearchList().ItemClick([this](auto&&, auto&& args) { PlayItem(args.ClickedItem()); });
    }

    winrt::w_music::LibraryViewModel DiscoverPage::ViewModel() const
    {
        return wm::app::LibraryVm();
    }

    void DiscoverPage::OnSearchTextChanged(AutoSuggestBox const& sender, AutoSuggestBoxTextChangedEventArgs const& args)
    {
        if (args.Reason() != AutoSuggestionBoxTextChangeReason::UserInput)
        {
            return;
        }
        RefreshSearchResults(sender.Text());
    }

    void DiscoverPage::RefreshSearchResults(hstring const& query)
    {
        if (query.empty())
        {
            SearchPanel().Visibility(Visibility::Collapsed);
            return;
        }

        const std::wstring needle = Lower(std::wstring_view{ query.c_str(), query.size() });
        auto results = winrt::single_threaded_observable_vector<winrt::w_music::TrackItem>();

        for (auto const& track : wm::app::LibraryVm().Tracks())
        {
            const std::wstring title = Lower(std::wstring_view{ track.Title().c_str(), track.Title().size() });
            const std::wstring artist = Lower(std::wstring_view{ track.Artist().c_str(), track.Artist().size() });
            const std::wstring album = Lower(std::wstring_view{ track.Album().c_str(), track.Album().size() });

            if (title.find(needle) != std::wstring::npos ||
                artist.find(needle) != std::wstring::npos ||
                album.find(needle) != std::wstring::npos)
            {
                results.Append(track);
            }
        }

        SearchList().ItemsSource(results);
        SearchPanel().Visibility(Visibility::Visible);
    }

    void DiscoverPage::PlayItem(IInspectable const& item)
    {
        auto track = item.try_as<winrt::w_music::TrackItem>();
        if (track == nullptr)
        {
            return;
        }
        auto player = wm::app::Player();
        player.SetQueue(wm::app::LibraryVm().Tracks(), 0);
        player.PlayTrack(track);
    }
}
