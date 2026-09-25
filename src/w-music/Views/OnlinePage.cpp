#include "pch.h"

#include "Views/OnlinePage.h"
#include "Views/OnlinePage.g.cpp"

#include "Services/Services.h"
#include "Services/OnlineProviderService.h"
#include "Services/DiscoverSettings.h"
#include "Services/LibraryService.h"
#include "Services/AppPaths.h"
#include "ViewModels/PlayerViewModel.h"

#include "Models/OnlineTrackItem.h"
#include "Models/QualityChipItem.h"
#include "Models/TrackItem.h"

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Foundation::Collections;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;

namespace winrt::w_music::implementation
{
    namespace
    {
        std::string Narrow(hstring const& value)
        {
            return value.empty() ? std::string{} : wm::app::Utf8(std::wstring_view{ value.c_str(), value.size() });
        }

        hstring Wide(std::string const& value)
        {
            return hstring{ wm::app::Utf16(value) };
        }

        std::wstring Trimmed(std::wstring const& text)
        {
            std::size_t begin = 0;
            std::size_t end = text.size();
            while (begin < end && iswspace(text[begin])) ++begin;
            while (end > begin && iswspace(text[end - 1])) --end;
            return text.substr(begin, end - begin);
        }

        std::wstring SanitizeFileName(std::wstring const& name)
        {
            static const std::wstring kForbidden = L"\\/:*?\"<>|\r\n";
            std::wstring out;
            out.reserve(name.size());
            for (wchar_t c : name)
            {
                out.push_back(kForbidden.find(c) == std::wstring::npos ? c : L'_');
            }
            while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
            {
                out.pop_back();
            }
            return out.empty() ? std::wstring(L"track") : out;
        }

        std::wstring FmtBytes(std::uint64_t bytes)
        {
            wchar_t buffer[32]{};
            if (bytes >= 1ull << 30)
            {
                swprintf_s(buffer, L"%.2f GB", static_cast<double>(bytes) / 1073741824.0);
            }
            else if (bytes >= 1ull << 20)
            {
                swprintf_s(buffer, L"%.1f MB", static_cast<double>(bytes) / 1048576.0);
            }
            else if (bytes > 0)
            {
                swprintf_s(buffer, L"%.0f KB", static_cast<double>(bytes) / 1024.0);
            }
            else
            {
                return L"0 B";
            }
            return buffer;
        }

        /// File name for a QQ download: "歌手 - 歌名.m4a" (a-music fileNameFor).
        std::wstring QqFileName(winrt::w_music::OnlineTrackItem const& item)
        {
            std::wstring displayName = Trimmed(std::wstring{ item.Artist().c_str() });
            if (!displayName.empty())
            {
                displayName += L" - ";
            }
            displayName += std::wstring{ item.Title().c_str() };
            std::wstring base = SanitizeFileName(displayName);
            if (base.size() > 120)
            {
                base.resize(120);
            }
            return base + L".m4a";
        }

        std::wstring QualityNameOf(wm::core::Net24Quality quality)
        {
            return wm::app::Utf16(Net24QualityLabel(quality));
        }

        wm::core::Net24Quality QualityOfTypeLetter(std::wstring const& type)
        {
            if (type == L"a")
            {
                return wm::core::Net24Quality::Master;
            }
            if (type == L"c")
            {
                return wm::core::Net24Quality::Surround;
            }
            return wm::core::Net24Quality::Lossless;
        }

        /// Row item -> core song description (net24 resolve/preview input).
        wm::core::Net24Song ToNet24Song(winrt::w_music::OnlineTrackItem const& item)
        {
            wm::core::Net24Song song;
            song.title = Narrow(item.Title());
            song.artist = Narrow(item.Artist());
            song.album = Narrow(item.Album());
            song.masterId = Narrow(item.MasterId());
            song.losslessId = Narrow(item.LosslessId());
            return song;
        }

        winrt::w_music::OnlineTrackItem ToQqItem(wm::core::QqSong const& song)
        {
            auto item = winrt::make<winrt::w_music::implementation::OnlineTrackItem>();
            item.SourceId(L"qq");
            item.Id(Wide(song.mid));
            item.Title(Wide(song.title));
            item.Artist(Wide(song.artist));
            item.Album(Wide(song.album));
            item.DurationText(Wide(song.DurationText()));
            item.DurationMs(static_cast<std::int64_t>(song.durationSec) * 1000);
            item.CoverUrl(Wide(song.CoverUrl()));
            item.ExtraText(Wide(song.SizeText()));
            item.VipOnly(song.vipOnly);
            return item;
        }

        winrt::w_music::OnlineTrackItem ToNet24Item(wm::core::Net24Song const& song)
        {
            auto item = winrt::make<winrt::w_music::implementation::OnlineTrackItem>();
            item.SourceId(L"net24");
            item.Id(Wide(song.Id()));
            item.Title(Wide(song.title));
            item.Artist(Wide(song.artist));
            item.Album(Wide(song.album));
            item.CoverUrl(Wide(song.coverUrl));
            item.MasterId(Wide(song.masterId));
            item.LosslessId(Wide(song.losslessId));
            // "无损站 · 来自母带源 / 无损源 / 母带源 + 无损源"
            item.ExtraText(hstring{ L"无损站 · 来自" } + Wide(song.SourceLabel()));
            for (wm::core::Net24Quality const quality : song.Qualities())
            {
                item.AddQuality(Wide(song.Key(quality)), Wide(Net24QualityShort(quality)));
            }
            return item;
        }

        /// QqSource pre-wired with the persisted login session (when present),
        /// so preview/direct-url requests honour the logged-in account.
        wm::core::QqSource QqSourceWithSession()
        {
            wm::core::QqSource source{ wm::app::Online().Transport() };
            auto& settings = wm::app::Settings();
            if (settings.QqLoggedIn())
            {
                std::wstring const cookie{ settings.QqSessionCookie().c_str() };
                std::wstring const uin{ settings.QqUin().c_str() };
                source.SetSession(
                    wm::app::Utf8(std::wstring_view{ cookie.data(), cookie.size() }),
                    wm::app::Utf8(std::wstring_view{ uin.data(), uin.size() }));
            }
            return source;
        }
    } // namespace

