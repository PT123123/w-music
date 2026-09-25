#include "pch.h"

#include "Services/DiscoverSettings.h"
#include "Services/AppPaths.h"

#include <wm/core/Json.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

namespace wm::app
{
    namespace
    {
        std::string ReadFile(std::filesystem::path const& path)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
            {
                return {};
            }
            return std::string{ std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>() };
        }

        void WriteFile(std::filesystem::path const& path, std::string const& text)
        {
            EnsureDataDirectory();
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (out)
            {
                out << text;
            }
        }
    } // namespace

    DiscoverSettings::DiscoverSettings()
    {
        Load();
    }

    void DiscoverSettings::Load()
    {
        std::string const text = ReadFile(SettingsFilePath());
        if (text.empty())
        {
            return;
        }
        auto root = wm::core::json::Parse(text);
        if (!root)
        {
            return;
        }

        if (auto const* source = root->Find("discoverSource"); source != nullptr && source->isString())
        {
            std::wstring const value = Utf16(source->asString());
            if (value == SourceQq() || value == SourceNet24() || value == SourceAdapter())
            {
                m_discoverSource = value;
            }
        }
        if (auto const* history = root->Find("searchHistory"); history != nullptr && history->isArray())
        {
            for (wm::core::json::Value const& entry : history->asArray())
            {
                if (!entry.isString())
                {
                    continue;
                }
                std::wstring const word = Utf16(entry.asString());
                if (!word.empty())
                {
                    m_searchHistory.push_back(word);
                }
            }
        }
        if (auto const* url = root->Find("net24BaseUrl"); url != nullptr && url->isString())
        {
            m_net24BaseUrl = Utf16(url->asString());
        }
        if (auto const* theme = root->Find("uiTheme"); theme != nullptr && theme->isString())
        {
            std::wstring const value = Utf16(theme->asString());
            if (!value.empty())
            {
                m_uiTheme = value;
            }
        }
        if (auto const* cookie = root->Find("qqSessionCookie"); cookie != nullptr && cookie->isString())
        {
            m_qqSessionCookie = Utf16(cookie->asString());
        }
        if (auto const* uin = root->Find("qqUin"); uin != nullptr && uin->isString())
        {
            m_qqUin = Utf16(uin->asString());
        }
        if (auto const* enabled = root->Find("eqEnabled"); enabled != nullptr && enabled->isBool())
        {
            m_eqEnabled = enabled->asBool();
        }
        if (auto const* preset = root->Find("eqPreset"); preset != nullptr && preset->isString())
        {
            std::wstring const value = Utf16(preset->asString());
            if (!value.empty())
            {
                m_eqPreset = value;
            }
        }
        if (auto const* preamp = root->Find("eqPreampDb"); preamp != nullptr && preamp->isNumber())
        {
            m_eqPreampDb = std::clamp(preamp->asNumber(), -12.0, 12.0);
        }
        if (auto const* gains = root->Find("eqGains"); gains != nullptr && gains->isArray())
        {
            std::size_t i = 0;
            for (wm::core::json::Value const& entry : gains->asArray())
            {
                if (i >= EqBandCount || !entry.isNumber())
                {
                    break;
                }
                m_eqGains[i++] = std::clamp(entry.asNumber(), -12.0, 12.0);
            }
        }
        if (auto const* dir = root->Find("recommendServerDir"); dir != nullptr && dir->isString())
        {
            m_recommendServerDir = Utf16(dir->asString());
        }
        if (auto const* port = root->Find("recommendServerPort"); port != nullptr && port->isNumber())
        {
            int const value = static_cast<int>(port->asInt());
            if (value >= 1024 && value <= 65535)
            {
                m_recommendServerPort = static_cast<unsigned short>(value);
            }
        }
    }

    void DiscoverSettings::Save() const
    {
        wm::core::json::Object root;
        root["discoverSource"] = wm::core::json::Value{ Utf8(m_discoverSource) };

        wm::core::json::Array history;
        for (std::wstring const& word : m_searchHistory)
        {
            history.push_back(wm::core::json::Value{ Utf8(word) });
        }
        root["searchHistory"] = wm::core::json::Value{ std::move(history) };
        root["net24BaseUrl"] = wm::core::json::Value{ Utf8(m_net24BaseUrl) };
        root["uiTheme"] = wm::core::json::Value{ Utf8(m_uiTheme) };
        root["qqSessionCookie"] = wm::core::json::Value{ Utf8(m_qqSessionCookie) };
        root["qqUin"] = wm::core::json::Value{ Utf8(m_qqUin) };
        root["eqEnabled"] = wm::core::json::Value{ m_eqEnabled };
        root["eqPreset"] = wm::core::json::Value{ Utf8(m_eqPreset) };
        root["eqPreampDb"] = wm::core::json::Value{ m_eqPreampDb };

        wm::core::json::Array gains;
        for (double gain : m_eqGains)
        {
            gains.push_back(wm::core::json::Value{ gain });
        }
        root["eqGains"] = wm::core::json::Value{ std::move(gains) };

        root["recommendServerDir"] = wm::core::json::Value{ Utf8(m_recommendServerDir) };
        root["recommendServerPort"] = wm::core::json::Value{ static_cast<std::int64_t>(m_recommendServerPort) };

        WriteFile(SettingsFilePath(), wm::core::json::Serialize(wm::core::json::Value{ std::move(root) }, true));
    }

    void DiscoverSettings::DiscoverSource(std::wstring const& value)
    {
        if (m_discoverSource == value)
        {
            return;
        }
        m_discoverSource = value;
        Save();
    }

    void DiscoverSettings::AddSearchHistory(std::wstring const& word)
    {
        std::wstring trimmed = word;
        while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
        if (trimmed.empty())
        {
            return;
        }

        auto lower = [](std::wstring const& text) {
            std::wstring out = text;
            std::transform(out.begin(), out.end(), out.begin(), towlower);
            return out;
        };

        std::vector<std::wstring> next{ trimmed };
        for (std::wstring const& existing : m_searchHistory)
        {
            if (lower(existing) != lower(trimmed))
            {
                next.push_back(existing);
            }
        }
        if (next.size() > MaxHistory)
        {
            next.resize(MaxHistory);
        }
        if (next == m_searchHistory)
        {
            return;
        }
        m_searchHistory = std::move(next);
        Save();
    }

    void DiscoverSettings::RemoveSearchHistory(std::wstring const& word)
    {
        auto const it = std::find(m_searchHistory.begin(), m_searchHistory.end(), word);
        if (it == m_searchHistory.end())
        {
            return;
        }
        m_searchHistory.erase(it);
        Save();
    }

    void DiscoverSettings::ClearSearchHistory()
    {
        if (m_searchHistory.empty())
        {
            return;
        }
        m_searchHistory.clear();
        Save();
    }

    void DiscoverSettings::Net24BaseUrl(std::wstring const& value)
    {
        if (m_net24BaseUrl == value)
        {
            return;
        }
        m_net24BaseUrl = value;
        Save();
    }

    void DiscoverSettings::UiTheme(std::wstring const& value)
    {
        if (value.empty() || m_uiTheme == value)
        {
            return;
        }
        m_uiTheme = value;
        Save();
    }

    void DiscoverSettings::SetQqSession(std::wstring const& cookie, std::wstring const& uin)
    {
        if (m_qqSessionCookie == cookie && m_qqUin == uin)
        {
            return;
        }
        m_qqSessionCookie = cookie;
        m_qqUin = uin;
        Save();
    }

    void DiscoverSettings::ClearQqSession()
    {
        if (m_qqSessionCookie.empty() && m_qqUin.empty())
        {
            return;
        }
        m_qqSessionCookie.clear();
        m_qqUin.clear();
        Save();
    }

    void DiscoverSettings::SetEqualizer(bool enabled, std::wstring const& preset, double preampDb,
                                        std::array<double, EqBandCount> const& gainsDb)
    {
        std::array<double, EqBandCount> clamped{};
        for (std::size_t i = 0; i < EqBandCount; ++i)
        {
            clamped[i] = std::clamp(gainsDb[i], -12.0, 12.0);
        }
        preampDb = std::clamp(preampDb, -12.0, 12.0);
        if (m_eqEnabled == enabled && m_eqPreset == preset && m_eqPreampDb == preampDb && m_eqGains == clamped)
        {
            return;
        }
        m_eqEnabled = enabled;
        m_eqPreset = preset;
        m_eqPreampDb = preampDb;
        m_eqGains = clamped;
        Save();
    }

    void DiscoverSettings::RecommendServerDir(std::wstring const& value)
    {
        if (m_recommendServerDir == value)
        {
            return;
        }
        m_recommendServerDir = value;
        Save();
    }

    void DiscoverSettings::RecommendServerPort(unsigned short value)
    {
        if (m_recommendServerPort == value)
        {
            return;
        }
        m_recommendServerPort = value;
        Save();
    }

    DiscoverSettings& Settings()
    {
        static DiscoverSettings instance;
        return instance;
    }
} // namespace wm::app
