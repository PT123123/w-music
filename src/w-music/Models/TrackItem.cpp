#include "pch.h"

#include "TrackItem.h"
#include "TrackItem.g.cpp"

#include "Services/AppPaths.h"

namespace winrt::w_music::implementation
{
    void TrackItem::RaisePropertyChanged(std::wstring_view const& name)
    {
        if (!wm::app::UiThread())
        {
            wm::app::Diag("Track Raise off-ui: " + wm::app::Utf8(name));
            wm::app::PostToUi([strong = get_strong(), text = std::wstring{ name }] {
                strong->RaisePropertyChanged(text);
            });
            return;
        }
        m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ hstring{ name } });
    }

    void TrackItem::Id(hstring const& value)
    {
        if (m_id != value) { m_id = value; RaisePropertyChanged(L"Id"); }
    }

    void TrackItem::Title(hstring const& value)
    {
        if (m_title != value) { m_title = value; RaisePropertyChanged(L"Title"); }
    }

    void TrackItem::Artist(hstring const& value)
    {
        if (m_artist != value) { m_artist = value; RaisePropertyChanged(L"Artist"); }
    }

    void TrackItem::Album(hstring const& value)
    {
        if (m_album != value) { m_album = value; RaisePropertyChanged(L"Album"); }
    }

    void TrackItem::FilePath(hstring const& value)
    {
        if (m_filePath != value) { m_filePath = value; RaisePropertyChanged(L"FilePath"); }
    }

    void TrackItem::DurationMs(int64_t value)
    {
        if (m_durationMs != value)
        {
            m_durationMs = value;
            RaisePropertyChanged(L"DurationMs");
            RaisePropertyChanged(L"DurationText");
        }
    }

    hstring TrackItem::DurationText() const
    {
        const int64_t totalSeconds = m_durationMs / 1000;
        wchar_t buffer[16]{};
        swprintf_s(buffer, L"%02lld:%02lld",
                   static_cast<long long>(totalSeconds / 60),
                   static_cast<long long>(totalSeconds % 60));
        return hstring{ buffer };
    }

    void TrackItem::IsFavorite(bool value)
    {
        if (m_isFavorite != value) { m_isFavorite = value; RaisePropertyChanged(L"IsFavorite"); }
    }

    void TrackItem::PlayCount(int32_t value)
    {
        if (m_playCount != value) { m_playCount = value; RaisePropertyChanged(L"PlayCount"); }
    }

    void TrackItem::IsPlaying(bool value)
    {
        if (m_isPlaying != value) { m_isPlaying = value; RaisePropertyChanged(L"IsPlaying"); }
    }

    winrt::event_token TrackItem::PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
    {
        return m_propertyChanged.add(handler);
    }
}
