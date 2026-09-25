# w-music

类 QQ 音乐的 Windows 桌面播放器：**WinUI 3 (C++/WinRT)** 界面 + **MediaPlayerElement** 播放 + **WASAPI loopback** 实时频谱。曲库本地优先，同时内置 **QQ 音乐在线源**（搜索 / 扫码登录 / 试听 / 歌词 / 下载），并接入姊妹项目 [music-recommend](https://github.com/PT123123/music-recommend) 提供**基于音频分析的本地个性化推荐**。

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
  App.* / MainWindow.*    应用入口 + 导航（发现 / 个性推荐 / 在线发现 / 我的音乐 / 正在播放）+ 底部播放条 + 系统托盘
  Models.*  Models/       TrackItem / PlaylistItem / LyricLineItem / OnlineTrackItem / QualityChipItem /
                          RecommendItem / CategoryItem（XAML 可绑定）
  ViewModels.*  ViewModels/  PlayerViewModel / LibraryViewModel / RecommendViewModel
  Services/               LibraryService（扫描/持久化/歌单）、OnlineProviderService（在线源，含 QQ 直连）、
                          RecommendService（本地推荐引擎进程管理 + HTTP 客户端）、
                          BuiltinProviders（内置 CC 授权源）、DiscoverSettings（设置/QQ 登录态持久化）、
                          TrayIcon（Shell_NotifyIcon 托盘）、AppPaths、Services（单例）
  Audio/WasapiLoopback.*  WASAPI loopback 采集线程
  Controls/SpectrumView.* 直接操作 Rectangle 的频谱绘制（不走绑定，省开销）
  Views/                  DiscoverPage / RecommendPage / OnlinePage / LibraryPage / NowPlayingPage
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
just run                # 启动 build\w-music.exe（只运行，不构建）
just launch             # 同 just run（别名）
just workshop-deploy    # Release 构建 → 部署到 C:\workshop\w-music-<版本> → 启动
just deploy-workshop    # 同 workshop-deploy（别名）
just gen                # 强制重生成 C++/WinRT 投影后再构建（改过 .idl 时用）
just clean              # 删掉 build\（下次全量重生成，约 2 分钟）
just clean-soft         # 只删 obj / exe / 日志，保留上千个投影头文件
just tools              # 只打印探到的工具链路径
```

`justfile` 只是**最顶层入口**，每个 recipe 都把开关转发给 `build.ps1` → `tools\dev-build.ps1`，
后者是构建逻辑的唯一出处。两个 ps1 也都还能直接用：
`.\build.ps1 [-NoGen] [-NoTests] [-Clean] [-Release] [-ListOnly]`。整条链路是**纯 PowerShell + Ninja**——
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
> `just run` 只启动界面（不构建），构建和单测分别用 `just build` / `just test`。

### Release 构建与工作台部署（`just workshop-deploy`）

```powershell
just workshop-deploy    # 版本号 +1 → Release 构建 → 部署到 C:\workshop\w-music-<版本> → 启动
```

- `just build` 是**未优化**的 Debug 配置（`cl.exe` 不给 `/O` 就是 `/Od`）；`just workshop-deploy` 走
  `-Release`（`/O2 /Oi /Gy /DNDEBUG`），也就是 `.\build.ps1 -Release`。两套配置共用 `build\` 与同一份
  obj 文件名（这里没有 MSBuild 的 Debug/Release 输出目录），所以**切换配置 = 整树重编**：ninja 靠命令行
  变化自己发现，脚本额外打一行 `config : Release (was Debug …)` 把这件事说出来。
- 版本号只有一处：仓库根的 **`version.txt`**。`just workshop-deploy` **先**把它 patch +1、同步进
  `Package.appxmanifest` / `app.manifest` 的身份版本，**再**构建——顺序不能换：`tools\dev-build.ps1`
  用它的值生成 `build\obj\version.rc`，把版本作为 VERSIONINFO 资源嵌进 exe。先构建后改号的话，
  `C:\workshop\w-music-<版本>` 里的二进制会报着上一个版本号，之后分不清哪个目录是哪个构建；
  部署收尾会把目录名和 `(Get-Item w-music.exe).VersionInfo` 对一遍。
- 目录里装的是**能跑起来所需的全部文件**（非打包 WinUI 3 应用按 exe 所在目录解析一切，没有包可读）：
  `w-music.exe`、`Microsoft.WindowsAppRuntime.Bootstrap.dll`（exe 按名字导入它，系统只搜 exe 目录与
  系统路径，不会去 NuGet 缓存里找）、`app.ico`（窗口/托盘图标按路径加载）、`w_music\*.xbf`
  （每个 `InitializeComponent()` 都走 `ms-appx:///w_music/<页>.xaml`，而 `ms-appx:///` 就是 exe 目录）。
  用户数据（`library.json` / `settings.json` / `providers`）仍在 `%LOCALAPPDATA%\w-music`，换构建不动它。
- 同一个版本再部署会先复制到临时目录，校验通过后再替换正式目录（免得上一版删掉的页面 `.xbf` 残留成幽灵页面）；
  若那个目录里的 `w-music.exe` 正在运行，脚本会直接拒绝并提示先从托盘退出，而不是删一半。

首次启动 → 发现页点 **添加音乐文件夹** → 选你的音乐目录 → 递归扫描并读取 `MusicProperties` 元数据建库。

## 已实现的功能

### 1. 曲库 / 发现页
- 「**添加音乐文件夹**」走 shell 的 `IFileOpenDialog`（`FOS_PICKFOLDERS`）拿**绝对路径**：系统 COM 类，
  非打包进程也能解析。不用 `Windows.Storage.Pickers.FolderPicker` + `FutureAccessList`——那两个绑在
  **包身份**上，本应用是非打包的，调用即 `0x80040154 没有注册类`。
- 因此曲库记录的是路径而不是授权 token：`scanFolders` 与 `library.json` 一起放在
  `%LOCALAPPDATA%\w-music`，**部署新版本只换 `C:\workshop\w-music-<版本>` 目录，曲库与歌单不受影响**。
- 递归扫描，读取标题/艺术家/专辑/时长/码率，重扫不会清掉播放次数与喜爱状态；扫描按 100 首一批入库并落盘，
  中途崩溃也能在下一次启动时被重扫接上。收尾（歌单计数/落盘）出错只记 `diag.log`，不会把已导入的歌报成"导入失败"。
- **容器存 WinRT 对象不用 `map[id] = item`**：下标运算会**默认构造**一个 `TrackItem`，而 C++/WinRT 给可激活
  类生成的默认构造走的是**类激活**（RoActivateInstance）——非打包进程没有类注册，于是 `RefreshTracks()` /
  `RefreshPlaylists()` 在放进第一行之后就被 `0x80040154 没有注册类` 打断：`library.json` 明明读到了 271 首，
  侧栏歌单却一直空白，状态条还报"导入失败：没有注册类"。改用 `insert_or_assign`（只赋值，不默认构造）；
  扫描入库那条路一直用 `emplace`，所以它从没坏过——这正是"歌过一会儿又自己冒出来"的原因。
  `diag.log` 的 `bound lists tracks=… playlists=…` 行用来看两条列表有没有真被填上（正常应为 tracks=<曲库数> playlists=2+N）。
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

### 5. 个性推荐（本地 MIR 引擎）
- 引擎是独立仓库 **[music-recommend](https://github.com/PT123123/music-recommend)**
  （`git@github.com:PT123123/music-recommend.git`）：对本地 MP3/WAV 做 MIR 特征提取
  （MFCC / 色度 / 节奏 / 和声 / 人声 / 结构）→ SQLite + FAISS → 歌曲相似 / 固定曲风 / 带时间衰减的动态 Feed，
  **全部来自音频分析，不是平台热榜或人工标签**。
- w-music 通过 `Services/RecommendService` 以**子进程**方式拉起引擎的 FastAPI 服务
  （`<引擎目录>\.venv\Scripts\python.exe scripts\run_server.py --port 26128`，仅监听 127.0.0.1），
  并挂到 kill-on-close 的 Job Object 上——w-music 退出引擎随之退出，端口上已有健康引擎时直接复用。
- 「**个性推荐**」页提供四类入口，全部走引擎同一套「库内分位」打分路径：
  - **动态 Feed**（**为你推荐** / **换一批**）：带时间衰减的口味画像；
  - **曲风分类**：引擎 `categories.yaml` 的 **22 条预设**（高能量 / 低刺激 / 强节奏 / 明亮音色 / 极致高音女声 /
    低沉人声 / 纯音乐 / 慢速抒情 / 说唱感…），chips 自动换行；
  - **本曲库里自动发现的类别**（`auto-*`）：引擎在分位空间上做 KMeans，k 由轮廓系数在 [4,12] 中选，
    每簇 ≥ max(4, 4%×库)，查询目标直接来自簇质心；chip 上标成员数，标题行写 `k / 轮廓系数`，
    「分析曲库」完成后自动重取（曲库换了类别跟着重标定）；
  - **按描述找**：一句中文（如「安静又明亮的纯音乐」「不要快节奏」）交给引擎的 142 词词表做最长匹配，
    否定会翻转目标分位。
  另有 **与我正在听的相似**（以当前播放曲目为种子）、**分析曲库**（把「发现音乐」里添加的文件夹交给引擎
  增量分析：新增 43s/首、重扫跳过不变文件）。
- 每行推荐带**匹配度**（引擎归一化分数）与**推荐理由**（真实计算的分组相似度）；点行即播（整列表为队列），
  **喜欢 / 不喜欢 / 播放**会通过 `/v1/feed/feedback` 回传引擎，逐步收紧口味画像。文件自带 tag 时另起一行
  显示「文件标签：曲风 · 语种」——那是文件自己的声明，不是音频分析出的结论。
- 列表下方固定挂一条**诚实说明**（引擎每次类别回答都随结果返回的元数据）：落点在库里覆盖多少首
  （`support` / `low_support`，低于门槛时写明「这是按目标凑出来的排名，不是这类歌有这么多」）、实际生效的
  硬过滤及其中哪些只是阈值化**估计**（人声 / 纯音乐判定实测不可靠）、被忽略或无数据可排的维度、写错了的
  过滤条件（只报告不生效），以及文本查询里**答不了的词与原因**（情绪词没有本地测量支撑、语种/曲风无 tag
  可比对时不做近似猜测，改走 `genre_proxies` / `vocal_proxies` 会注明是听感近似）。
- 引擎目录解析顺序：环境变量 `WMUSIC_RECOMMEND_DIR` → `settings.json` 的 `recommendServerDir` →
  `<桌面>\music-recommend`；端口可用 `recommendServerPort` 覆盖（默认 26128）。


## 频谱是怎么接的

`WasapiLoopback` 在后台线程抓系统混音（loopback）→ 转 float → `core::SpectrumAnalyzer`
（Hann 窗 + FFT + 对数分频 + dB 映射 + 快起慢落平滑）→ 结果放进缓冲，
UI 侧 66ms 的 `DispatcherQueueTimer` 取帧并让 `SpectrumView` 改 Rectangle 高度。
播放条与正在播放页可同时显示（多个 sink，页面卸载时自动注销）。

## 崩溃怎么查（diag.log + /MAP）

发布版**不带 PDB**，WER 只会给一个偏移量，所以 `App::App()` 里先装
`wm::app::InstallCrashLogger()`（`Services/AppPaths.cpp`）：

- **向量化异常处理器**（`AddVectoredExceptionHandler(1, …)`）先看到故障，写一行
  `AV code=… rip=+<rva> fault=… ts=<PE 时间戳> tid=… ui=…` 加一行 `AV stack +<rva> +<rva> …`
  （只记访问违例的前 4 次，栈上只挑落在 exe 代码段内的字，最多 24 个）；
  `SetUnhandledExceptionFilter` 再兜一层写 `CRASH` 同样的行。两者都只用 Win32 API 直接追加到
  `%LOCALAPPDATA%\w-music\diag.log`，不依赖 CRT。
- 链接规则带 `/MAP`，产物 `build\w-music.map` 与 exe 是同一次链接。解析：

```powershell
python tools\crash_symbols.py    # 读 diag.log 的 AV/CRASH 行，按 map 里最近的导出符号还原调用栈
```

  `ts=` 对不上就是拿错了 map——先确认 map 的时间戳和 AV 行的时间戳一致。

**C++/WinRT 协程参数规则**（0.1.21 修的那次闪退就是它）：协程函数**别用引用参数**。MSVC 把
`T const&` 按引用存进协程帧，而 IDL 方法进来时先过 generated produce shim，shim 里的接口是个临时对象；
第一次 `co_await` 之后再读它就是访问已释放内存。`fire_and_forget` 更直接——调用方（比如 XAML 事件处理函数）
在第一个挂起点就返回了，它的局部变量当场作废。凡是 await 之后还要用的参数一律**按值**
（`CategoryItem category` / `hstring text` / `std::string mid`），帧自己持有引用计数。
`SelectCategoryAsync`（点「本曲库里自动发现的类别」chip 闪退，`category.Note()` 在 await 后读）
就是这条规则的样本，同类写法在 `LoadOnlineLyric` / `ShowAddToPlaylistDialog` /
`DownloadItemsAsync` / `ResolveNet24Tier` / `RunNet24Download` / `ImportFileAsync` / `ScanPathAsync` 一并改了。

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
- 下载：已接通（`OnlineProviderService::DownloadAsync`：解析地址 → 下载到 `%LOCALAPPDATA%\w-music\Downloads`
  → 自动导入曲库；适配器有 lyric step 时会顺带存同名 `.lrc`）。尚未做断点续传与并发队列；
  在线结果列表也还没有封面图。
- 导入/扫描的收尾步骤（刷新歌单计数、落盘）单独记在 `diag.log` 的 `scan tail <步骤> hr=…` 行里：
  它们失败不会再把已经入库的歌报成"导入失败"，状态条会改说"（收尾步骤出错：…）"。
- 数据规模到几万首时，JSON 全量读写会变慢，可替换成 SQLite（`LibraryStore` 已隔离在 core 层）。
- 歌词没有桌面歌词（悬浮窗），可用 `AppWindow` 的 `Presenter` 做 Always-On-Top 小窗。
