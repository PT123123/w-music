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

    DiscoverSettings& Settings()
    {
        static DiscoverSettings instance;
        return instance;
    }
} // namespace wm::app