    OnlinePage::OnlinePage()
    {
        InitializeComponent();

        m_dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
        m_currentSource = wm::app::Settings().DiscoverSource();

        // ---- adapter section (wired in code, not XAML) ----
        ReloadAdaptersButton().Click([this](auto&&, auto&&) { ReloadSources(); });
        AdapterCombo().SelectionChanged([this](auto&&, auto&&) { OnAdapterSelectionChanged(); });
        DownloadSelectedButton().Click([this](auto&&, auto&&) {
            DownloadItemsAsync(ResultList().SelectedItems().GetView());
        });
        ResultList().SelectionChanged([this](auto&&, auto&&) { UpdateSelectionUi(); });

        RebuildSources();
        LoadAdapters();
        ApplySourceSelection(m_currentSource);
        RefreshQqLoginUi();
        RefreshSuggestions();
        RefreshLibraryStats();
    }

    // =====================================================================
    // == common                                                          ==
    // =====================================================================

    void OnlinePage::OnSourceTabClick(IInspectable const& sender, RoutedEventArgs const&)
    {
        auto button = sender.try_as<Primitives::ToggleButton>();
        if (button == nullptr)
        {
            return;
        }
        const std::wstring tag = winrt::unbox_value_or<hstring>(button.Tag(), hstring{}).c_str();
        m_currentSource = tag;
        wm::app::Settings().DiscoverSource(tag);
        ApplySourceSelection(tag);
    }

    std::wstring OnlinePage::CurrentSource() const
    {
        return m_currentSource;
    }

    void OnlinePage::ApplySourceSelection(std::wstring const& source)
    {
        QqTab().IsChecked(source == L"qq");
        Net24Tab().IsChecked(source == L"net24");
        AdapterTab().IsChecked(source == L"adapter");

        QqSection().Visibility(source == L"qq" ? Visibility::Visible : Visibility::Collapsed);
        Net24Section().Visibility(source == L"net24" ? Visibility::Visible : Visibility::Collapsed);
        AdapterSection().Visibility(source == L"adapter" ? Visibility::Visible : Visibility::Collapsed);

        if (source == L"qq")
        {
            HeaderTagline().Text(L"QQ音乐公共接口 · 在线试听 · 下载 m4a（复刻 a-music 发现页）");
        }
        else if (source == L"net24")
        {
            HeaderTagline().Text(L"母带 / 环绕 / 无损 · 直链 flac（复刻 a-music 发现页）");
        }
        else
        {
            HeaderTagline().Text(L"自定义适配器源 · JSON 规则（见 adapters/README.md）");
        }
        UpdateNet24ConfigVisibility();
    }

    void OnlinePage::OnQueryBoxKeyDown(IInspectable const& sender, Input::KeyRoutedEventArgs const& args)
    {
        if (args.Key() != winrt::Windows::System::VirtualKey::Enter)
        {
            return;
        }
        auto box = sender.try_as<TextBox>();
        if (box == nullptr)
        {
            return;
        }
        if (box == QqQueryBox())
        {
            QqSubmit(box.Text());
        }
        else if (box == Net24QueryBox())
        {
            Net24Submit(box.Text());
        }
        else if (box == QueryBox())
        {
            OnSearchClick(nullptr, nullptr);
        }
    }

    void OnlinePage::SubmitSearch(hstring const& word)
    {
        if (m_currentSource == L"qq")
        {
            QqSubmit(word);
        }
        else if (m_currentSource == L"net24")
        {
            Net24Submit(word);
        }
    }

    winrt::fire_and_forget OnlinePage::RescanAsync()
    {
        auto lifetime = get_strong();
        RescanButton().IsEnabled(false);
        OnlineStatus().Text(L"正在重新扫描本地音乐文件夹…");
        co_await wm::app::Library().RescanAsync();
        co_await wm::app::ResumeOnUi();
        RescanButton().IsEnabled(true);
        RefreshLibraryStats();
        OnlineStatus().Text(L"本地曲库已更新");
    }

    void OnlinePage::OnRescanClick(IInspectable const&, RoutedEventArgs const&)
    {
        RescanAsync();
    }

    void OnlinePage::RefreshLibraryStats()
    {
        std::size_t tracks = 0;
        std::size_t favorites = 0;
        for (winrt::w_music::TrackItem const& track : wm::app::Library().Tracks())
        {
            if (track == nullptr)
            {
                continue;
            }
            ++tracks;
            if (track.IsFavorite())
            {
                ++favorites;
            }
        }
        LibraryStatsLine().Text(hstring{
            L"本地 " + std::to_wstring(tracks) + L" 首 · 收藏 " + std::to_wstring(favorites) +
            L" 首 · 下载完自动加入本地曲库（下载到 Downloads 文件夹）" });
    }

    void OnlinePage::RefreshSuggestions()
    {
        auto const& history = wm::app::Settings().SearchHistory();
        std::vector<hstring> historyWords;
        for (std::wstring const& word : history)
        {
            historyWords.push_back(hstring{ word });
        }
        const bool hasHistory = !historyWords.empty();

        QqHistoryPanel().Visibility(hasHistory ? Visibility::Visible : Visibility::Collapsed);
        Net24HistoryPanel().Visibility(hasHistory ? Visibility::Visible : Visibility::Collapsed);
        FillWordChips(QqHistoryList(), historyWords, true);
        FillWordChips(Net24HistoryList(), historyWords, true);

        std::vector<hstring> qqHot;
        for (std::string const& word : wm::core::QqSource::HotWords())
        {
            qqHot.push_back(Wide(word));
        }
        FillWordChips(QqHotList(), qqHot, false);

        std::vector<hstring> net24Hot;
        for (std::string const& word : wm::core::Net24Source::HotWords())
        {
            net24Hot.push_back(Wide(word));
        }
        FillWordChips(Net24HotList(), net24Hot, false);
    }

