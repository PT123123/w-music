#include "pch.h"

#include "ViewModels/LibraryViewModel.h"
#include "ViewModels/LibraryViewModel.g.cpp"

#include "Services/LibraryService.h"
#include "Services/Services.h"
#include "ViewModels/PlayerViewModel.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;

namespace winrt::w_music::implementation
{
    LibraryViewModel::LibraryViewModel()
    {
        m_discoverRecent = winrt::single_threaded_observable_vector<winrt::w_music::TrackItem>();
        m_discoverTop = winrt::single_threaded_observable_vector<winrt::w_music::TrackItem>();
        m_discoverShuffle = winrt::single_threaded_observable_vector<winrt::w_music::TrackItem>();
        m_playlistTracks = winrt::single_threaded_observable_vector<winrt::w_music::TrackItem>();

        auto& library = wm::app::Library();
        m_tracks = library.Tracks();
        m_playlists = library.Playlists();
    }

    void LibraryViewModel::RaisePropertyChanged(std::wstring_view const& name)
    {
        m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ hstring{ name } });
    }

    winrt::event_token LibraryViewModel::PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
    {
        return m_propertyChanged.add(handler);
    }

    void LibraryViewModel::SetStatus(hstring const& text)
    {
        m_statusText = text;
        RaisePropertyChanged(L"StatusText");
    }

    void LibraryViewModel::Fill(IObservableVector<winrt::w_music::TrackItem> const& target,
                               std::vector<winrt::w_music::TrackItem> const& source)
    {
        target.Clear();
        for (auto const& item : source)
        {
            target.Append(item);
        }
    }

    void LibraryViewModel::RefreshDiscover()
    {
        auto& library = wm::app::Library();
        Fill(m_discoverRecent, library.RecentlyAdded(12));
        Fill(m_discoverTop, library.TopPlayed(12));
        Fill(m_discoverShuffle, library.ShufflePick(12));

        m_trackCount = static_cast<int32_t>(library.Tracks().Size());
        RaisePropertyChanged(L"TrackCount");
    }

    IAsyncAction LibraryViewModel::InitializeAsync(winrt::Microsoft::UI::WindowId windowId)
    {
        m_windowId = windowId;

        auto& library = wm::app::Library();
        library.Load();
        m_tracks = library.Tracks();
        m_playlists = library.Playlists();
        RaisePropertyChanged(L"Tracks");
        RaisePropertyChanged(L"Playlists");

        RefreshDiscover();

        if (library.FolderCount() == 0)
        {
            SetStatus(hstring{ L"还没有添加音乐文件夹，点“添加文件夹”开始导入。" });
            co_return;
        }

        m_isScanning = true;
        RaisePropertyChanged(L"IsScanning");
        SetStatus(hstring{ L"正在扫描本地曲库…" });

        const int scanned = co_await library.RescanAsync();

        m_isScanning = false;
        RaisePropertyChanged(L"IsScanning");
        RefreshDiscover();
        SetStatus(hstring{ L"曲库共 " + std::to_wstring(static_cast<int>(m_tracks.Size())) + L" 首，本次处理 " + std::to_wstring(scanned) + L" 个文件" });
    }

    IAsyncAction LibraryViewModel::AddFolderAsync()
    {
        m_isScanning = true;
        RaisePropertyChanged(L"IsScanning");
        SetStatus(hstring{ L"正在扫描…" });

        const int scanned = co_await wm::app::Library().PickAndAddFolderAsync(m_windowId);

        m_isScanning = false;
        RaisePropertyChanged(L"IsScanning");
        RefreshDiscover();
        SetStatus(hstring{ L"新增 " + std::to_wstring(scanned) + L" 首歌曲，曲库共 " + std::to_wstring(m_trackCount) + L" 首" });
    }

    IAsyncAction LibraryViewModel::RescanAsync()
    {
        m_isScanning = true;
        RaisePropertyChanged(L"IsScanning");
        SetStatus(hstring{ L"正在重新扫描…" });

        const int scanned = co_await wm::app::Library().RescanAsync();

        m_isScanning = false;
        RaisePropertyChanged(L"IsScanning");
        RefreshDiscover();
        SetStatus(hstring{ L"扫描完成，处理 " + std::to_wstring(scanned) + L" 个文件，曲库共 " + std::to_wstring(m_trackCount) + L" 首" });
    }

    void LibraryViewModel::CreatePlaylist(hstring const& name)
    {
        wm::app::Library().CreatePlaylist(name);
    }

    void LibraryViewModel::ToggleFavorite(winrt::w_music::TrackItem const& track)
    {
        if (track == nullptr)
        {
            return;
        }
        const bool favorite = wm::app::Library().ToggleFavorite(track.Id());
        track.IsFavorite(favorite);
    }

    IAsyncAction LibraryViewModel::OpenPlaylistAsync(hstring const& playlistId)
    {
        auto& library = wm::app::Library();
        m_currentPlaylistName = library.PlaylistName(playlistId);
        RaisePropertyChanged(L"CurrentPlaylistName");

        m_playlistTracks.Clear();
        for (const auto& id : library.TrackIdsOfPlaylist(playlistId))
        {
            if (auto item = library.FindTrack(id))
            {
                m_playlistTracks.Append(item);
            }
        }
        co_return;
    }

    void LibraryViewModel::PlayPlaylist(hstring const& playlistId)
    {
        auto& library = wm::app::Library();
        auto queue = winrt::single_threaded_vector<winrt::w_music::TrackItem>();
        for (const auto& id : library.TrackIdsOfPlaylist(playlistId))
        {
            if (auto item = library.FindTrack(id))
            {
                queue.Append(item);
            }
        }
        if (queue.Size() == 0)
        {
            return;
        }

        auto player = wm::app::Player();
        player.SetQueue(queue, 0);
        player.PlayTrack(queue.GetAt(0));
    }

    void LibraryViewModel::PlayAll()
    {
        if (m_tracks.Size() == 0)
        {
            return;
        }
        auto player = wm::app::Player();
        player.SetQueue(m_tracks, 0);
        player.PlayTrack(m_tracks.GetAt(0));
    }
}
