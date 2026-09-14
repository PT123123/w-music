#include "pch.h"

#include "Controls/SpectrumView.h"

using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Media;
using namespace Microsoft::UI::Xaml::Shapes;
using namespace Windows::UI;

namespace wm::app
{
    void SpectrumView::Attach(Canvas const& canvas, int barCount)
    {
        Detach();
        if (canvas == nullptr || barCount <= 0)
        {
            return;
        }

        m_canvas = canvas;
        canvas.Children().Clear();

        m_bars.reserve(static_cast<std::size_t>(barCount));
        for (int i = 0; i < barCount; ++i)
        {
            const double t = static_cast<double>(i) / static_cast<double>(barCount > 1 ? barCount - 1 : 1);
            const auto r = static_cast<std::uint8_t>(0x31 + t * (0x2D - 0x31));
            const auto g = static_cast<std::uint8_t>(0xC2 + t * (0xD4 - 0xC2));
            const auto b = static_cast<std::uint8_t>(0x7C + t * (0xBF - 0x7C));

            // Fully qualified: windows.h declares a global Rectangle() GDI
            // function, so the unqualified name is ambiguous (C2872).
            winrt::Microsoft::UI::Xaml::Shapes::Rectangle rect;
            rect.Fill(SolidColorBrush{ ColorHelper::FromArgb(0xDD, r, g, b) });
            rect.RadiusX(1.5);
            rect.RadiusY(1.5);
            rect.Height(2.0);
            m_bars.push_back(rect);
            canvas.Children().Append(rect);
        }
    }

    void SpectrumView::Detach()
    {
        if (m_canvas != nullptr)
        {
            m_canvas.Children().Clear();
        }
        m_bars.clear();
        m_canvas = nullptr;
        m_lastWidth = 0.0;
        m_lastHeight = 0.0;
    }

    void SpectrumView::Update(std::vector<double> const& bars)
    {
        if (m_canvas == nullptr || m_bars.empty() || bars.empty())
        {
            return;
        }

        const double width = m_canvas.ActualWidth();
        const double height = m_canvas.ActualHeight();
        if (width <= 0.0 || height <= 0.0)
        {
            return;
        }
        m_lastWidth = width;
        m_lastHeight = height;

        const std::size_t count = std::min(bars.size(), m_bars.size());
        constexpr double kGap = 2.0;
        const double barWidth = std::max(1.0, (width - kGap * static_cast<double>(count - 1)) / static_cast<double>(count));

        for (std::size_t i = 0; i < count; ++i)
        {
            double barHeight = std::clamp(bars[i], 0.0, 1.0) * height;
            if (barHeight < 2.0)
            {
                barHeight = 2.0;
            }

            auto& rect = m_bars[i];
            rect.Width(barWidth);
            rect.Height(barHeight);
            Canvas::SetLeft(rect, static_cast<double>(i) * (barWidth + kGap));
            Canvas::SetTop(rect, height - barHeight);
        }
    }
}