    void OnlinePage::FillWordChips(VariableSizedWrapGrid const& panel, std::vector<hstring> const& words, bool history)
    {
        if (panel == nullptr)
        {
            return;
        }
        panel.Children().Clear();
        for (hstring const& word : words)
        {
            Button chip;
            chip.Content(winrt::box_value(word));
            chip.FontSize(12.5);
            chip.Padding(ThicknessHelper::FromUniformLength(6));
            chip.CornerRadius(CornerRadiusHelper::FromUniformRadius(14));
            chip.Margin(ThicknessHelper::FromLengths(0, 0, 8, 8));

            chip.Click([this, word](IInspectable const&, RoutedEventArgs const&) {
                SubmitSearch(word);
            });
            if (history)
            {
                MenuFlyout flyout;
                MenuFlyoutItem remove;
                remove.Text(L"删除该历史");
                remove.Click([this, word](IInspectable const&, IInspectable const&) {
                    wm::app::Settings().RemoveSearchHistory(std::wstring{ word.c_str() });
                    RefreshSuggestions();
                });
                flyout.Items().Append(remove);
                chip.ContextFlyout(flyout);
            }
            panel.Children().Append(chip);
        }
    }

    void OnlinePage::OnClearHistoryClick(IInspectable const&, RoutedEventArgs const&)
    {
        wm::app::Settings().ClearSearchHistory();
        RefreshSuggestions();
        OnlineStatus().Text(L"已清空搜索历史。");
    }

    void OnlinePage::PostStatus(std::wstring const& text)
    {
        if (m_dispatcher == nullptr)
        {
            return;
        }
        auto weak = get_weak();
        m_dispatcher.TryEnqueue([weak, text]() {
            if (auto strong = weak.get())
            {
                strong->OnlineStatus().Text(hstring{ text });
            }
        });
    }

    // =====================================================================
    // == QQ 音乐                                                          ==
    // =====================================================================

    void OnlinePage::QqSubmit(hstring const& word)
    {
        const std::wstring trimmed = Trimmed(std::wstring{ word.c_str() });
        if (trimmed.empty() || m_qqLoading)
        {
            return;
        }
        QqQueryBox().Text(hstring{ trimmed });
        wm::app::Settings().AddSearchHistory(trimmed);
        RefreshSuggestions();
        RunQqSearch(hstring{ trimmed });
    }

    void OnlinePage::OnQqSearchClick(IInspectable const&, RoutedEventArgs const&)
    {
        QqSubmit(QqQueryBox().Text());
    }

    winrt::fire_and_forget OnlinePage::RunQqSearch(hstring query)
    {
        auto lifetime = get_strong();
        const std::uint32_t token = ++m_qqSearchToken;
        m_qqLoading = true;
        QqSearchRing().IsActive(true);
        QqEmptyHint().Visibility(Visibility::Collapsed);
        QqSuggestions().Visibility(Visibility::Collapsed);
        OnlineStatus().Text(L"搜索中…");

        std::vector<winrt::w_music::OnlineTrackItem> rows;
        co_await winrt::resume_background();
        {
            wm::core::QqSource source{ wm::app::Online().Transport() };
            for (wm::core::QqSong const& song : source.Search(Narrow(query), 1, 20))
            {
                rows.push_back(ToQqItem(song));
            }
        }
        co_await wm::app::ResumeOnUi();

        if (token != m_qqSearchToken)
        {
            co_return;   // a newer search superseded this one
        }
        m_qqLoading = false;
        QqSearchRing().IsActive(false);

        auto items = winrt::single_threaded_vector<winrt::w_music::OnlineTrackItem>();
        for (auto const& row : rows)
        {
            items.Append(row);
        }
        QqResultList().ItemsSource(items);

        if (rows.empty())
        {
            QqSuggestions().Visibility(Visibility::Visible);
            QqEmptyHint().Visibility(Visibility::Visible);
            OnlineStatus().Text(L"没有搜到结果，换个关键词试试");
        }
        else
        {
            OnlineStatus().Text(hstring{ L"找到 " + std::to_wstring(rows.size()) +
                                         L" 条 · 点行试听，点 ⤓ 下载（VIP 曲目无法试听/下载）" });
        }
    }

    winrt::fire_and_forget OnlinePage::RunQqPreview(winrt::w_music::OnlineTrackItem item)
    {
        auto lifetime = get_strong();
        if (item == nullptr)
        {
            co_return;
        }
        if (m_qqResolving)
        {
            OnlineStatus().Text(L"上一条试听还在解析中，稍候…");
            co_return;
        }
        m_qqResolving = true;
        OnlineStatus().Text(L"解析试听地址…");

        std::string url;
        co_await winrt::resume_background();
        {
            wm::core::QqSource source = QqSourceWithSession();
            url = source.DirectUrl(Narrow(item.Id()));
        }
        co_await wm::app::ResumeOnUi();
        m_qqResolving = false;

        if (url.empty())
        {
            OnlineStatus().Text(hstring{ L"《" + std::wstring{ item.Title().c_str() } + L"》需要 VIP，无法试听" });
            co_return;
        }

        auto track = winrt::make<winrt::w_music::implementation::TrackItem>();
        track.Id(hstring{ L"online:qq:" + std::wstring{ item.Id().c_str() } + L":preview" });
        track.Title(item.Title());
        track.Artist(item.Artist());
        track.Album(item.Album());
        track.DurationMs(item.DurationMs());
        track.FilePath(hstring{ wm::app::Utf16(url) });
        wm::app::Player().PlayTrack(track);
        OnlineStatus().Text(hstring{ L"正在播放：" + std::wstring{ item.Title().c_str() } +
                                     L"（喜欢就点行内的 ⤓ 下载入库）" });
    }

    // ------------------------------------------------------ QQ 扫码登录

    void OnlinePage::RefreshQqLoginUi()
    {
        auto& settings = wm::app::Settings();
        if (settings.QqLoggedIn())
        {
            QqLoginButton().Content(box_value(hstring{ L"退出登录" }));
            QqLoginStatus().Text(hstring{ L"已登录 QQ：" + settings.QqUin() });
        }
        else
        {
            QqLoginButton().Content(box_value(hstring{ L"扫码登录" }));
            QqLoginStatus().Text(L"未登录 · 仅免费音质");
        }
    }

