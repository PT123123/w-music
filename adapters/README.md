# 在线源适配器

「在线发现」tab 由两部分驱动：

1. **内置源**（随程序编译，零配置即可用）：目前只有 **ccMixter** —— 创作者自愿共享的
   CC 授权社区音乐，公开 JSON 接口。内置源只对接**内容本身允许分发**的站点。
2. **外部适配器**：站点地址、选择器、请求头全部是一份 JSON「适配器」，运行时从下面
   的目录读取：

1. `%WMUSIC_PROVIDER_DIR%`（多个目录用 `;` 分隔）
2. `%LOCALAPPDATA%\w-music\providers` ← 推荐
3. `<exe 目录>\adapters`

把 JSON 文件丢进任一目录，在「在线发现」点「重新加载」即可生效，**不用重新编译**。
**外部适配器与内置源 id 相同时会覆盖内置的**（内置源接口变了你可以自己修一份）。
仓库里的 `template.json.example` 是通用模板；`jamendo.json.example` 是需要免费
client_id 的 Jamendo 模板（后缀 `.example` 不会被加载）。

> 只对接你有权使用的内容：自己的后端、NAS、自建网关、明确授权的接口，或上面这类
> 开放/CC 授权源。本项目不内置、也不会帮你逆向第三方平台的私有加密协议。

## 文件结构

```jsonc
{
  "id": "my-source",              // 唯一 id，出现在下拉框里；与内置源相同则覆盖内置
  "name": "我的音乐源",            // 显示名
  "baseUrl": "https://host",      // 相对 url 的前缀
  "charset": "utf-8",             // "raw" 可关闭 HTML 实体解码
  "minIntervalMs": 500,           // 礼貌性间隔提示（调用方自行遵守）
  "preview": "stream",            // "cache" = 试听先由程序缓存到本地再播（站点拒绝播放器直连时用）
  "headers": { "User-Agent": "...", "Referer": "...", "Cookie": "..." },
  // ↑ 下载也走这组头；没写 User-Agent 时程序发 "w-music/1.0"

  "search": { ... },   // 必填：关键词 -> 曲目列表
  "detail": { ... },   // 可选：曲目 -> 播放/下载地址
  "lyric":  { ... },   // 可选：曲目 -> 歌词文本
  "detailRequired": false, // true 表示必须跑 detail 才能拿到下载地址
  "note": "搜索提示，会显示在搜索框下面"
}
```

## 一个 step 的写法

```jsonc
{
  "url": "/search?q={query_enc}",   // 模板变量见下
  "method": "GET",                  // 也支持 POST
  "body": "",                       // 同样是模板
  "headers": { "Referer": "{detail}" },

  // 二选一：把响应切成「一条条记录」
  "itemPattern": "<li class=\"song\">(.*?)</li>",  // HTML：整段匹配 = 一条
  "listPath": "data.list",                          // JSON：点号路径（支持 a.b[0].c）；"$" = 整个文档根

  // 可选：JSON 模式下，把每条记录里这个路径上的「内层数组」展开成多行记录
  // （字段先读内层元素、读不到再回退到外层记录）。适合"一张专辑 + 嵌套 files 数组"的站点。
  // "flattenPath": "files",

  "fields": [
    { "key": "id",    "source": "regex", "pattern": "data-id=\"(\\d+)\"", "group": 1, "required": true },
    { "key": "title", "source": "json",  "pattern": "name" },
    { "key": "album", "source": "static", "value": "未知专辑" }
  ]
}
```

- `itemPattern` / `listPath` **都不填**时，整个响应体当作一条记录（detail、lyric 常用）。
- JSON 字段值是数组时会用 ", " 拼接（比如 creator 是 `["A","B"]` → `A, B`）。
- `source`：`regex`（取捕获组，`group: 0` 表示整段）、`json`（点号路径）、`static`（原样取值）。
- `key` 决定落到哪里：`id` `title` `artist` `album` `duration` `cover` `play` `download` `detail`；
  其他名字进 `extra`，在结果行尾以 `key=value` 显示出来。
- `duration` 支持 `03:45` / `4:05` / `212` 三种写法，统一换算成秒。
- `cover` `play` `download` `detail` 会自动把相对地址补全成绝对地址（含 `//host/x`）。

### 模板变量

`{query}`（原样）`{query_enc}`（URL 编码）`{id}` `{title}` `{artist}` `{album}` `{detail}`，
以及上一步捕获到的 extra：`{$字段名}`。

## 排错

- 下拉框空 / 状态栏提示未加载 → 目录里没有 `*.json`，或 JSON 没通过校验（缺 `id`/`baseUrl`/`search`）。
- 有结果但下载失败 → 多数是 `download`/`play` 规则没匹配上；开「detail」step 并把 `detailRequired` 设 `true`。
- 试听秒失败但下载正常 → 站点拒绝播放器直连（没有 Referer/UA 就 403），给适配器加 `"preview": "cache"`。
- 站点改版 → 只改这份 JSON，不用动代码（内置源同理：写一份同 id 的外部适配器覆盖它）。

核心引擎 `core/src/ProviderEngine.cpp` 有 104 项离线单测（模板渲染、JSON 路径、正则提取、
HTML 实体、HTML/JSON 两种管线、根数组与嵌套展开、失败路径），在 WSL 里：

```
wsl -e bash -lc "cd /mnt/c/Users/<account>/Desktop/w-music && \
  g++ -std=c++17 -Wall -Wextra -I core/include core/src/*.cpp core/tests/test_provider.cpp -o /tmp/t && /tmp/t"
```
