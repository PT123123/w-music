#pragma once

#include "TrackItem.g.h"

namespace winrt::w_music::implementation
{
    struct TrackItem : TrackItemT<TrackItem>
    {
        TrackItem() = default;

        hstring Id() const noexcept { return m_id; }
        void Id(hstring const& value);

        hstring Title() const noexcept { return m_title; }
        void Title(hstring const& value);

        hstring Artist() const noexcept { return m_artist; }
        void Artist(hstring const& value);

        hstring Album() const noexcept { return m_album; }
        void Album(hstring const& value);

        hstring FilePath() const noexcept { return m_filePath; }
        void FilePath(hstring const& value);

        int64_t DurationMs() const noexcept { return m_durationMs; }
        void DurationMs(int64_t value);

        hstring DurationText() const;

        bool IsFavorite() const noexcept { return m_isFavorite; }
        void IsFavorite(bool value);

        int32_t PlayCount() const noexcept { return m_playCount; }
        void PlayCount(int32_t value);

        bool IsPlaying() const noexcept { return m_isPlaying; }
        void IsPlaying(bool value);

        winrt::event_token PropertyChanged(winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler const& handler);
        void PropertyChanged(winrt::event_token const& token) noexcept { m_propertyChanged.remove(token); }

    private:
        void RaisePropertyChanged(std::wstring_view const& name);

        hstring m_id;
        hstring m_title;
        hstring m_artist;
        hstring m_album;
        hstring m_filePath;
        int64_t m_durationMs = 0;
        int32_t m_playCount = 0;
        bool m_isFavorite = false;
        bool m_isPlaying = false;
        winrt::event<winrt::Microsoft::UI::Xaml::Data::PropertyChangedEventHandler> m_propertyChanged;
    };
}

namespace winrt::w_music::factory_implementation
{
    struct TrackItem : TrackItemT<TrackItem, implementation::TrackItem>
    {
    };
}