    void OnlinePage::OnQqLoginClick(IInspectable const&, RoutedEventArgs const&)
    {
        auto& settings = wm::app::Settings();
        if (settings.QqLoggedIn())
        {
            ContentDialog confirm;
            confirm.Title(box_value(hstring{ L"退出 QQ 登录" }));
            confirm.Content(box_value(hstring{ L"确定要退出当前 QQ 账号吗？" }));
            confirm.PrimaryButtonText(L"退出");
            confirm.CloseButtonText(L"取消");
            confirm.XamlRoot(XamlRoot());
            if (confirm.ShowAsync().get() == ContentDialogResult::Primary)
            {
                settings.ClearQqSession();
                RefreshQqLoginUi();
                OnlineStatus().Text(L"已退出登录。");
            }
            return;
        }
        RunQqLoginDialog();
    }

    winrt::fire_and_forget OnlinePage::RunQqLoginDialog()
    {
        auto lifetime = get_strong();
        if (m_qqResolving)
        {
            co_return;
        }
        m_qqResolving = true;

        // 1. Fetch a fresh QR code off the worker thread.
        wm::core::QqLoginFlow flow{ wm::app::Online().Transport() };
        wm::core::QqLoginContext context;
        co_await winrt::resume_background();
        context = flow.FetchQr();
        co_await wm::app::ResumeOnUi();
        m_qqResolving = false;

        if (!context.ok)
        {
            ContentDialog error;
            error.Title(box_value(hstring{ L"登录失败" }));
            error.Content(box_value(hstring{ wm::app::Utf16(context.reason) }));
            error.CloseButtonText(L"好");
            error.XamlRoot(XamlRoot());
            error.ShowAsync();
            co_return;
        }

        // 2. Compose the dialog: QR picture + live status line.
        auto qrImage = Media::Imaging::BitmapImage{};
        try
        {
            Windows::Storage::Streams::InMemoryRandomAccessStream stream;
            Windows::Storage::Streams::DataWriter writer{ stream };
            std::vector<std::uint8_t> bytes(context.qrImage.begin(), context.qrImage.end());
            writer.WriteBytes(bytes);
            writer.StoreAsync().get();
            stream.Seek(0);
            qrImage.SetSourceAsync(stream).get();
        }
        catch (...)
        {
            // QR payload malformed; the status line below explains the failure.
        }

        auto image = Image{};
        image.Width(220);
        image.Height(220);
        image.Source(qrImage);

        auto status = TextBlock{};
        status.Text(L"请用手机 QQ 扫描二维码");
        status.HorizontalAlignment(HorizontalAlignment::Center);
        status.FontSize(13);
        status.Opacity(0.85);

        auto hint = TextBlock{};
        hint.Text(L"登录后播放/下载可享当前账号的会员权益");
        hint.HorizontalAlignment(HorizontalAlignment::Center);
        hint.FontSize(11.5);
        hint.Opacity(0.55);

        auto panel = StackPanel{};
        panel.Spacing(10);
        panel.Children().Append(image);
        panel.Children().Append(status);
        panel.Children().Append(hint);

        ContentDialog dialog;
        dialog.Title(box_value(hstring{ L"QQ 音乐 · 扫码登录" }));
        dialog.Content(panel);
        dialog.CloseButtonText(L"取消");
        dialog.XamlRoot(XamlRoot());
        dialog.Closed([weak = get_weak()](IInspectable const&, ContentDialogClosedEventArgs const&) {
            // The dialog is gone; nothing else to reset (m_qqResolving was
            // already cleared on open).
            (void)weak;
        });

        // 3. Show and poll until the phone confirms.
        PollQqLogin(dialog, flow, context, status);
        dialog.ShowAsync();
    }

    winrt::fire_and_forget OnlinePage::PollQqLogin(ContentDialog dialog,
                                                   wm::core::QqLoginFlow flow,
                                                   wm::core::QqLoginContext context,
                                                   TextBlock status)
    {
        auto lifetime = get_strong();

        wm::core::QqLoginResult result;
        for (int attempt = 0; attempt < 30; ++attempt)   // ~60s cap
        {
            co_await winrt::resume_after(std::chrono::seconds{ 2 });
            co_await winrt::resume_background();
            result = flow.CheckStatus(context);
            co_await wm::app::ResumeOnUi();

            switch (result.status)
            {
            case wm::core::QqLoginStatus::Scanned:
                status.Text(L"已扫码，请在手机上确认登录");
                continue;
            case wm::core::QqLoginStatus::Waiting:
                status.Text(L"等待扫码…");
                continue;
            case wm::core::QqLoginStatus::Success:
            {
                status.Text(L"登录成功！");
                wm::app::Settings().SetQqSession(
                    wm::app::Utf16(result.cookie),
                    wm::app::Utf16(result.uin));
                RefreshQqLoginUi();
                OnlineStatus().Text(hstring{ L"已登录 QQ：" + wm::app::Utf16(result.uin) });
                dialog.Hide();
                co_return;
            }
            default:
                status.Text(hstring{ L"登录失败：" + wm::app::Utf16(result.reason) });
                co_return;
            }
        }
        status.Text(L"二维码已过期，请重新打开登录窗口");
    }

    void OnlinePage::OnQqResultClick(IInspectable const&, Controls::ItemClickEventArgs const& args)
    {
        if (auto item = args.ClickedItem().try_as<winrt::w_music::OnlineTrackItem>())
        {
            RunQqPreview(item);
        }
    }

    void OnlinePage::OnQqDownloadClick(IInspectable const& sender, RoutedEventArgs const&)
    {
        if (auto button = sender.try_as<Button>())
        {
            if (auto item = button.DataContext().try_as<winrt::w_music::OnlineTrackItem>())
            {
                RunQqDownload(item, button);
            }
        }
    }

