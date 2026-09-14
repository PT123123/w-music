#pragma once

#include "PlaylistItem.g.h"

namespace winrt::w_music::implementation
{
    struct PlaylistItem : PlaylistItemT<PlaylistItem>
    {
        PlaylistItem() = default;

        hstring Id() const noexcept { return m_id; }
        void Id(hstring const& value);

        hstring Name() const noexcept { return m_name; }
        void Name(hstring const& value);

        int32_t TrackCount() const noexcept { return m_trackCount; }
        void TrackCount(int32_t value);

        bool IsBuiltIn() const noexcept { return m_isBuiltIn; }
        void IsBuiltIn(bool value);

        hstring Glyph() const noexcept { return m_glyph; }
        void Glyph(hstring const& value);

        winrt::event_token PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept { m_propertyChanged.remove(token); }

    private:
        void RaisePropertyChanged(std::wstring_view const& name);

        hstring m_id;
        hstring m_name;
        hstring m_glyph{ L"\uE8D6" };
        int32_t m_trackCount = 0;
        bool m_isBuiltIn = false;
        winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;
    };
}

namespace winrt::w_music::factory_implementation
{
    struct PlaylistItem : PlaylistItemT<PlaylistItem, implementation::PlaylistItem>
    {
    };
}
