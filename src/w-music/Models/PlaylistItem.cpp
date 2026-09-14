#include "pch.h"

#include "PlaylistItem.h"
#include "PlaylistItem.g.cpp"

namespace winrt::w_music::implementation
{
    void PlaylistItem::RaisePropertyChanged(std::wstring_view const& name)
    {
        m_propertyChanged(*this, winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventArgs{ hstring{ name } });
    }

    void PlaylistItem::Id(hstring const& value)
    {
        if (m_id != value) { m_id = value; RaisePropertyChanged(L"Id"); }
    }

    void PlaylistItem::Name(hstring const& value)
    {
        if (m_name != value) { m_name = value; RaisePropertyChanged(L"Name"); }
    }

    void PlaylistItem::TrackCount(int32_t value)
    {
        if (m_trackCount != value) { m_trackCount = value; RaisePropertyChanged(L"TrackCount"); }
    }

    void PlaylistItem::IsBuiltIn(bool value)
    {
        if (m_isBuiltIn != value) { m_isBuiltIn = value; RaisePropertyChanged(L"IsBuiltIn"); }
    }

    void PlaylistItem::Glyph(hstring const& value)
    {
        if (m_glyph != value) { m_glyph = value; RaisePropertyChanged(L"Glyph"); }
    }

    winrt::event_token PlaylistItem::PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler)
    {
        return m_propertyChanged.add(handler);
    }
}
