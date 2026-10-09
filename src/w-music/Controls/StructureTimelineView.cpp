#include "pch.h"

#include "Controls/StructureTimelineView.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Shapes;
using namespace Windows::UI;

namespace wm::app
{
    namespace
    {
        /// Slots actually drawn. The engine hands back one bucket per half
        /// second, so a long track has hundreds of points for a canvas a few
        /// hundred pixels wide; the extras are averaged into these slots while
        /// the time axis keeps its exact positions.
        constexpr int kBarSlots = 96;

        /// configs/analysis.yaml caps a track at 12 sections; a few spare
        /// shapes means a raised cap never silently drops a band.
        constexpr int kShapePool = 14;

        struct Rgb
        {
            std::uint8_t r, g, b;
        };

        /// Muted hues, one per repeat group. Same group -> same colour: that is
        /// the part of the answer that needs no guess, while a section *name* is
        /// only an estimate and stays on the cards.
        constexpr Rgb kGroupColors[] = {
            { 0x4C, 0x8D, 0xFF }, { 0x31, 0xC2, 0x7C }, { 0xF5, 0x9F, 0x3B },
            { 0xB4, 0x62, 0xE0 }, { 0x17, 0xB2, 0xC7 }, { 0xE8, 0x5D, 0x75 },
        };

        Color GroupColor(std::wstring const& group, std::uint8_t alpha)
        {
            if (group.empty())
            {
                return ColorHelper::FromArgb(alpha, 0x9A, 0x9A, 0x9A);
            }
            auto const first = static_cast<std::uint8_t>(group[0] & 0xFFu);
            auto const letter = (first >= L'a' && first <= L'z') ? static_cast<char>(first - 32)
                                                                 : static_cast<char>(first);
            if (letter < L'A' || letter > L'Z')
            {
                return ColorHelper::FromArgb(alpha, 0x9A, 0x9A, 0x9A);
            }
            auto const& c = kGroupColors[(letter - L'A') % (sizeof(kGroupColors) / sizeof(Rgb))];
            return ColorHelper::FromArgb(alpha, c.r, c.g, c.b);
        }

        /// 读一支主题画刷的当前颜色（同 SpectrumView：取的是 App.xaml 里那批
        /// 共享实例，所以切主题后重画一次就能跟上）。
        Color BrushColor(wchar_t const* key, Color fallback)
        {
            try
            {
                auto value = Application::Current().Resources().Lookup(box_value(hstring{ key }));
                if (auto brush = value.try_as<SolidColorBrush>())
                {
                    return brush.Color();
                }
            }
            catch (...)
            {
            }
            return fallback;
        }

        std::uint8_t LerpChannel(std::uint8_t from, std::uint8_t to, double t)
        {
            const double value = static_cast<double>(from) + (static_cast<double>(to) - static_cast<double>(from)) * t;
            return static_cast<std::uint8_t>(value + 0.5);
        }

        double TimeToX(double seconds, double duration, double width)
        {
            if (duration <= 0.0 || width <= 0.0)
            {
                return 0.0;
            }
            return std::clamp(seconds / duration, 0.0, 1.0) * width;
        }

        // windows.h declares a global Rectangle() GDI function, so the
        // unqualified name is ambiguous (C2872).
        void Hide(winrt::Microsoft::UI::Xaml::Shapes::Rectangle const& rect)
        {
            rect.Width(0.0);
            rect.Visibility(Visibility::Collapsed);
        }
    } // namespace

    /// 页面（歌词行左侧色条）也用它：颜色和这里的时间轴色块必须同源，
    /// 否则「同色就是同一段落」这句话在两个地方会有两种说法。
    winrt::Windows::UI::Color StructureGroupColor(std::wstring const& group, std::uint8_t alpha)
    {
        return GroupColor(group, alpha);
    }

