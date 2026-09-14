#pragma once

#include "pch.h"

#include <vector>

namespace wm::app
{
    /// Draws the spectrum bars into a plain Canvas by resizing a fixed set of
    /// rectangles every frame -- cheaper and simpler than data-binding 30 bars.
    class SpectrumView
    {
    public:
        void Attach(winrt::Microsoft::UI::Xaml::Controls::Canvas const& canvas, int barCount = 24);
        void Update(std::vector<double> const& bars);
        void Detach();

    private:
        winrt::Microsoft::UI::Xaml::Controls::Canvas m_canvas{ nullptr };
        std::vector<winrt::Microsoft::UI::Xaml::Shapes::Rectangle> m_bars;
        double m_lastWidth = 0.0;
        double m_lastHeight = 0.0;
    };
}
