#pragma once

#include "pch.h"

#include <string>
#include <vector>

namespace wm::app
{
    /// 段落取色：一个重复组一个稳定的颜色。时间轴上的色块和歌词行左侧的色条
    /// 走的是同一支函数，所以「同色 = 同一段落反复出现」在两处讲的是同一件事。
    winrt::Windows::UI::Color StructureGroupColor(std::wstring const& group, std::uint8_t alpha);

    /// The 正在播放 page's structure timeline: the engine's full-track loudness
    /// curve with its sections painted behind it, drawn into a plain Canvas by
    /// resizing rectangles -- the SpectrumView approach, for the same reason:
    /// a few hundred buckets move faster as shapes than as bound elements.
    class StructureTimelineView
    {
    public:
        struct Segment
        {
            double start = 0.0;
            double end = 0.0;
            /// "A" / "B" / ...: the repeat letter. Sections of one group get one
            /// colour, which is the part of the analysis that needs no naming.
            std::wstring group;
            double boundaryConfidence = 0.0;
        };

        /// 一条可切换显示的曲线。|values| 是原始测量值（各曲线单位不同），
        /// 画图时按本曲线自身的 min-max 归一化；|valid| 为假的点不参与统计、
        /// 也不补零——零可能是真实测量结果。
        struct Curve
        {
            std::wstring id;
            std::vector<double> times;
            std::vector<double> values;
            std::vector<bool> valid;
        };

        void Attach(winrt::Microsoft::UI::Xaml::Controls::Canvas const& canvas);
        /// |times| are the engine's bucket centers in seconds and |curve| the
        /// matching 0..1 loudness. An empty curve paints nothing -- the view
        /// never invents a flat line to look like an answer.
        void SetData(double duration, std::vector<double> const& times,
                     std::vector<double> const& curve, std::vector<Segment> const& segments);
        /// 多曲线版本：切曲线走 SetActiveCurve，段落与播放头原样保留。
        void SetCurves(double duration, std::vector<Curve> const& curves,
                       std::vector<Segment> const& segments);
        void SetActiveCurve(std::size_t index);
        std::size_t curveCount() const noexcept { return m_curves.size(); }
        /// Moves the playhead; |seconds| outside the track simply parks it at
        /// the edge.
        void SetPosition(double seconds);
        void Detach();
        /// Seconds at a canvas x (click-to-jump), or -1.0 with nothing to place.
        double TimeAt(double x) const;
        double duration() const noexcept { return m_duration; }

    private:
        void Relayout();
        void HideAll();

        winrt::Microsoft::UI::Xaml::Controls::Canvas m_canvas{ nullptr };
        winrt::event_token m_sizeToken{};
        std::vector<winrt::Microsoft::UI::Xaml::Shapes::Rectangle> m_bars;
        std::vector<winrt::Microsoft::UI::Xaml::Shapes::Rectangle> m_bands;
        std::vector<winrt::Microsoft::UI::Xaml::Shapes::Rectangle> m_ticks;
        winrt::Microsoft::UI::Xaml::Shapes::Rectangle m_playhead{ nullptr };

        std::vector<Curve> m_curves;
        std::size_t m_active = 0;
        std::vector<Segment> m_segments;
        double m_duration = 0.0;
        double m_position = 0.0;
        double m_width = 0.0;
        double m_height = 0.0;
        /// Theme accent as last painted: a theme switch has to follow the
        /// curve without waiting for the page to be rebuilt.
        winrt::Windows::UI::Color m_paintedAccent{};
        winrt::Windows::UI::Color m_paintedAccentAlt{};
    };
}