    winrt::fire_and_forget OnlinePage::RunQqDownload(winrt::w_music::OnlineTrackItem item,
                                                     Button downloadButton)
    {
        auto lifetime = get_strong();
        const std::wstring mid{ item.Id().c_str() };
        if (m_qqDownloading.contains(mid) || m_qqDownloaded.contains(mid))
        {
            co_return;
        }
        m_qqDownloading.insert(mid);
        winrt::Windows::Foundation::IInspectable const originalContent = downloadButton.Content();
        downloadButton.IsEnabled(false);
        downloadButton.Content(winrt::box_value(hstring{ L"…" }));
        OnlineStatus().Text(hstring{ L"解析下载地址：" + std::wstring{ item.Title().c_str() } + L" …" });

        std::string url;
        co_await winrt::resume_background();
        {
            wm::core::QqSource source = QqSourceWithSession();
            url = source.DirectUrl(Narrow(item.Id()));
        }
        co_await wm::app::ResumeOnUi();
        if (url.empty())
        {
            OnlineStatus().Text(hstring{ L"《" + std::wstring{ item.Title().c_str() } +
                                         L"》需要 VIP 或已下架，无法下载" });
            downloadButton.Content(originalContent);
            downloadButton.IsEnabled(true);
            m_qqDownloading.erase(mid);
            co_return;
        }

        // Stream the m4a into the Downloads folder, then import it.
        std::filesystem::path const target =
            std::filesystem::path{ wm::app::Online().DownloadsDirectory() } / QqFileName(item);
        OnlineStatus().Text(hstring{ L"开始下载：" + std::wstring{ item.Title().c_str() } });

        bool ok = false;
        co_await winrt::resume_background();
        {
            double lastFraction = -1.0;
            ok = wm::app::Online().DownloadUrlToFile(
                target.wstring(), url, wm::core::QqSource::DownloadHeaders(), 0,
                [this, &lastFraction](double fraction) {
                    if (fraction - lastFraction >= 0.01 || fraction >= 1.0)
                    {
                        lastFraction = fraction;
                        PostStatus(L"下载中 " + std::to_wstring(static_cast<int>(fraction * 100)) + L"%");
                    }
                });
        }
        co_await wm::app::ResumeOnUi();

        if (!ok)
        {
            OnlineStatus().Text(hstring{ L"下载失败：" + std::wstring{ item.Title().c_str() } });
            downloadButton.Content(originalContent);
            downloadButton.IsEnabled(true);
            m_qqDownloading.erase(mid);
            co_return;
        }

        winrt::w_music::TrackItem imported{ nullptr };
        try
        {
            auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(target.wstring());
            imported = co_await wm::app::Library().ImportFileAsync(file);
        }
        catch (...)
        {
            imported = nullptr;
        }
        co_await wm::app::ResumeOnUi();

        m_qqDownloading.erase(mid);
        if (imported == nullptr)
        {
            OnlineStatus().Text(hstring{ L"下载完成但导入失败：" + std::wstring{ item.Title().c_str() } });
            downloadButton.Content(originalContent);
            downloadButton.IsEnabled(true);
            co_return;
        }

        m_qqDownloaded.insert(mid);
        downloadButton.Content(winrt::box_value(hstring{ L"✓" }));
        OnlineStatus().Text(hstring{ L"已下载：" + std::wstring{ item.Title().c_str() } +
                                     L"（已加入本地曲库）" });
        RefreshLibraryStats();
    }

    // =====================================================================
    // == 无损站                                                           ==
    // =====================================================================

    void OnlinePage::RebuildSources()
    {
        m_net24 = std::make_unique<wm::core::Net24Source>(
            Narrow(hstring{ wm::app::Settings().Net24BaseUrl() }),
            wm::app::Online().Transport());
    }

    void OnlinePage::UpdateNet24ConfigVisibility()
    {
        const bool configured = !wm::app::Settings().Net24BaseUrl().empty();
        Net24ConfigPanel().Visibility(configured ? Visibility::Collapsed : Visibility::Visible);
        if (!configured && Net24UrlBox().Text().empty())
        {
            Net24UrlBox().Text(hstring{ wm::app::Settings().Net24BaseUrl() });
        }
    }

    void OnlinePage::Net24Submit(hstring const& word)
    {
        const std::wstring trimmed = Trimmed(std::wstring{ word.c_str() });
        if (trimmed.empty() || m_net24Loading)
        {
            return;
        }
        if (wm::app::Settings().Net24BaseUrl().empty())
        {
            OnlineStatus().Text(L"先填写无损站地址（下方输入框），保存后再搜索。");
            return;
        }
        Net24QueryBox().Text(hstring{ trimmed });
        wm::app::Settings().AddSearchHistory(trimmed);
        RefreshSuggestions();
        RunNet24Search(hstring{ trimmed });
    }

    void OnlinePage::OnNet24SearchClick(IInspectable const&, RoutedEventArgs const&)
    {
        Net24Submit(Net24QueryBox().Text());
    }

    void OnlinePage::OnNet24SaveClick(IInspectable const&, RoutedEventArgs const&)
    {
        std::wstring const url = Trimmed(std::wstring{ Net24UrlBox().Text().c_str() });
        wm::app::Settings().Net24BaseUrl(url);
        RebuildSources();
        UpdateNet24ConfigVisibility();
        OnlineStatus().Text(url.empty()
            ? L"已清空无损站地址。"
            : L"无损站地址已保存。");
    }

