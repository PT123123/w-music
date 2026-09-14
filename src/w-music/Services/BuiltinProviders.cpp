#include "pch.h"

#include "Services/BuiltinProviders.h"

#include <wm/core/ProviderAdapter.h>

namespace wm::app
{
    namespace
    {
        // ccMixter (https://ccmixter.org) hosts community music under explicit
        // Creative Commons licenses. The JSON API is public and keyless; search
        // is tag based. File downloads answer only to browser-like requests,
        // hence the headers below and preview: "cache" (the media player cannot
        // send a Referer, so previews are fetched through the app first).
        char const* kCcmixter = R"JSON({
  "id": "ccmixter",
  "name": "ccMixter",
  "baseUrl": "https://ccmixter.org",
  "charset": "utf-8",
  "minIntervalMs": 400,
  "preview": "cache",
  "headers": {
    "User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36",
    "Referer": "https://ccmixter.org/"
  },
  "search": {
    "url": "/api/query?dataview=files&f=json&limit=30&tags={query_enc}",
    "method": "GET",
    "listPath": "$",
    "fields": [
      { "key": "id",     "source": "json", "pattern": "upload_id", "required": true },
      { "key": "title",  "source": "json", "pattern": "upload_name" },
      { "key": "artist", "source": "json", "pattern": "user_name" },
      { "key": "tags",   "source": "json", "pattern": "upload_extra.usertags" }
    ]
  },
  "detail": {
    "url": "/api/query?dataview=files&f=json&ids={id}",
    "method": "GET",
    "listPath": "$",
    "flattenPath": "files",
    "fields": [
      { "key": "id",       "source": "json", "pattern": "file_id" },
      { "key": "title",    "source": "json", "pattern": "upload_name" },
      { "key": "kind",     "source": "json", "pattern": "file_nicname" },
      { "key": "duration", "source": "json", "pattern": "file_format_info.ps", "required": true },
      { "key": "download", "source": "json", "pattern": "download_url" },
      { "key": "play",     "source": "json", "pattern": "download_url" }
    ]
  },
  "detailRequired": true,
  "note": "按标签搜索：jazz / piano / remix / acapella / guitar / electronic…（多个标签用逗号连接）。内容为创作者自愿共享的 CC 授权音乐。"
})JSON";
    } // namespace

    std::vector<wm::core::ProviderAdapter> BuiltinAdapters()
    {
        std::vector<wm::core::ProviderAdapter> out;
        char const* sources[] = { kCcmixter };
        for (char const* text : sources)
        {
            std::string error;
            if (auto adapter = wm::core::ParseAdapter(text, &error))
            {
                out.push_back(std::move(*adapter));
            }
        }
        return out;
    }
} // namespace wm::app