    void StructureTimelineView::Attach(Canvas const& canvas)
    {
        Detach();
        if (canvas == nullptr)
        {
            return;
        }

        m_canvas = canvas;
        canvas.Children().Clear();

        // 叠放顺序 = 加入顺序：段落色块在最底下，响度柱压上去，边界线和播放头最后。
        for (int i = 0; i < kShapePool; ++i)
        {
            winrt::Microsoft::UI::Xaml::Shapes::Rectangle band;
            band.RadiusX(3.0);
            band.RadiusY(3.0);
            m_bands.push_back(band);
            canvas.Children().Append(band);
        }

        const Color accent = BrushColor(L"WmAccentBrush", ColorHelper::FromArgb(0xFF, 0x31, 0xC2, 0x7C));
        const Color accentAlt = BrushColor(L"WmAccentAltBrush", ColorHelper::FromArgb(0xFF, 0x2D, 0xD4, 0xBF));
        m_paintedAccent = accent;
        m_paintedAccentAlt = accentAlt;
        for (int i = 0; i < kBarSlots; ++i)
        {
            const double t = static_cast<double>(i) / static_cast<double>(kBarSlots - 1);
            winrt::Microsoft::UI::Xaml::Shapes::Rectangle bar;
            bar.Fill(SolidColorBrush{ ColorHelper::FromArgb(0xE6,
                                                            LerpChannel(accent.R, accentAlt.R, t),
                                                            LerpChannel(accent.G, accentAlt.G, t),
                                                            LerpChannel(accent.B, accentAlt.B, t)) });
            m_bars.push_back(bar);
            canvas.Children().Append(bar);
        }

        for (int i = 0; i < kShapePool; ++i)
        {
            winrt::Microsoft::UI::Xaml::Shapes::Rectangle tick;
            m_ticks.push_back(tick);
            canvas.Children().Append(tick);
        }

        m_playhead = winrt::Microsoft::UI::Xaml::Shapes::Rectangle{};
        m_playhead.Fill(SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0xF7, 0xF9, 0xFC) });
        canvas.Children().Append(m_playhead);

        HideAll();

        // 只有尺寸真的变了才整体重排；播放头每 tick 的移动走 SetPosition 的快路。
        m_sizeToken = canvas.SizeChanged([this](winrt::Windows::Foundation::IInspectable const&,
                                                        winrt::Microsoft::UI::Xaml::SizeChangedEventArgs const&)
        {
            Relayout();
        });
    }

    void StructureTimelineView::Detach()
    {
        if (m_canvas != nullptr)
        {
            m_canvas.SizeChanged(m_sizeToken);
            m_canvas.Children().Clear();
        }
        m_sizeToken = {};
        m_bars.clear();
        m_bands.clear();
        m_ticks.clear();
        m_playhead = nullptr;
        m_canvas = nullptr;
        m_curves.clear();
        m_active = 0;
        m_segments.clear();
        m_width = 0.0;
        m_height = 0.0;
    }

    void StructureTimelineView::SetData(double duration, std::vector<double> const& times,
                                        std::vector<double> const& curve,
                                        std::vector<Segment> const& segments)
    {
        // 单曲线便利入口：0..1 响度本来就是归一化好的，逐点有效。
        Curve level;
        level.id = L"level";
        level.times = times;
        level.values = curve;
        level.valid.assign(curve.size(), true);
        SetCurves(duration, { std::move(level) }, segments);
    }

    void StructureTimelineView::SetCurves(double duration, std::vector<Curve> const& curves,
                                          std::vector<Segment> const& segments)
    {
        m_duration = std::max(0.0, duration);
        m_curves = curves;
        m_active = m_curves.empty() ? 0 : std::min(m_active, m_curves.size() - 1);
        m_segments = segments;
        Relayout();
    }

    void StructureTimelineView::SetActiveCurve(std::size_t index)
    {
        if (index >= m_curves.size() || index == m_active)
        {
            return;
        }
        m_active = index;
        Relayout();
    }

    void StructureTimelineView::SetPosition(double seconds)
    {
        m_position = std::max(0.0, seconds);
        if (m_canvas == nullptr || m_playhead == nullptr)
        {
            return;
        }
        const double width = m_canvas.ActualWidth();
        const double height = m_canvas.ActualHeight();
        if (width <= 0.0 || height <= 0.0)
        {
            return;
        }
        if (width != m_width || height != m_height)
        {
            Relayout();
            return;
        }
        if (m_duration <= 0.0 || m_active >= m_curves.size() || m_curves[m_active].values.empty())
        {
            m_playhead.Visibility(Visibility::Collapsed);
            return;
        }
        m_playhead.Width(2.0);
        m_playhead.Height(height);
        Canvas::SetTop(m_playhead, 0.0);
        Canvas::SetLeft(m_playhead, std::max(0.0, TimeToX(m_position, m_duration, width) - 1.0));
        m_playhead.Visibility(Visibility::Visible);
    }

    double StructureTimelineView::TimeAt(double x) const
    {
        if (m_duration <= 0.0 || m_width <= 0.0)
        {
            return -1.0;
        }
        return std::clamp(x / m_width, 0.0, 1.0) * m_duration;
    }

    void StructureTimelineView::HideAll()
    {
        for (auto const& bar : m_bars)
        {
            Hide(bar);
        }
        for (auto const& band : m_bands)
        {
            Hide(band);
        }
        for (auto const& tick : m_ticks)
        {
            Hide(tick);
        }
        if (m_playhead != nullptr)
        {
            Hide(m_playhead);
        }
    }

    void StructureTimelineView::Relayout()
    {
        if (m_canvas == nullptr)
        {
            return;
        }
        const double width = m_canvas.ActualWidth();
        const double height = m_canvas.ActualHeight();
        if (width <= 0.0 || height <= 0.0)
        {
            return;
        }
        m_width = width;
        m_height = height;

        // 主题可能在页面停留期间换掉：颜色读回来，变了就重画一遍柱子。
        const Color accent = BrushColor(L"WmAccentBrush", ColorHelper::FromArgb(0xFF, 0x31, 0xC2, 0x7C));
        const Color accentAlt = BrushColor(L"WmAccentAltBrush", ColorHelper::FromArgb(0xFF, 0x2D, 0xD4, 0xBF));
        if (accent != m_paintedAccent || accentAlt != m_paintedAccentAlt)
        {
            m_paintedAccent = accent;
            m_paintedAccentAlt = accentAlt;
            for (std::size_t i = 0; i < m_bars.size(); ++i)
            {
                const double t = static_cast<double>(i) / static_cast<double>(m_bars.size() - 1);
                m_bars[i].Fill(SolidColorBrush{ ColorHelper::FromArgb(0xE6,
                                                                      LerpChannel(accent.R, accentAlt.R, t),
                                                                      LerpChannel(accent.G, accentAlt.G, t),
                                                                      LerpChannel(accent.B, accentAlt.B, t)) });
            }
        }

        HideAll();
        if (m_active >= m_curves.size() || m_duration <= 0.0)
        {
            return;
        }
        auto const& curve = m_curves[m_active];
        auto const& vals = curve.values;
        std::size_t const n = vals.size();
        if (n == 0 || curve.times.size() != n)
        {
            return;
        }

        // 显示归一化按本曲线自身的 min-max：各曲线单位不同（dBFS / 次/秒 /
        // 占比 / Hz），不能画成同一把无单位的 0-1 尺子；真实单位由页面文字说明。
        double lo = 0.0;
        double hi = 0.0;
        bool any = false;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (i < curve.valid.size() && !curve.valid[i])
            {
                continue;
            }
            double const v = vals[i];
            if (!std::isfinite(v))
            {
                continue;
            }
            if (!any)
            {
                lo = hi = v;
                any = true;
            }
            else
            {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
        }
        if (!any)
        {
            return;
        }
        double const span = hi - lo;
        auto norm = [&](double v)
        {
            if (!std::isfinite(v) || span < 1e-9)
            {
                return 0.5;
            }
            return std::clamp((v - lo) / span, 0.0, 1.0);
        };

        const double interval = curve.times.size() > 1
            ? (curve.times[1] - curve.times[0])
            : (m_duration / static_cast<double>(n));
        for (int j = 0; j < static_cast<int>(m_bars.size()); ++j)
        {
            auto const a = static_cast<std::size_t>(n * static_cast<std::size_t>(j) / m_bars.size());
            auto const b = static_cast<std::size_t>(n * static_cast<std::size_t>(j + 1) / m_bars.size());
            if (b <= a || a >= n || b - 1 >= n)
            {
                continue;
            }
            double sum = 0.0;
            int count = 0;
            for (std::size_t i = a; i < b; ++i)
            {
                if (i < curve.valid.size() && !curve.valid[i])
                {
                    continue;
                }
                double const v = vals[i];
                if (!std::isfinite(v))
                {
                    continue;
                }
                sum += norm(v);
                ++count;
            }
            if (count == 0)
            {
                continue; // 这一段没有可信数据：留空，不画一个假的 0
            }
            const double value = sum / static_cast<double>(count);

            // 桶心在 (i+0.5)*interval，所以这一段的左右边界是首尾桶心各外推半格。
            const double left = TimeToX(curve.times[a] - interval * 0.5, m_duration, width);
            const double right = TimeToX(std::min(curve.times[b - 1] + interval * 0.5, m_duration), m_duration, width);
            const double barWidth = std::max(1.0, right - left - 1.0);
            const double barHeight = std::max(1.5, value * (height - 6.0));

            auto& bar = m_bars[j];
            bar.Width(barWidth);
            bar.Height(barHeight);
            Canvas::SetLeft(bar, left);
            Canvas::SetTop(bar, height - barHeight);
            bar.Visibility(Visibility::Visible);
        }

        for (std::size_t i = 0; i < m_segments.size() && i < m_bands.size(); ++i)
        {
            auto const& seg = m_segments[i];
            const double left = TimeToX(seg.start, m_duration, width);
            const double right = TimeToX(seg.end, m_duration, width);
            if (right <= left)
            {
                continue;
            }
            auto& band = m_bands[i];
            band.Fill(SolidColorBrush{ GroupColor(seg.group, 0x40) });
            band.Width(std::max(1.0, right - left - 1.0));
            band.Height(height);
            Canvas::SetLeft(band, left);
            Canvas::SetTop(band, 0.0);
            band.Visibility(Visibility::Visible);

            // 边界线的不透明度就是引擎给的边界置信度：模糊的接缝画成淡的。
            if (i > 0 && i < m_ticks.size())
            {
                auto& tick = m_ticks[i - 1];
                auto const alpha = static_cast<std::uint8_t>(0x40 + std::clamp(seg.boundaryConfidence, 0.0, 1.0) * 0xA0);
                tick.Fill(SolidColorBrush{ ColorHelper::FromArgb(alpha, accent.R, accent.G, accent.B) });
                tick.Width(1.0);
                tick.Height(height);
                Canvas::SetLeft(tick, left);
                Canvas::SetTop(tick, 0.0);
                tick.Visibility(Visibility::Visible);
            }
        }

        SetPosition(m_position);
    }
}