    winrt::fire_and_forget OnlinePage::RunNet24Search(hstring query)
    {
        auto lifetime = get_strong();
        const std::uint32_t token = ++m_net24SearchToken;
        m_net24Loading = true;
        Net24SearchRing().IsActive(true);
        Net24EmptyHint().Visibility(Visibility::Collapsed);
        Net24Suggestions().Visibility(Visibility::Collapsed);
        OnlineStatus().Text(L"搜索中…");

        std::vector<winrt::w_music::OnlineTrackItem> rows;
        std::map<std::wstring, winrt::w_music::OnlineTrackItem> byChipKey;
        co_await winrt::resume_background();
        {
            // Both catalogues (searchOnlineMusicOne/Two) are fetched and folded
            // inside the source, exactly like a-music's Net24Api.search.
            for (wm::core::Net24Song const& song : m_net24->Search(Narrow(query), 1))
            {
                auto item = ToNet24Item(song);
                for (winrt::w_music::QualityChipItem const& chip : item.Qualities())
                {
                    byChipKey[std::wstring{ chip.Key().c_str() }] = item;
                }
                rows.push_back(std::move(item));
            }
        }
        co_await wm::app::ResumeOnUi();

        if (token != m_net24SearchToken)
        {
            co_return;
        }
        m_net24Loading = false;
        Net24SearchRing().IsActive(false);
        m_net24ByChipKey = std::move(byChipKey);

        auto items = winrt::single_threaded_vector<winrt::w_music::OnlineTrackItem>();
        for (auto const& row : rows)
        {
            items.Append(row);
        }
        Net24ResultList().ItemsSource(items);

        if (rows.empty())
        {
            Net24Suggestions().Visibility(Visibility::Visible);
            Net24EmptyHint().Visibility(Visibility::Visible);
            OnlineStatus().Text(L"没有搜到结果，换个关键词试试");
        }
        else
        {
            OnlineStatus().Text(hstring{ L"找到 " + std::to_wstring(rows.size()) +
                                         L" 条 · 点行试听（自动选最小的一档），点音质按钮下载对应档" });
        }
    }

    winrt::fire_and_forget OnlinePage::RunNet24Preview(winrt::w_music::OnlineTrackItem item)
    {
        auto lifetime = get_strong();
        if (item == nullptr)
        {
            co_return;
        }
        if (m_net24Previewing)
        {
            OnlineStatus().Text(L"上一条试听还在解析中，稍候…");
            co_return;
        }
        m_net24Previewing = true;
        OnlineStatus().Text(hstring{ L"解析试听：" + std::wstring{ item.Title().c_str() } + L" …" });

        wm::core::Net24Song const song = ToNet24Song(item);
        wm::core::Net24Resolve result;
        co_await winrt::resume_background();
        {
            // Preview streams the lightest tier the row can serve.
            result = m_net24->Preview(song);
        }
        co_await wm::app::ResumeOnUi();
        m_net24Previewing = false;

        if (!result.ok)
        {
            OnlineStatus().Text(hstring{ L"《" + std::wstring{ item.Title().c_str() } + L"》：" +
                                         wm::app::Utf16(result.reason) });
            co_return;
        }

        // Remember the size for the tier we streamed (its chip click below
        // will not need another quota slot).
        m_net24Sizes[wm::app::Utf16(song.Key(result.tier))] = result.download;

        auto track = winrt::make<winrt::w_music::implementation::TrackItem>();
        track.Id(hstring{ L"online:net24:" + std::wstring{ item.Id().c_str() } + L":preview" });
        track.Title(item.Title());
        track.Artist(item.Artist());
        track.Album(item.Album());
        track.FilePath(hstring{ wm::app::Utf16(result.download.url) });
        wm::app::Player().PlayTrack(track);

        std::wstring line = L"正在试听：" + std::wstring{ item.Title().c_str() } +
                            L"（" + wm::app::Utf16(result.download.quality);
        if (!result.download.sizeText.empty())
        {
            line += L" · " + wm::app::Utf16(result.download.sizeText);
        }
        line += L"）";
        OnlineStatus().Text(hstring{ line });
    }

    void OnlinePage::OnNet24ResultClick(IInspectable const&, Controls::ItemClickEventArgs const& args)
    {
        if (auto item = args.ClickedItem().try_as<winrt::w_music::OnlineTrackItem>())
        {
            RunNet24Preview(item);
        }
    }

    void OnlinePage::OnQualityChipClick(IInspectable const& sender, RoutedEventArgs const&)
    {
        auto chip = sender.try_as<Button>();
        if (chip == nullptr)
        {
            return;
        }
        const std::wstring key = winrt::unbox_value_or<hstring>(chip.Tag(), hstring{}).c_str();
        const std::size_t separator = key.rfind(L':');
        if (separator == std::wstring::npos)
        {
            return;
        }
        auto const row = m_net24ByChipKey.find(key);
        if (row == m_net24ByChipKey.end())
        {
            return;
        }
        ResolveNet24Tier(row->second, QualityOfTypeLetter(key.substr(separator + 1)), chip);
    }

    winrt::fire_and_forget OnlinePage::ResolveNet24Tier(winrt::w_music::OnlineTrackItem const& row,
                                                        wm::core::Net24Quality quality,
                                                        Button chip)
    {
        auto lifetime = get_strong();
        wm::core::Net24Song const song = ToNet24Song(row);
        const std::wstring key = wm::app::Utf16(song.Key(quality));
        if (m_net24Resolving || m_net24Downloading.contains(key) || m_net24Done.contains(key))
        {
            co_return;
        }
        m_net24Resolving = true;
        winrt::Windows::Foundation::IInspectable const originalContent = chip.Content();
        chip.IsEnabled(false);
        chip.Content(winrt::box_value(hstring{ QualityNameOf(quality) + L" 查询中…" }));

        wm::core::Net24Resolve result;
        co_await winrt::resume_background();
        {
            // Each detail lookup costs one of the site's daily quota slots;
            // successful lookups are cached inside the source (and remembered
            // below), so repeat asks are free.
            result = m_net24->Resolve(song, quality);
        }
        co_await wm::app::ResumeOnUi();
        m_net24Resolving = false;
        chip.Content(originalContent);
        chip.IsEnabled(true);

        if (!result.ok)
        {
            OnlineStatus().Text(hstring{ L"《" + std::wstring{ row.Title().c_str() } + L"》" +
                                         wm::app::Utf16(Net24QualityShort(quality)) + L"：" +
                                         wm::app::Utf16(result.reason) });
            co_return;
        }
        m_net24Sizes[key] = result.download;

        // ---- confirm dialog (格式 / 大小 / 来源), like a-music's AlertDialog ----
        StackPanel content;
        content.Spacing(4);
        auto addLine = [&content](hstring const& text, double size, double opacity) {
            TextBlock line;
            line.Text(text);
            line.FontSize(size);
            line.Opacity(opacity);
            line.TextWrapping(TextWrapping::Wrap);
            content.Children().Append(line);
        };
        addLine(row.Title(), 15, 1.0);
        addLine(row.Album().empty() ? row.Artist()
                                    : hstring{ row.Artist() + L" · " + row.Album() }, 12, 0.65);
        addLine(hstring{ L"格式  " } + wm::app::Utf16(result.download.ext), 13.5, 0.95);
        addLine(hstring{ L"大小  " } +
                    (result.download.sizeText.empty() ? std::wstring{ L"未知" }
                                                      : wm::app::Utf16(result.download.sizeText)) +
                    (result.download.sizeBytes > 0
                         ? L"（" + FmtBytes(static_cast<std::uint64_t>(result.download.sizeBytes)) + L"）"
                         : L""),
                13.5, 0.95);
        addLine(hstring{ L"来源  " } + wm::app::Utf16(
                    Net24OriginLabel(wm::core::Net24QualityOrigin(quality))), 12, 0.65);
        addLine(L"保存到 Downloads 文件夹，下完自动加入「我的音乐」。", 12, 0.6);

        ContentDialog dialog;
        dialog.Title(winrt::box_value(hstring{ L"下载 " + QualityNameOf(quality) }));
        dialog.Content(content);
        dialog.PrimaryButtonText(L"下载");
        dialog.CloseButtonText(L"取消");
        dialog.DefaultButton(ContentDialogButton::Primary);
        dialog.XamlRoot(XamlRoot());

        ContentDialogResult const choice = co_await dialog.ShowAsync();
        if (choice == ContentDialogResult::Primary)
        {
            RunNet24Download(row, quality);
        }
    }

