#include "pch.h"

#include "LyricLineItem.h"
#include "LyricLineItem.g.cpp"

namespace winrt::w_music::implementation
{
    namespace
    {
        using namespace winrt::Microsoft::UI::Xaml::Media;
        using namespace winrt::Windows::UI;

        /// 当前主题的强调色画刷（App.xaml 的 WmAccentBrush）。这里刻意复用共享
        /// 实例而不是新建一支：MainWindow::ApplyTheme 改的就是这支画刷的 Color，
        /// 于是正在显示的高亮歌词行会跟着换色，不用重建歌词列表。
        SolidColorBrush AccentBrush()
        {
            try
            {
                auto value = winrt::Microsoft::UI::Xaml::Application::Current().Resources()
                                 .Lookup(winrt::box_value(winrt::hstring{ L"WmAccentBrush" }));
                if (auto brush = value.try_as<SolidColorBrush>())
                {
                    return brush;
                }
            }
            catch (...)
            {
                // 取不到（比如不在 UI 线程）就退回默认绿。
            }
            return SolidColorBrush{ ColorHelper::FromArgb(0xFF, 0x31, 0xC2, 0x7C) };
        }

        SolidColorBrush MakeForeground(bool active)
        {
            if (active)
            {
                return AccentBrush();
            }
            return SolidColorBrush{ ColorHelper::FromArgb(0x99, 0xFF, 0xFF, 0xFF) };
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
