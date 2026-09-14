#include "pch.h"

#include "LyricLineItem.h"
#include "LyricLineItem.g.cpp"

namespace winrt::w_music::implementation
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml::Media;
        using namespace winrt::Windows::UI;

        SolidColorBrush MakeForeground(bool active)
        {
            const Color colour = active
                ? ColorHelper::FromArgb(0xFF, 0x31, 0xC2, 0x7C)
                : ColorHelper::FromArgb(0x99, 0xFF, 0xFF, 0xFF);
            return SolidColorBrush{ colour };
        }
    } // namespace

    LyricLineItem::LyricLineItem()
        : m_foreground(MakeForeground(false))
    {
    }

    void LyricLineItem::RaisePropertyChanged(std::wstring_view const& name)
    {
        m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ hstring{ name } });
    }

    void LyricLineItem::TimeMs(int64_t value)
    {
        if (m_timeMs != value)
        {
            m_timeMs = value;
            RaisePropertyChanged(L"TimeMs");
            RaisePropertyChanged(L"TimeText");
        }
    }

    void LyricLineItem::Text(hstring const& value)
    {
        if (m_text != value) { m_text = value; RaisePropertyChanged(L"Text"); }
    }

    void LyricLineItem::IsActive(bool value)
    {
        if (m_isActive == value)
        {
            return;
        }
        m_isActive = value;
        m_foreground = MakeForeground(value);
        m_fontSize = value ? 20.0 : 15.0;
        RaisePropertyChanged(L"IsActive");
        RaisePropertyChanged(L"Foreground");
        RaisePropertyChanged(L"FontSize");
    }

    void LyricLineItem::Index(int32_t value)
    {
        if (m_index != value) { m_index = value; RaisePropertyChanged(L"Index"); }
    }

    hstring LyricLineItem::TimeText() const
    {
        const int64_t totalSeconds = m_timeMs / 1000;
        wchar_t buffer[16]{};
        swprintf_s(buffer, L"%02lld:%02lld",
                   static_cast<long long>(totalSeconds / 60),
                   static_cast<long long>(totalSeconds % 60));
        return hstring{ buffer };
    }

    winrt::event_token LyricLineItem::PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
    {
        return m_propertyChanged.add(handler);
    }
}
