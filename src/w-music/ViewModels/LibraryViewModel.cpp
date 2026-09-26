#include "pch.h"

#include "ViewModels/LibraryViewModel.h"
#include "ViewModels/LibraryViewModel.g.cpp"

#include "Services/AppPaths.h"
#include "Services/LibraryService.h"
#include "Services/RecommendService.h"
#include "Services/Services.h"
#include "ViewModels/PlayerViewModel.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;

namespace
{
    std::string FormatHr(winrt::hresult hr)
    {
        char buf[16]{};
        snprintf(buf, sizeof(buf), "0x%08X", static_cast<uint32_t>(static_cast<int32_t>(hr)));
        return buf;
    }

    // A scan writes into %LOCALAPPDATA%\w-music\library.json as it goes, so a
    // bigger library means the songs landed and only a later step broke. The
    // status line then has to say what actually happened -- not "import failed".
    bool LibraryGrew(uint32_t tracksBefore, int& added)
    {
        const uint32_t now = wm::app::Library().Tracks().Size();
        added = static_cast<int>(now - tracksBefore);
        return now > tracksBefore;
    }

    /// 曲库（重新）加载后同步给推荐侧：缓存按曲库规模做指纹，规模一变
    /// 旧答案就不该再被端上来；推荐流的离线种子也由此重新可用。
    void NotifyRecommendLibrarySize()
    {
        wm::app::Recommend().SetLibrarySize(
            static_cast<int32_t>(wm::app::Library().Tracks().Size()));
        wm::app::Player().NotifyLibraryReady();
    }

