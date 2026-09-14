#include <wm/core/Json.h>
#include <wm/core/ProviderAdapter.h>
#include <wm/core/ProviderEngine.h>   // JsonText

namespace wm::core
{
    namespace
    {
        std::string ReadString(json::Value const& container, char const* key, std::string const& fallback = {})
        {
            json::Value const* v = container.Find(key);
            if (v == nullptr || !v->isString())
            {
                return fallback;
            }
            return v->asString();
        }

        bool ReadBool(json::Value const& container, char const* key, bool fallback)
        {
            json::Value const* v = container.Find(key);
            if (v == nullptr)
            {
                return fallback;
            }
            if (v->isBool())
            {
                return v->asBool();
            }
            return v->asNumber(fallback ? 1.0 : 0.0) != 0.0;
        }

        int ReadInt(json::Value const& container, char const* key, int fallback)
        {
            json::Value const* v = container.Find(key);
            if (v == nullptr || !v->isNumber())
            {
                return fallback;
            }
            return static_cast<int>(v->asInt(fallback));
        }

        std::map<std::string, std::string> ReadHeaders(json::Value const& container, char const* key)
        {
            std::map<std::string, std::string> out;
            json::Value const* v = container.Find(key);
            if (v == nullptr || !v->isObject())
            {
                return out;
            }
            for (auto const& [name, value] : v->asObject())
            {
                out[name] = value.isString() ? value.asString() : JsonText(value);
            }
            return out;
        }

        std::vector<ExtractRule> ReadFields(json::Value const& container)
        {
            std::vector<ExtractRule> out;
            json::Value const* v = container.Find("fields");
            if (v == nullptr || !v->isArray())
            {
                return out;
            }
            for (json::Value const& item : v->asArray())
            {
                if (!item.isObject())
                {
                    continue;
                }
                ExtractRule rule;
                rule.key = ReadString(item, "key");
                rule.source = ReadString(item, "source", "regex");
                rule.pattern = ReadString(item, "pattern");
                rule.value = ReadString(item, "value");
                rule.group = ReadInt(item, "group", 1);
                rule.required = ReadBool(item, "required", false);
                if (!rule.key.empty())
                {
                    out.push_back(std::move(rule));
                }
            }
            return out;
        }

        RequestStep ReadStep(json::Value const& root, char const* key, RequestStep const& fallback)
        {
            json::Value const* v = root.Find(key);
            if (v == nullptr || !v->isObject())
            {
                return fallback;
            }
            RequestStep step;
            step.enabled = true;
            step.url = ReadString(*v, "url");
            step.method = ReadString(*v, "method", "GET");
            step.body = ReadString(*v, "body");
            step.headers = ReadHeaders(*v, "headers");
            step.itemPattern = ReadString(*v, "itemPattern");
            step.listPath = ReadString(*v, "listPath");
            step.flattenPath = ReadString(*v, "flattenPath");
            step.fields = ReadFields(*v);
            return step;
        }
    } // namespace

    std::optional<ProviderAdapter> ParseAdapter(const std::string& jsonText, std::string* error)
    {
        std::string parseError;
        auto parsed = json::Parse(jsonText, &parseError);
        if (!parsed.has_value())
        {
            if (error != nullptr)
            {
                *error = "JSON: " + parseError;
            }
            return std::nullopt;
        }

        ProviderAdapter adapter;
        json::Value const& root = *parsed;
        adapter.id = ReadString(root, "id");
        adapter.name = ReadString(root, "name", adapter.id);
        adapter.baseUrl = ReadString(root, "baseUrl");
        adapter.headers = ReadHeaders(root, "headers");
        adapter.charset = ReadString(root, "charset", "utf-8");
        adapter.minIntervalMs = ReadInt(root, "minIntervalMs", 0);
        adapter.note = ReadString(root, "note");

        adapter.search = ReadStep(root, "search", adapter.search);
        adapter.detail = ReadStep(root, "detail", adapter.detail);
        adapter.lyric = ReadStep(root, "lyric", adapter.lyric);
        adapter.detailRequired = ReadBool(root, "detailRequired", false);
        adapter.preview = ReadString(root, "preview", "stream");

        const std::string problem = ValidateAdapter(adapter);
        if (!problem.empty())
        {
            if (error != nullptr)
            {
                *error = problem;
            }
            return std::nullopt;
        }
        return adapter;
    }

    std::string ValidateAdapter(ProviderAdapter const& adapter)
    {
        if (adapter.id.empty())
        {
            return "missing \"id\"";
        }
        if (adapter.baseUrl.empty())
        {
            return "missing \"baseUrl\"";
        }
        if (!adapter.search.enabled)
        {
            return "missing \"search\" step";
        }
        if (adapter.search.url.empty())
        {
            return "search step has no \"url\"";
        }
        if (adapter.search.itemPattern.empty() && adapter.search.listPath.empty() && adapter.search.fields.empty())
        {
            return "search step needs \"itemPattern\", \"listPath\" or \"fields\"";
        }
        for (ExtractRule const& rule : adapter.search.fields)
        {
            if (rule.source != "json" && rule.source != "regex" && rule.source != "static")
            {
                return "unknown extract source \"" + rule.source + "\" for field \"" + rule.key + "\"";
            }
        }
        return {};
    }
} // namespace wm::core
