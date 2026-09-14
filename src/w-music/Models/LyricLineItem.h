#pragma once

#include "LyricLineItem.g.h"

namespace winrt::w_music::implementation
{
    struct LyricLineItem : LyricLineItemT<LyricLineItem>
    {
        LyricLineItem();

        int64_t TimeMs() const noexcept { return m_timeMs; }
        void TimeMs(int64_t value);

        hstring Text() const noexcept { return m_text; }
        void Text(hstring const& value);

        bool IsActive() const noexcept { return m_isActive; }
        void IsActive(bool value);

        int32_t Index() const noexcept { return m_index; }
        void Index(int32_t value);

        hstring TimeText() const;

        /// Accent colour while active, muted otherwise (no converter needed).
        winrt::Microsoft::UI::Xaml::Media::Brush Foreground() const noexcept { return m_foreground; }
        double FontSize() const noexcept { return m_fontSize; }

        winrt::event_token PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept { m_propertyChanged.remove(token); }

    private:
        void RaisePropertyChanged(std::wstring_view const& name);

        hstring m_text;
        int64_t m_timeMs = 0;
        int32_t m_index = 0;
        bool m_isActive = false;
        double m_fontSize = 15.0;
        winrt::Microsoft::UI::Xaml::Media::Brush m_foreground{ nullptr };
        winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;
    };
}

namespace winrt::w_music::factory_implementation
{
    struct LyricLineItem : LyricLineItemT<LyricLineItem, implementation::LyricLineItem>
    {
    };
}