    /// Telling XAML about a change is the *last* thing a data change does, and
    /// these coroutines are started fire-and-forget (MainWindow::OnLoaded does
    /// not await InitializeAsync) -- an exception escaping from here unwinds the
    /// coroutine into a discarded async action and leaves no trace at all. So it
    /// is named in diag.log instead, and the library stays as scanned.
    template <typename Fn>
    void NotifyUi(char const* what, Fn&& fn)
    {
        try
        {
            fn();
        }
        catch (hresult_error const& exception)
        {
            wm::app::Diag(std::string{ "ui notify " } + what + " hr=" + FormatHr(exception.code()));
        }
        catch (...)
        {
            wm::app::Diag(std::string{ "ui notify " } + what + " failed");
        }
    }
}

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
        if (!wm::app::UiThread())
        {
            wm::app::Diag("LV Raise off-ui: " + wm::app::Utf8(name));
            wm::app::PostToUi([weak = get_weak(), text = std::wstring{ name }] {
                if (auto self = weak.get())
                {
                    self->RaisePropertyChanged(text);
                }
            });
            return;
        }
        NotifyUi("PropertyChanged", [&] {
            m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ hstring{ name } });
        });
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
        // Clear()/Append() run the XAML binder synchronously (item containers are
        // realized or dropped right here), which is exactly the step that must not
        // be able to unwind a scan.
        NotifyUi("Fill", [&] {
            target.Clear();
            for (auto const& item : source)
            {
                target.Append(item);
            }
        });
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
        // 曲库规模就位：推荐缓存按它做指纹，推荐流的离线种子此刻才可用。
        NotifyRecommendLibrarySize();

        if (library.FolderCount() == 0)
        {
            SetStatus(hstring{ L"还没有添加音乐文件夹，点“添加文件夹”开始导入。" });
            co_return;
        }

        m_isScanning = true;
        RaisePropertyChanged(L"IsScanning");
        SetStatus(hstring{ L"正在快速扫描本地音乐文件…" });

        const auto tracksBefore = library.Tracks().Size();
        int scanned = 0;
        std::wstring error;
        std::wstring note;
        try
        {
            auto lifetime = get_strong();
            scanned = co_await library.RescanAsync([this, lifetime](int count) {
                RefreshDiscover();
                SetStatus(hstring{ L"已扫描 " + std::to_wstring(count) + L" 首，继续导入…" });
            });
        }
        catch (hresult_error const& exception)
        {
            const std::wstring reason{ exception.message().c_str() };
            wm::app::Diag(std::string{ "Init catch hr=" } + FormatHr(exception.code()) +
                          " msg=" + wm::app::Utf8(reason));
            int added = 0;
            if (LibraryGrew(tracksBefore, added))
            {
                scanned = added;
                note = L"（收尾步骤出错：" + reason + L"，已记入 diag.log）";
            }
            else
            {
                error = L"扫描失败：" + reason;
            }
        }
        catch (...)
        {
            wm::app::Diag("Init catch(...)");
            int added = 0;
            if (LibraryGrew(tracksBefore, added))
            {
                scanned = added;
                note = L"（收尾步骤出错，已记入 diag.log）";
            }
            else
            {
                error = L"扫描失败，请重新添加文件夹。";
            }
        }

        co_await wm::app::ResumeOnUi();
        if (error.empty())
        {
            RefreshDiscover();
            SetStatus(hstring{ L"曲库共 " + std::to_wstring(static_cast<int>(m_tracks.Size())) + L" 首，本次处理 " + std::to_wstring(scanned) + L" 个文件" + note });
        }
        else
        {
            SetStatus(hstring{ error });
        }
        m_isScanning = false;
        RaisePropertyChanged(L"IsScanning");
        NotifyRecommendLibrarySize();
    }

    IAsyncAction LibraryViewModel::AddFolderAsync()
    {
        if (m_isScanning)
        {
            co_return;
        }
        m_isScanning = true;
        RaisePropertyChanged(L"IsScanning");
        SetStatus(hstring{ L"正在快速扫描本地音乐文件…" });

        const auto tracksBefore = wm::app::Library().Tracks().Size();
        int scanned = 0;
        std::wstring error;
        std::wstring note;
        try
        {
            auto lifetime = get_strong();
            scanned = co_await wm::app::Library().PickAndAddFolderAsync(
                m_windowId,
                [this, lifetime](int count) {
                    m_trackCount = static_cast<int32_t>(wm::app::Library().Tracks().Size());
                    RaisePropertyChanged(L"TrackCount");
                    SetStatus(hstring{ L"已扫描 " + std::to_wstring(count) + L" 首，继续导入…" });
                });
        }
        catch (hresult_error const& exception)
        {
            const std::wstring reason{ exception.message().c_str() };
            wm::app::Diag(std::string{ "AddFolder catch hr=" } + FormatHr(exception.code()) +
                          " msg=" + wm::app::Utf8(reason));
            int added = 0;
            if (LibraryGrew(tracksBefore, added))
            {
                scanned = added;
                note = L"（收尾步骤出错：" + reason + L"，已记入 diag.log）";
            }
            else
            {
                error = L"导入失败：" + reason;
            }
        }
        catch (...)
        {
            wm::app::Diag("AddFolder catch(...)");
            int added = 0;
            if (LibraryGrew(tracksBefore, added))
            {
                scanned = added;
                note = L"（收尾步骤出错，已记入 diag.log）";
            }
            else
            {
                error = L"导入失败，请重新选择文件夹。";
            }
        }

        co_await wm::app::ResumeOnUi();
        if (error.empty())
        {
            RefreshDiscover();
            SetStatus(hstring{ L"新增 " + std::to_wstring(scanned) + L" 首歌曲，曲库共 " + std::to_wstring(m_trackCount) + L" 首" + note });
        }
        else
        {
            SetStatus(hstring{ error });
        }
        m_isScanning = false;
        RaisePropertyChanged(L"IsScanning");
        NotifyRecommendLibrarySize();
    }

    IAsyncAction LibraryViewModel::RescanAsync()
    {
        if (m_isScanning)
        {
            co_return;
        }
        m_isScanning = true;
        RaisePropertyChanged(L"IsScanning");
        SetStatus(hstring{ L"正在快速扫描本地音乐文件…" });

        const auto tracksBefore = wm::app::Library().Tracks().Size();
        int scanned = 0;
        std::wstring error;
        std::wstring note;
        try
        {
            auto lifetime = get_strong();
            scanned = co_await wm::app::Library().RescanAsync([this, lifetime](int count) {
                RefreshDiscover();
                SetStatus(hstring{ L"已扫描 " + std::to_wstring(count) + L" 首，继续导入…" });
            });
        }
        catch (hresult_error const& exception)
        {
            const std::wstring reason{ exception.message().c_str() };
            wm::app::Diag(std::string{ "Rescan catch hr=" } + FormatHr(exception.code()) +
                          " msg=" + wm::app::Utf8(reason));
            int added = 0;
            if (LibraryGrew(tracksBefore, added))
            {
                scanned = added;
                note = L"（收尾步骤出错：" + reason + L"，已记入 diag.log）";
            }
            else
            {
                error = L"扫描失败：" + reason;
            }
        }
        catch (...)
        {
            wm::app::Diag("Rescan catch(...)");
            int added = 0;
            if (LibraryGrew(tracksBefore, added))
            {
                scanned = added;
                note = L"（收尾步骤出错，已记入 diag.log）";
            }
            else
            {
                error = L"扫描失败，请重新添加文件夹。";
            }
        }

        co_await wm::app::ResumeOnUi();
        if (error.empty())
        {
            RefreshDiscover();
            SetStatus(hstring{ L"扫描完成，处理 " + std::to_wstring(scanned) + L" 个文件，曲库共 " + std::to_wstring(m_trackCount) + L" 首" + note });
        }
        else
        {
            SetStatus(hstring{ error });
        }
        m_isScanning = false;
        RaisePropertyChanged(L"IsScanning");
        NotifyRecommendLibrarySize();
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
