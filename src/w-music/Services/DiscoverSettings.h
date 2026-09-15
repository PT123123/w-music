#pragma once

/// Persisted user settings for the online-discovery tab. The w-music replica
/// of the small slice a-music keeps in its SettingsRepository:
///
///   - which online source the tab last had selected ("qq" / "net24" /
///     "adapter"),
///   - the per-source search history (most recent first, max 12),
///   - the machine-local base URL of the lossless source (a-music injects it
///     at build time through local.properties; here it lives in settings.json
///     because the desktop app has no build-time injection).
///
/// Stored as one JSON document at AppPaths::SettingsFilePath(). Plain C++ --
/// the page reads and writes it directly.

#include "pch.h"

#include <string>
#include <vector>

namespace wm::app
{
    class DiscoverSettings
    {
    public:
        DiscoverSettings();

        /// Source ids used by the online page.
        static wchar_t const* SourceQq() noexcept { return L"qq"; }
        static wchar_t const* SourceNet24() noexcept { return L"net24"; }
        static wchar_t const* SourceAdapter() noexcept { return L"adapter"; }

        std::wstring DiscoverSource() const noexcept { return m_discoverSource; }
        void DiscoverSource(std::wstring const& value);

        std::vector<std::wstring> const& SearchHistory() const noexcept { return m_searchHistory; }
        void AddSearchHistory(std::wstring const& word);
        void RemoveSearchHistory(std::wstring const& word);
        void ClearSearchHistory();

        std::wstring Net24BaseUrl() const noexcept { return m_net24BaseUrl; }
        void Net24BaseUrl(std::wstring const& value);

        /// 界面主题 id（qq / ocean / sunset / galaxy / sakura）。这个文件在实践中
        /// 是应用级的 settings.json，界面偏好也塞在这里，省得再开一个文件。
        /// 未知 / 空值一律回落到 qq —— 由 MainWindow::ApplyTheme 负责解析。
        std::wstring UiTheme() const noexcept { return m_uiTheme; }
        void UiTheme(std::wstring const& value);

        /// QQ 音乐扫码登录态（持久化，供播放/歌词复用）。
        std::wstring QqSessionCookie() const noexcept { return m_qqSessionCookie; }
        std::wstring QqUin() const noexcept { return m_qqUin; }
        bool QqLoggedIn() const noexcept { return !m_qqSessionCookie.empty(); }
        void SetQqSession(std::wstring const& cookie, std::wstring const& uin);
        void ClearQqSession();

        static constexpr std::size_t MaxHistory = 12;

    private:
        void Load();
        void Save() const;

        std::wstring m_discoverSource{ SourceQq() };
        std::vector<std::wstring> m_searchHistory;
        std::wstring m_net24BaseUrl;
        std::wstring m_uiTheme{ L"qq" };
        std::wstring m_qqSessionCookie;
        std::wstring m_qqUin;
    };

    DiscoverSettings& Settings();
} // namespace wm::app
