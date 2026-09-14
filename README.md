# w-music

类 QQ 音乐的 Windows 桌面播放器：**WinUI 3 (C++/WinRT)** 界面 + **MediaPlayerElement** 播放 + **WASAPI loopback** 实时频谱。曲库本地优先，同时内置 **QQ 音乐在线源**（搜索 / 扫码登录 / 试听 / 歌词 / 下载）。

## 技术选型

| 层面 | 选择 | 说明 |
| --- | --- | --- |
| UI | WinUI 3 (Windows App SDK 2.3.1) + C++/WinRT | XAML、`x:Bind`、NavigationView |
| 播放 | `Windows.Media.Playback.MediaPlayer` 挂到 `MediaPlayerElement` | 解码交给 Media Foundation，支持 mp3/flac/m4a/wav… |
| 频谱 | WASAPI loopback 采集 + FFT | `IMMDeviceEnumerator` + `IAudioClient(AUDCLNT_STREAMFLAGS_LOOPBACK)` |
| 持久化 | 单个 JSON（core 层自带解析器） | `%LOCALAPPDATA%\w-music\library.json`，无外部依赖 |
| 数据/算法 | 平台无关 C++17（见 `core/`） | FFT、频谱、LRC 解析、曲库/歌单、播放队列、在线源引擎 |
| 在线源 | QQ 音乐官方 Web 接口直连（`QqSource`） | 搜索 / 歌词 / 扫码登录 / 试听 vkey 直链；会话 cookie 持久化到 `settings.json` |

## 目录结构

```
core/                     平台无关核心层（可用 g++ 直接编译并单测）
  include/wm/core/        Fft, SpectrumAnalyzer, LyricParser, Json, LibraryStore, PlayQueue,
                          OnlineSources（QqSource / QqLoginFlow）, ProviderEngine, ProviderAdapter
  src/  tests/            344 项断言（3 个测试程序），见下方"核心层测试"
src/w-music/              WinUI 3 应用
  App.* / MainWindow.*    应用入口 + 导航（发现 / 在线发现 / 我的音乐 / 正在播放）+ 底部播放条 + 系统托盘
  Models.*  Models/       TrackItem / PlaylistItem / LyricLineItem / OnlineTrackItem / QualityChipItem（XAML 可绑定）
  ViewModels.*  ViewModels/  PlayerViewModel / LibraryViewModel
  Services/               LibraryService（扫描/持久化/歌单）、OnlineProviderService（在线源，含 QQ 直连）、
                          BuiltinProviders（内置 CC 授权源）、DiscoverSettings（设置/QQ 登录态持久化）、
                          TrayIcon（Shell_NotifyIcon 托盘）、AppPaths、Services（单例）
  Audio/WasapiLoopback.*  WASAPI loopback 采集线程
  Controls/SpectrumView.* 直接操作 Rectangle 的频谱绘制（不走绑定，省开销）
  Views/                  DiscoverPage / OnlinePage / LibraryPage / NowPlayingPage
adapters/                 适配器模板与文档（真实适配器放外部目录，见下）
tools/gen_assets.py       生成 MSIX 占位图标（历史保留）
tools/gen_icon.ps1        生成 Assets\app.ico（窗口/托盘/任务栏图标）
```

## 构建与运行

需要 **Visual Studio 2022**（"使用 C++ 的桌面开发"）、**Windows 10 SDK 10.0.26100**，以及 **just**
（`just --version` 能跑就行；本机在 `C:\Users\<你>\Tools\just.exe`）。

```powershell
just                    # 列出全部 recipe
just build              # 全量：投影 → IDL → cppwinrt → build.ninja → ninja（不跑单测）
just fast               # 复用已生成的投影，只重编（改 .cpp 时的内循环）
just test               # 跑 core 单测
just run                # 全量构建 + 跑单测
just launch             # 启动 build\w-music.exe（exe 不存在时先构建）
just gen                # 强制重生成 C++/WinRT 投影后再构建（改过 .idl 时用）
just clean              # 删掉 build\（下次全量重生成，约 2 分钟）
just clean-soft         # 只删 obj / exe / 日志，保留上千个投影头文件
just tools              # 只打印探到的工具链路径
```

`justfile` 只是**最顶层入口**，每个 recipe 都把开关转发给 `build.ps1` → `tools\dev-build.ps1`，
后者是构建逻辑的唯一出处。两个 ps1 也都还能直接用：
`.\build.ps1 [-NoGen] [-NoTests] [-Clean] [-ListOnly]`。整条链路是**纯 PowerShell + Ninja**——
不用 MSBuild、不用 .bat、不用 vcvars、也不经 git-bash。

