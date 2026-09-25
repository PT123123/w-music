#include "pch.h"

#include "Views/LibraryPage.h"
#include "Views/LibraryPage.g.cpp"

#include "Services/AppPaths.h"
#include "Services/LibraryService.h"
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
    LibraryPage::LibraryPage()
    {
        InitializeComponent();

        Loaded({ this, &LibraryPage::OnLoaded });
        PlaylistList().SelectionChanged({ this, &LibraryPage::OnPlaylistSelectionChanged });
        TrackList().ItemClick({ this, &LibraryPage::OnTrackItemClick });
        ShowAllButton().Click({ this, &LibraryPage::OnShowAllClicked });
        NewPlaylistButton().Click({ this, &LibraryPage::OnNewPlaylistClicked });
        DeletePlaylistButton().Click({ this, &LibraryPage::OnDeletePlaylistClicked });
        PlayPlaylistButton().Click({ this, &LibraryPage::OnPlayPlaylistClicked });
    }

    winrt::w_music::LibraryViewModel LibraryPage::ViewModel() const
    {
        return wm::app::LibraryVm();
    }

    void LibraryPage::OnLoaded(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        TrackList().ItemsSource(wm::app::LibraryVm().Tracks());
    }

    void LibraryPage::OnShowAllClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        PlaylistList().SelectedIndex(-1);
        m_selectedPlaylist = nullptr;
        ListTitle().Text(hstring{ L"全部音乐" });
        TrackList().ItemsSource(wm::app::LibraryVm().Tracks());
    }

    void LibraryPage::OnPlaylistSelectionChanged(Windows::Foundation::IInspectable const&, SelectionChangedEventArgs const&)
    {
        auto item = PlaylistList().SelectedItem().try_as<winrt::w_music::PlaylistItem>();
        if (item == nullptr)
        {
            return;
        }

        m_selectedPlaylist = item;
        ListTitle().Text(item.Name());
        wm::app::LibraryVm().OpenPlaylistAsync(item.Id());
        TrackList().ItemsSource(wm::app::LibraryVm().PlaylistTracks());
    }

    void LibraryPage::OnTrackItemClick(Windows::Foundation::IInspectable const&, ItemClickEventArgs const& args)
    {
        wm::app::Diag("click track");
        try
        {
            auto track = args.ClickedItem().try_as<winrt::w_music::TrackItem>();
            if (track == nullptr)
            {
                return;
            }

            auto player = wm::app::Player();
            std::uint32_t index = 0;

            if (auto vector = TrackList().ItemsSource().try_as<IVector<winrt::w_music::TrackItem>>())
            {
                vector.IndexOf(track, index);
                player.SetQueue(vector, static_cast<int32_t>(index));
            }

            player.PlayTrack(track);
        }
        catch (...)
        {
            wm::app::Diag("click track exception");
        }
    }

    void LibraryPage::OnPlayPlaylistClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        auto vm = wm::app::LibraryVm();

        if (m_selectedPlaylist == nullptr)
        {
            vm.PlayAll();
            return;
        }
        vm.PlayPlaylist(m_selectedPlaylist.Id());
    }

    void LibraryPage::OnNewPlaylistClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        ShowNewPlaylistDialog();
    }

    winrt::fire_and_forget LibraryPage::ShowNewPlaylistDialog()
    {
        auto lifetime = get_strong();

        TextBox box;
        box.PlaceholderText(hstring{ L"歌单名称" });
        box.MinWidth(280);

        ContentDialog dialog;
        dialog.Title(box_value(L"新建歌单"));
        dialog.Content(box);
        dialog.PrimaryButtonText(hstring{ L"创建" });
        dialog.CloseButtonText(hstring{ L"取消" });
        dialog.XamlRoot(XamlRoot());

        if (co_await dialog.ShowAsync() != ContentDialogResult::Primary)
        {
            co_return;
        }

        const auto name = box.Text();
        if (!name.empty())
        {
            wm::app::LibraryVm().CreatePlaylist(name);
        }
    }

    void LibraryPage::OnDeletePlaylistClicked(Windows::Foundation::IInspectable const&, RoutedEventArgs const&)
    {
        if (m_selectedPlaylist == nullptr || m_selectedPlaylist.IsBuiltIn())
        {
            return; // built-in playlists are protected
        }

        wm::app::Library().DeletePlaylist(m_selectedPlaylist.Id());
        m_selectedPlaylist = nullptr;
        PlaylistList().SelectedIndex(-1);
        ListTitle().Text(hstring{ L"全部音乐" });
        TrackList().ItemsSource(wm::app::LibraryVm().Tracks());
    }

    void LibraryPage::OnFavoriteClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
    {
        auto button = sender.as<Button>();
        if (button.Tag() == nullptr)
        {
            return;
        }
        const auto id = winrt::unbox_value<hstring>(button.Tag());
        if (auto track = wm::app::Library().FindTrack(id))
        {
            wm::app::LibraryVm().ToggleFavorite(track);
        }
    }

    void LibraryPage::OnAddToPlaylistClick(Windows::Foundation::IInspectable const& sender, RoutedEventArgs const&)
    {
        auto button = sender.as<Button>();
        if (button.Tag() == nullptr)
        {
            return;
        }
        const auto id = winrt::unbox_value<hstring>(button.Tag());
        if (auto track = wm::app::Library().FindTrack(id))
        {
            ShowAddToPlaylistDialog(track);
        }
    }

    winrt::fire_and_forget LibraryPage::ShowAddToPlaylistDialog(winrt::w_music::TrackItem const& track)
    {
        auto lifetime = get_strong();

        ComboBox box;
        box.MinWidth(280);
        box.PlaceholderText(hstring{ L"选择歌单" });

        for (auto const& playlist : wm::app::LibraryVm().Playlists())
        {
            if (playlist.IsBuiltIn())
            {
                continue;
            }
            ComboBoxItem item;
            item.Content(box_value(playlist.Name()));
            item.Tag(box_value(playlist.Id()));
            box.Items().Append(item);
        }

        if (box.Items().Size() == 0)
        {
            ContentDialog tip;
            tip.Title(box_value(L"还没有歌单"));
            tip.Content(box_value(L"请先在左下角新建一个歌单。"));
            tip.CloseButtonText(hstring{ L"知道了" });
            tip.XamlRoot(XamlRoot());
            co_await tip.ShowAsync();
            co_return;
        }

        box.SelectedIndex(0);

        ContentDialog dialog;
        dialog.Title(box_value(L"添加到歌单"));
        dialog.Content(box);
        dialog.PrimaryButtonText(hstring{ L"添加" });
        dialog.CloseButtonText(hstring{ L"取消" });
        dialog.XamlRoot(XamlRoot());

        if (co_await dialog.ShowAsync() != ContentDialogResult::Primary)
        {
            co_return;
        }

        auto selected = box.SelectedItem().as<ComboBoxItem>();
        wm::app::Library().AddToPlaylist(winrt::unbox_value<hstring>(selected.Tag()), track.Id());
    }
}
