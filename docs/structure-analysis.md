# 结构分析（结构卡片）与本地推荐引擎

记录「正在播放」页结构卡片的数据从哪来、怎么生成、各状态文案的含义，
以及引擎（music-recommend）的启动 / 预热机制和性能特征。
最后更新：2026-10-08。

## 总览

- **数据源**：外部 Python 引擎 [music-recommend](https://github.com/PT123123/music-recommend)。
  它维护自己的 SQLite 库（按文件的 (size, mtime, analysis_version) 做增量），
  分析产物包括全曲响度曲线、段落切分、节奏 / 调性 / 和声 / 人声 / 配器标量。
  w-music 只通过 HTTP 调用它，**分析数据不落在 w-music 这边**（会话内只在内存缓存）。
- **引擎目录解析**（`RecommendService::RepoDir`）：环境变量 `WMUSIC_RECOMMEND_DIR`
  → 设置项 `RecommendServerDir` → 默认 `桌面\music-recommend`。
- **格式限制**：引擎只收 `.mp3 / .wav`。flac 等留在 w-music 播放，不进引擎。
  w-music 侧有同样的扩展名过滤（`EngineSupportedAudio`），逐首分析不会拿 flac 去碰引擎。

## 结构卡片的数据流

```
切歌 OnPlayerPropertyChanged
  → SyncTimeline(filePath)            // 换曲备忘 m_timelinePath，同曲不重取
    → PeekTimeline(filePath)          // 会话内存缓存，命中直接画
    → GET /v1/analysis?file_path=...  // 引擎只读库；404 = 这首还没分析过
    → GET /v1/tracks/{track_id}       // 同一首的标量列（BPM / 调性 / 人声占比…）
    → ParseTimeline → PaintTimeline   // 曲线 + 段落卡片 + 本曲分析卡 + 歌词段落标注
```

- 缓存 `m_timelines` 只存有内容的回答（`ok || analysis.ok`）；「没有数据」的
  回答每次重新问，所以引擎侧分析完成后切回来就能画。
- 点卡片跳段落、点曲线任意位置跳时间（`OnStructureTapped`）。

## 多曲线显示（analysis version 4）

`GET /v1/analysis` 的回答带 `curves` 数组：每条曲线自带 `id / unit / meaning /
scope / time_seconds / values / valid / source / method_version`。`ParseTimeline`
逐点校验（有限值、时间不倒退），坏点标 `valid=false` 画图时跳过（**不补零**——
零可能是真实测量结果），缺失的曲线根本不进列表。

可切换的曲线（结构卡片曲线上方小字）：

| id | 界面名 | 单位 | 含义 |
|---|---|---|---|
| `level_db` | 电平(dB) | dBFS | 短时 RMS 电平，不是 LUFS |
| `level_norm` | 电平(相对) | 0..1 | 全曲内归一化电平 |
| `onset_activity` | 起音活动 | activity_proxy | onset 包络积分代理量（旧约定包络），不是起音次数 |
| `onset_rate_hz` | 起音率 | events/s | 实际检测到的起音事件数 / 桶时长 |
| `low_band_ratio` | 低频比例 | ratio 0..1 | 20-250 Hz 功率占比 |
| `spectral_brightness` | 亮度 | Hz | 功率加权频率质心 |
| `spectral_flux` | 谱变化 | normalized 0..1 | 帧间正变化占比，只说明音色在变 |

- 每条曲线按**自身 min-max** 显示（各曲线单位不同，不能共用一把无单位的 0-1
  尺子）；真实单位显示在 chips 下面一行。
- 旧引擎回答（version 3 及更早）没有 `curves` 块：`ParseTimeline` 从 legacy
  dynamics 字段合成同一套曲线，老数据也能切换。
- 事件除了电平驱动的 build/peak/drop/release，还有与电平无关的
  **axis-rise / axis-fall / axis-drop**（引擎 `detect_axis_changes` 按各维度
  曲线独立检测：起音率、起音活动、低频比例、亮度、谱流）。电平平稳的母带歌
  照样能报"节奏活动上升 / 音色变亮 / 某维度骤降"。哪个维度在事件的 evidence
  里（tooltip 可见）。
- 引擎侧语义修正（dynamics.py）：`onset_density` 标记 deprecated（activity 代理量）；
  新增规范名 `low_band_ratio` / `spectral_flux`，旧名 `low_band_energy` /
  `arrangement_change` 保留为兼容别名。`config.yaml` 的 `analysis.version`
  已升到 5，旧版本行会在下次分析时重算。

## P1 模块（analysis version 5）

`/v1/analysis` 回答新增五个模块（`dynamics_json` 升级为多模块载荷，
`repository.get_analysis` 兼容旧的单层格式）：

| 模块 | 内容 | 曲线 |
|---|---|---|
| `texture` | 频谱平坦度 / 带宽 / 高频占比 / 分频段谱流 | `spectral_flatness` `spectral_bandwidth` `hf_power_ratio` `flux_low/mid/high` |
| `spatial` | 原声道分支上的左右分析（`analysis.stereo` 开关，默认开） | `lr_balance_db` `lr_correlation` `side_energy_ratio` |
| `rhythm_timeline` | 窗口局部速度、拍点候选、速度变化区间 | `local_tempo`（BPM，自带时间网格） |
| `profiles` | 12 种多标签动态轮廓（逐段增加 / 交替对比 / 抽空再恢复 / 结尾骤停…），每条带维度、区间、分数、证据 | — |
| `description` | 确定性模板中文描述（无语言模型，逐句可追溯） | — |

诚实性约定（全部写在各模块 meta.notes 里）：

- 平坦度高 ≠ 用了白噪声合成器；带宽是频率维度指标，与立体声宽度无关；
- 空间模块只在原声道解码成功时 ready：单声道 / 多声道（无下混策略）/
  解码失败都如实报 `unavailable:<reason>`，无效桶是 `None` 不是 0；
  side 占比必须与相关度一起读（反相会虚高）；
- 起音变密 ≠ 加速（局部速度按窗口自相关）；半速/倍速保留候选不强行定论；
  拍号一律 unknown；
- 轮廓是多标签组合不是互斥曲风；恒定的轴逐条声明"全曲平稳"，不沉默也不外推；
  结尾骤停要求起音与电平同时收掉，单轴证据降分并注明无法排除文件截断；
- 描述层不把"明亮、宽、密集、响"合并成"更有力量"，标签全 unknown 时只用
  A/B 字母，不发明"副歌"。

## P2 深度分析适配器（默认关闭）

`models/` 下是统一适配器框架（`DeepModelAdapter`：`available()` 只查
导入/权重，不隐式下载；`analyze()` 不可用就抛 `NotAvailable`，基础结果保留）：

- `structure_labeler.py`：All-In-One 结构功能标签适配器，公开标签集之外的
  一律 unsupported；A/B/C 重复分组保持独立事实层；
- `source_separator.py`：Demucs 声源分离适配器，人声活动/主旋律音域等
  只在有可信人声 stem 后才输出。

配置 `analysis.deep.enabled: false`（默认）：模块状态为 `not_requested`，
全库分析不会下载或运行大模型；开启后也只对选中的歌曲运行。模块状态
统一在回答的 `modules` 字典里（ready / unavailable:<reason> / not_requested /
not_installed）。

## 「生成分析」按钮（结构卡片标题行）

只分析**当前正在播的这一首**，不扫目录——结构视图只关心这一首：

1. `AnalyzeTrackAsync(filePath)`：
   - 引擎没在跑就拉起（`EnsureStartedAsync`，见下文预热）；
   - 先 `GET /v1/analysis` 做**跳过检查**：引擎里已有当前版本全曲曲线就直接成功，
     重复点击不会把已分析的歌再解码一遍；
   - 没有才 `POST /v1/tracks/index`（引擎 `index_one`：解码 → 提特征 → 结构分段）。
2. 成功后页面 `ForgetTimeline(filePath)` 清掉这首歌的会话缓存，重走 `SyncTimeline`，
   曲线立刻出来。
3. 失败返回错误文本，原样显示在卡片页脚。

单曲分析耗时通常几秒到十几秒（解码 + 特征提取占绝大部分）。

## 整库逐首分析（「个性推荐」页「分析曲库」）

`AnalyzeLibraryAsync(trackPaths, folders, progress)`：

1. 过滤出 mp3/wav，逐首：`GET /v1/analysis` 检查 → 已有最新分析则跳过，
   否则 `POST /v1/tracks/index`。**进度回调实时汇报**：
   「第 i / N 首《歌名》—— 引擎里已有最新分析，跳过」/
   「第 i / N 首《歌名》—— 正在分析：解码音频 → 提取特征（响度曲线、节奏、
   调性、和声、人声、配器）→ 结构分段」。
   **分批推进**：每 8 首为一批，批间歇 2 秒并把阶段放回 Ready，期间推荐流
   补货、结构卡片取曲线可以插进来用引擎——整库分析不再一口气占满引擎
   （`AnalyzeBatchSize` / `AnalyzeBatchPause`）。
2. 有新增时，结尾对每个库文件夹跑一次增量 `POST /v1/library/scan`：
   此刻所有文件都已分析过，这一步只核对签名不重新解码，作用是重建推荐侧的
   **embedding + FAISS**（结构卡片不依赖它，个性推荐页要用）。
3. 返回一行摘要（新增 / 跳过 / 失败计数 + 失败明细）。

进度回调从后台线程调用，UI 侧用 `DispatcherQueue.TryEnqueue` 转回 UI 线程。

## 引擎生命周期与预热（性能关键）

- **冷启动代价**：`run_server.py` 在 uvicorn 监听**之前**就完成 librosa 等
  重依赖导入，Python 进程从拉起到 `/v1/health` 应答要十几秒。这是目前
  「首次分析慢」的主因，单次点击无法加速，只能**提前预热**。
- **拉起方式**（`SpawnEngine`）：`.venv\Scripts\python.exe`（没有就 PATH 上的
  python 3.10–3.12）执行 `scripts/run_server.py`；`CREATE_SUSPENDED` +
  kill-on-close Job Object，w-music 退出引擎跟着死。
- **就绪判定**：`EnsureStartedAsync` 先复用已在跑的引擎（健康检查），
  否则拉起并每 0.5s 轮询，上限 45s；进程中途退出会报
  「进程已退出，详见引擎 logs\errors.log（也可能是端口被占用）」。
- **预热时机**（都是纯后台，UI 不等）：
  1. 主窗口出来后：设置项 `RecommendPrewarm` 开着就预热；
  2. 推荐流兜底：没用过个性推荐却听推荐流的人，`PlayerViewModel` 会 kick 一次；
  3. **结构卡片一出现**（`SyncTimeline`）：当前在播本地文件且引擎未就绪就 kick
     一次（`m_prewarmKicked` 防重入，`PrewarmAsync` 幂等）。
     用户点「生成分析」时引擎多半已经在跑或正在启动。

## 状态文案对照

| 现象 | 含义 |
|---|---|
| 「读取中…」 | 正在向引擎要这首歌的分析（有会话缓存就不出现） |
| 「本地引擎 · 全曲」 | 拿到当前版本的全曲曲线，正常展示 |
| 「本地引擎 · 没有数据」+「这首歌还没有结构分析数据。点上面「生成分析」…」 | 引擎在跑但库里没这首（404 `file_path not indexed` / `track not found`），**不是连接故障**，点按钮即可 |
| 「结构分析要用本地推荐引擎…」类 | 引擎没在跑且预热未开（老文案，已被上一条逐步替换） |
| 「读取结构分析时出了异常（引擎可能未启动）」 | HTTP 请求异常：引擎刚挂 / 端口不通，重启应用或点按钮兜底 |
| 「《歌名》分析失败：…」 | 单曲索引失败，看引擎 `logs\errors.log`（常见：文件损坏、解码失败） |
| 「正在启动本地引擎（首次要拉起 Python 进程…）」 | 按钮触发的冷启动等待（预热机制就是为了少见到这句） |

## 故障排查

- **引擎目录找不到**：设 `WMUSIC_RECOMMEND_DIR` 或在设置里指定 `RecommendServerDir`。
- **Python 版本**：引擎要求 3.10–3.12；没有 `.venv` 时走 PATH 上的 `python`。
- **端口占用 / 进程秒退**：看引擎 `logs\errors.log`；w-music 诊断行
  （`Diag`）里有 spawn / 轮询 / 请求耗时的记录。
- **曲线是旧的**：引擎 `analysis_version` 升级后旧行会带 `stale` 标记，
  界面照画但注明是旧版；重新逐首分析或整库分析会升级到当前版本。