    winrt::fire_and_forget OnlinePage::RunNet24Download(winrt::w_music::OnlineTrackItem const& row,
                                                        wm::core::Net24Quality quality)
    {
        auto lifetime = get_strong();
        const std::wstring key = wm::app::Utf16(ToNet24Song(row).Key(quality));
        if (m_net24Downloading.contains(key) || m_net24Done.contains(key))
        {
            co_return;
        }
        auto const resolved = m_net24Sizes.find(key);
        if (resolved == m_net24Sizes.end())
        {
            co_return;
        }
        wm::core::Net24Download const download = resolved->second;
        m_net24Downloading.insert(key);

        OnlineStatus().Text(hstring{ L"开始下载：" + std::wstring{ row.Title().c_str() } + L" · " +
                                     wm::app::Utf16(download.quality) + L" " +
                                     wm::app::Utf16(download.sizeText) });

        std::filesystem::path const target =
            std::filesystem::path{ wm::app::Online().DownloadsDirectory() } /
            std::wstring{ wm::app::Utf16(download.fileName) };

        bool ok = false;
        co_await winrt::resume_background();
        {
            double lastFraction = -1.0;
            std::uint64_t const total = static_cast<std::uint64_t>(std::max<std::int64_t>(download.sizeBytes, 0));
            ok = wm::app::Online().DownloadUrlToFile(
                target.wstring(), download.url, m_net24->DownloadHeaders(), total,
                [this, &lastFraction, total](double fraction) {
                    if (fraction - lastFraction >= 0.01 || fraction >= 1.0)
                    {
                        lastFraction = fraction;
                        std::wstring line = L"下载中 " +
                            std::to_wstring(static_cast<int>(fraction * 100)) + L"%";
                        if (total > 0)
                        {
                            line += L" · " + FmtBytes(static_cast<std::uint64_t>(fraction * total)) +
                                    L" / " + FmtBytes(total);
                        }
                        PostStatus(line);
                    }
                });
        }
        co_await wm::app::ResumeOnUi();
        m_net24Downloading.erase(key);

        if (!ok)
        {
            OnlineStatus().Text(hstring{ L"下载失败：" + std::wstring{ row.Title().c_str() } });
            co_return;
        }

        winrt::w_music::TrackItem imported{ nullptr };
        try
        {
            auto file = co_await winrt::Windows::Storage::StorageFile::GetFileFromPathAsync(target.wstring());
            imported = co_await wm::app::Library().ImportFileAsync(file);
        }
        catch (...)
        {
            imported = nullptr;
        }
        co_await wm::app::ResumeOnUi();
        if (imported == nullptr)
        {
            OnlineStatus().Text(hstring{ L"下载完成但导入失败：" + std::wstring{ row.Title().c_str() } });
            co_return;
        }

        m_net24Done.insert(key);
        OnlineStatus().Text(hstring{ L"已下载：" + std::wstring{ row.Title().c_str() } + L"（" +
                                     wm::app::Utf16(download.quality) + L"，已加入本地曲库）" });
        RefreshLibraryStats();
    }

    // =====================================================================
    // == 自定义源（可插拔适配器）—— 原有实现保持不变                        ==
    // =====================================================================

    void OnlinePage::LoadAdapters()
    {
        auto& online = wm::app::Online();
        online.Reload();

        AdapterCombo().Items().Clear();
        for (hstring const& name : online.AdapterNames())
        {
            AdapterCombo().Items().Append(winrt::box_value(name));
        }
        AdapterCombo().SelectedIndex(online.Count() > 0 ? 0 : -1);
        SourceStatus().Text(online.StatusText());
        OnAdapterSelectionChanged();
    }

    void OnlinePage::ReloadSources()
    {
        LoadAdapters();
        OnlineStatus().Text(L"在线源已重新加载。");
    }

    void OnlinePage::OnAdapterSelectionChanged()
    {
        AdapterNote().Text(wm::app::Online().AdapterNoteAt(AdapterCombo().SelectedIndex()));
    }

    void OnlinePage::OnSearchClick(IInspectable const&, RoutedEventArgs const&)
    {
        const hstring query = QueryBox().Text();
        if (query.empty())
        {
            OnlineStatus().Text(L"先输入要搜索的关键词。");
            return;
        }
        const int32_t index = AdapterCombo().SelectedIndex();
        const hstring adapterId = wm::app::Online().AdapterIdAt(index);
        if (adapterId.empty())
        {
            OnlineStatus().Text(L"没有可用的在线源。把适配器 JSON 放到 %LOCALAPPDATA%\\w-music\\providers 后点「重新加载」。");
            return;
        }

        SearchRing().IsActive(true);
        OnlineStatus().Text(L"搜索中…");
        RunSearch(adapterId, query);
    }

