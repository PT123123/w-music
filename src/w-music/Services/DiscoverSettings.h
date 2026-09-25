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

#include <array>
#include <cstddef>
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

        /// ---- 均衡器 ----
        /// 十段参量 EQ（频点见 core::Equalizer::CenterFreqs），增益 / 前置放大器
        /// 单位 dB，core 层负责夹在 ±12。eqPreset 只记录 UI 上次选中的预设，
        /// 用户拖动滑条后置为 "custom"。
        static constexpr std::size_t EqBandCount = 10;

        bool EqEnabled() const noexcept { return m_eqEnabled; }
        std::wstring const& EqPreset() const noexcept { return m_eqPreset; }
        double EqPreampDb() const noexcept { return m_eqPreampDb; }
        std::array<double, EqBandCount> const& EqGainsDb() const noexcept { return m_eqGains; }
        void SetEqualizer(bool enabled, std::wstring const& preset, double preampDb,
                          std::array<double, EqBandCount> const& gainsDb);

        /// ---- 本地推荐引擎（music-recommend）----
        /// 引擎仓库目录：空 = 用默认位置（<桌面>\music-recommend），环境变量
        /// WMUSIC_RECOMMEND_DIR 又优先于这里。引擎以子进程方式运行其 FastAPI
        /// 服务（见 Services/RecommendService.h 与引擎仓库
        /// https://github.com/PT123123/music-recommend）。
        std::wstring RecommendServerDir() const noexcept { return m_recommendServerDir; }
        void RecommendServerDir(std::wstring const& value);

        /// 引擎 HTTP 端口，仅监听 127.0.0.1。
        static constexpr unsigned short RecommendPortDefault = 26128;
        unsigned short RecommendServerPort() const noexcept { return m_recommendServerPort; }
        void RecommendServerPort(unsigned short value);

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
        bool m_eqEnabled = false;
        std::wstring m_eqPreset{ L"flat" };
        double m_eqPreampDb = 0.0;
        std::array<double, EqBandCount> m_eqGains{};
        std::wstring m_recommendServerDir;
        unsigned short m_recommendServerPort = RecommendPortDefault;
    };

    DiscoverSettings& Settings();
} // namespace wm::app