脚本自己用 `Microsoft.VisualStudio.DevShell.dll`（纯 PowerShell）进 VS 环境，
然后：生成 Windows SDK + WinUI(WindowsAppSDK 全量元数据) 的 C++/WinRT 投影 → midlrt 编译 `*.idl`
→ `cppwinrt -component` 生成应用运行时类 → 生成 `build\build.ninja` → 跑 ninja 编译 `core/`
（并链接执行单测）与 `src/w-music/` 的各编译单元。产物在 `build\`。

> **提速备注**：`Enter-VsDevShell` 在本机每次要 40–50s，所以脚本把它的 `PATH/INCLUDE/LIB/LIBPATH`
> 缓存到 `build\toolenv.json`（VS/SDK 升级后 `cl.exe` 探不到会自动重算，`just clean` 会一并删掉）。
> 另外生成的 shim / `.g.cpp` 替身只在内容变化时才落盘，否则 mtime 一变 ninja 就会重编 11 个单元。
> 实测空转 `just fast` 从 ~53s 降到 ~13s。

> 本机没有 VS 的 "C++ v143 UWP tools" 组件（即 C++ XAML markup compiler，`Microsoft.Windows.UI.Xaml.Cpp.targets`），
> 所以**不做 MSIX 打包**；XAML 由 `tools\xaml-markup.ps1` 直接驱动 XamlCompiler 生成
> `build\gen\component\w_music\*.xaml.g.h` 与 XBF，非打包 WinUI 3 应用可正常构建运行。
> 产物 `build\w-music.exe` 依赖已注册的 **WindowsAppRuntime 2.3.1 框架包**（引导程序找不到会弹框提示）。
> `just run` 跑的是 core 单测，启动界面用 `just launch`。

首次启动 → 发现页点 **添加音乐文件夹** → 选你的音乐目录 → 递归扫描并读取 `MusicProperties` 元数据建库。

## 已实现的功能

### 1. 曲库 / 发现页
- `FolderPicker` 选目录，权限 token 存进 `FutureAccessList`，下次启动自动重扫。
- 递归扫描，读取标题/艺术家/专辑/时长/码率，重扫不会清掉播放次数与喜爱状态。
- 发现页提供 **每日推荐**（随机）、**最近添加**、**常听**，以及本地曲库搜索（标题/艺术家/专辑）。

### 2. 在线发现（独立 tab）
- 导航栏单独一个「**在线发现**」标签：选源 → 关键词搜索 → 行内按钮**试听 / 下载**，
  支持多选批量下载；下载完成自动导入本地曲库（「我的音乐」直接可见），歌词条件允许时顺带存 `.lrc`。
- **内置源：QQ 音乐**（官方 Web 接口直连，`core/src/OnlineSources.cpp` 的 `QqSource`）。
  关键词搜索 → 行内**试听 / 下载**（登录后按账号权限取 vkey 直链，未登录仅免费音质）→ 下载完成自动导入曲库 → **歌词**随播放自动加载（QQ 歌词接口 `nobase64=1` 纯文本 LRC）。
  - **扫码登录**：QQ 区「扫码登录」按钮 → 弹出二维码对话框 → 轮询扫码/确认状态 → 成功后把会话 cookie 与 uin 持久化到 `settings.json`，重启自动恢复；登录后试听/下载继承当前账号的会员权益。点「退出登录」可随时清除。
  - 登录流程实现在 `core` 层（`QqLoginFlow`：取二维码 → `qrsig` → `hash33` 算 `ptqrtoken` → 轮询 `ptuiCB`），与 UI 解耦，便于单测。
- **内置源：ccMixter**（零配置可用）。ccMixter 是创作者自愿共享的 CC 授权社区音乐，公开 JSON 接口；
  因为其文件直链要求浏览器式 UA + Referer（播放器发不了），试听走"程序缓存到本地再播"（`preview: "cache"`），
  下载则带上适配器声明的请求头直取无损 FLAC。搜索按标签进行（jazz / piano / remix / acapella…）。
- **外部可插拔适配器**。把站点规则写成 JSON 放进 `%LOCALAPPDATA%\w-music\providers`
  （或 `WMUSIC_PROVIDER_DIR`，或 `<exe>\adapters`），点「重新加载」生效，**不用重新编译**；
  外部适配器与内置源同 id 时**覆盖内置**。字段说明见 `adapters/README.md`，模板 `adapters/template.json.example`，
  Jamendo（需免费 client_id）模板 `adapters/jamendo.json.example`。
- 引擎是通用的（`core/src/ProviderEngine.cpp`）：模板渲染 → HTTP → 按 `itemPattern`(HTML 正则)
  或 `listPath`(JSON 路径，`"$"` 为根数组) 切分记录，支持 `flattenPath`（嵌套文件数组展开）、
  字段提取（`regex` / `json` / `static`）→ 可选 detail 二次请求拿真实地址 → 可选 lyric。

> 版权提示：QQ 音乐是版权商业内容。内置源只调用其**公开 Web 接口**（搜索 / 歌词 / 扫码登录 / 试听直链），
> 不逆向私有加密协议；请用本人账号并遵守服务条款，下载内容仅限个人试听，勿再分发。

### 3. 列表管理
- 歌单：`新建 / 删除 / 重命名 / 加入曲目`，内置「我喜欢的音乐」「最近播放」（不可删除）。
- 喜爱：曲目行心形按钮、播放条、正在播放页均可切换，自动同步到内置喜爱歌单。
- 播放队列：`顺序播放 / 列表循环 / 随机播放 / 单曲循环`，随机模式保证一轮内不重复。
- 点任意曲目即以**当前列表**为队列播放（下一曲有上下文）。

### 4. 歌词
- LRC 解析支持：`[mm:ss.xx]`、`[mm:ss]`、`[mm:ss:xx]`、一行多时间标签、`[offset:±ms]`、`<mm:ss.xx>` 增强逐字标签（自动剥离）、UTF-8 / UTF-16(BOM) / 换行符混合。
- 自动查找歌词：音乐文件同目录同名 `.lrc` → `lyrics\` 子目录 → 同名子目录。
- 实时高亮 + 自动居中滚动；**点击任意行跳转到该时间点**；`±0.5s` 微调整体偏移、可重置。

## 频谱是怎么接的

`WasapiLoopback` 在后台线程抓系统混音（loopback）→ 转 float → `core::SpectrumAnalyzer`
（Hann 窗 + FFT + 对数分频 + dB 映射 + 快起慢落平滑）→ 结果放进缓冲，
UI 侧 66ms 的 `DispatcherQueueTimer` 取帧并让 `SpectrumView` 改 Rectangle 高度。
播放条与正在播放页可同时显示（多个 sink，页面卸载时自动注销）。

## 核心层测试

核心层不依赖 WinRT。`just build`（或 `.\build.ps1`）会用 MSVC（`/std:c++20`）把它编译成
`build\test_*.exe` 并直接跑，`just test` 可以只重跑已构建好的这些二进制；
也可以单独用 g++（Linux/WSL 下同样通过）：

```bash
g++ -std=c++17 -I core/include core/src/*.cpp core/tests/test_core.cpp -o test_core && ./test_core
# 94 passed, 0 failed
g++ -std=c++17 -I core/include core/src/*.cpp core/tests/test_provider.cpp -o test_provider && ./test_provider
# provider: 104 checks, 0 failed
g++ -std=c++17 -I core/include core/src/*.cpp core/tests/test_online_sources.cpp -o test_online && ./test_online
# online sources: 146 checks, 0 failed
```

覆盖：FFT（直流/单频峰值定位/非法尺寸）、频谱（高频能量分布、值域、静音衰减）、
LRC（多标签/offset/增强标签/UTF-16/序列化往返）、JSON（嵌套/Unicode/错误输入/round-trip）、
曲库（持久化、重扫不丢统计、歌单增删）、播放队列（四种模式语义）；
在线源引擎（模板渲染、URL 编码、JSON 路径、根数组("$")、嵌套展开、正则提取、HTML 实体、
HTML 与 JSON 两条完整管线、detail 二次解析、限流条数、失败路径）；
QQ 音乐（`QqSource::Lyric` 歌词解析、`QqLoginFlow::Hash33`、二维码上下文解析、`ptuiCB` 轮询状态解析、会话 cookie 拼接）。

## 已知限制 / 下一步

- 封面：目前是统一占位图标，未读取内嵌封面（可用 `StorageFile.GetThumbnailAsync(MusicView)` 填 `TrackItem.Cover`）。
- 下载：已接通（`OnlineProviderService::DownloadAsync`：解析地址 → 下载到 `LocalState\Downloads`
  → 自动导入曲库；适配器有 lyric step 时会顺带存同名 `.lrc`）。尚未做断点续传与并发队列；
  在线结果列表也还没有封面图。
- 数据规模到几万首时，JSON 全量读写会变慢，可替换成 SQLite（`LibraryStore` 已隔离在 core 层）。
- 歌词没有桌面歌词（悬浮窗），可用 `AppWindow` 的 `Presenter` 做 Always-On-Top 小窗。
