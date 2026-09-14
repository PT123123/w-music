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

        static constexpr std::size_t MaxHistory = 12;

    private:
        void Load();
        void Save() const;

        std::wstring m_discoverSource{ SourceQq() };
        std::vector<std::wstring> m_searchHistory;
        std::wstring m_net24BaseUrl;
    };

    DiscoverSettings& Settings();
} // namespace wm::app