    winrt::fire_and_forget OnlinePage::RunSearch(hstring adapterId, hstring query)
    {
        auto lifetime = get_strong();
        const std::uint32_t token = ++m_searchToken;

        auto results = co_await wm::app::Online().SearchAsync(adapterId, query);

        if (token != m_searchToken)
        {
            co_return;
        }
        SearchRing().IsActive(false);

        if (results == nullptr || results.Size() == 0)
        {
            ResultList().ItemsSource(nullptr);
            ResultHeader().Text(L"没有结果");
            OnlineStatus().Text(L"没有搜到内容：换个关键词，或在「重新加载」后确认在线源仍可用。");
            co_return;
        }

        ResultList().ItemsSource(results);
        ResultHeader().Text(hstring{ L"找到 " + std::to_wstring(results.Size()) + L" 条，选中后可批量下载；点行内按钮试听或下载。" });
        OnlineStatus().Text(L"");
    }

    void OnlinePage::UpdateSelectionUi()
    {
        const bool hasSelection = ResultList().SelectedItems().Size() > 0;
        DownloadSelectedButton().IsEnabled(hasSelection && !DownloadRing().IsActive());
        if (hasSelection)
        {
            OnlineStatus().Text(hstring{ L"已选中 " + std::to_wstring(ResultList().SelectedItems().Size()) +
                                         L" 项。" });
        }
    }

    void OnlinePage::OnResultAction(IInspectable const& sender, RoutedEventArgs const&)
    {
        auto button = sender.try_as<Button>();
        if (button == nullptr)
        {
            return;
        }
        auto item = button.DataContext().try_as<winrt::w_music::OnlineTrackItem>();
        if (item == nullptr)
        {
            return;
        }

        const hstring action = button.Tag().try_as<hstring>().value_or(hstring{});
        if (action == L"preview")
        {
            PreviewAsync(item);
        }
        else if (action == L"download")
        {
            auto single = winrt::single_threaded_vector<IInspectable>();
            single.Append(item);
            DownloadItemsAsync(single.GetView());
        }
    }

    winrt::fire_and_forget OnlinePage::PreviewAsync(winrt::w_music::OnlineTrackItem item)
    {
        auto lifetime = get_strong();
        if (m_previewBusy)
        {
            OnlineStatus().Text(L"上一条试听还在准备中，稍候…");
            co_return;
        }
        m_previewBusy = true;
        DownloadRing().IsActive(true);

        OnlineStatus().Text(L"解析试听地址…");
        auto resolved = co_await wm::app::Online().ResolveAsync(item);

        if (resolved == nullptr ||
            (resolved.PlayUrl().empty() && resolved.DownloadUrl().empty()))
        {
            OnlineStatus().Text(L"试听失败：这个源暂时取不到可播放的地址。");
            DownloadRing().IsActive(false);
            m_previewBusy = false;
            co_return;
        }

        bool started = false;
        if (wm::app::Online().CachesPreview(resolved.SourceId()))
        {
            OnlineStatus().Text(hstring{ L"缓冲试听：" + std::wstring{ resolved.Title().c_str() } + L" …" });
            auto localPath = co_await wm::app::Online().CachePreviewAsync(resolved);
            if (!localPath.empty())
            {
                auto track = winrt::make<winrt::w_music::implementation::TrackItem>();
                track.Id(hstring{ L"online:" + std::wstring{ resolved.SourceId().c_str() } + L":" +
                                  std::wstring{ resolved.Id().c_str() } + L":preview" });
                track.Title(resolved.Title());
                track.Artist(resolved.Artist());
                track.Album(resolved.Album());
                track.DurationMs(resolved.DurationMs());
                track.FilePath(localPath);
                wm::app::Player().PlayTrack(track);
                started = true;
            }
        }
        else
        {
            wm::app::Online().PlayOnline(resolved);
            started = true;
        }

        DownloadRing().IsActive(false);
        m_previewBusy = false;
        OnlineStatus().Text(started
            ? hstring{ L"正在试听：" + std::wstring{ resolved.Title().c_str() } + L"（喜欢就点行内的下载按钮入库）" }
            : L"试听失败：文件下载不下来，稍后再试。");
    }

    winrt::fire_and_forget OnlinePage::DownloadItemsAsync(
        IVectorView<IInspectable> const& items)
    {
        auto lifetime = get_strong();
        if (items.Size() == 0)
        {
            co_return;
        }

        DownloadRing().IsActive(true);
        DownloadSelectedButton().IsEnabled(false);

        std::uint32_t ok = 0;
        for (std::uint32_t i = 0; i < items.Size(); ++i)
        {
            auto item = items.GetAt(i).try_as<winrt::w_music::OnlineTrackItem>();
            if (item == nullptr)
            {
                continue;
            }
            OnlineStatus().Text(hstring{ L"下载中（" + std::to_wstring(i + 1) + L"/" +
                                         std::to_wstring(items.Size()) + L"）：" +
                                         std::wstring{ item.Title().c_str() } + L" …" });
            auto track = co_await wm::app::Online().DownloadAsync(item);
            // DownloadAsync failure paths finish on a background thread.
            co_await wm::app::ResumeOnUi();
            if (track != nullptr)
            {
                ++ok;
            }
            else
            {
                OnlineStatus().Text(hstring{ L"下载失败：" + std::wstring{ item.Title().c_str() } +
                                             L"（源没有给出可用地址，或文件无法入库）" });
            }
        }

        DownloadRing().IsActive(false);
        UpdateSelectionUi();
        RefreshLibraryStats();

        if (ok == items.Size())
        {
            OnlineStatus().Text(hstring{ L"完成：已把 " + std::to_wstring(ok) +
                                         L" 首加入本地曲库，到「我的音乐」里查看。" });
        }
        else
        {
            OnlineStatus().Text(hstring{ L"完成：成功 " + std::to_wstring(ok) + L" / 失败 " +
                                         std::to_wstring(items.Size() - ok) + L"。成功部分已加入本地曲库。" });
        }
    }
} // namespace winrt::w_music::implementation
