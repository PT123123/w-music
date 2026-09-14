#pragma once

#include "OnlineTrackItem.g.h"
#include "QualityChipItem.h"

namespace winrt::w_music::implementation
{
    /// Plain data holder for a track that still lives on a remote source.
    /// Once downloaded it becomes an ordinary TrackItem in the local library.
    struct OnlineTrackItem : OnlineTrackItemT<OnlineTrackItem>
    {
        OnlineTrackItem() = default;

        hstring SourceId() const noexcept { return m_sourceId; }
        void SourceId(hstring const& value) noexcept { m_sourceId = value; }

        hstring Id() const noexcept { return m_id; }
        void Id(hstring const& value) noexcept { m_id = value; }

        hstring Title() const noexcept { return m_title; }
        void Title(hstring const& value) noexcept { m_title = value; }

        hstring Artist() const noexcept { return m_artist; }
        void Artist(hstring const& value) noexcept { m_artist = value; }

        hstring Album() const noexcept { return m_album; }
        void Album(hstring const& value) noexcept { m_album = value; }

        hstring DurationText() const noexcept { return m_durationText; }
        void DurationText(hstring const& value) noexcept { m_durationText = value; }

        int64_t DurationMs() const noexcept { return m_durationMs; }
        void DurationMs(int64_t value) noexcept { m_durationMs = value; }

        hstring PlayUrl() const noexcept { return m_playUrl; }
        void PlayUrl(hstring const& value) noexcept { m_playUrl = value; }

        hstring DownloadUrl() const noexcept { return m_downloadUrl; }
        void DownloadUrl(hstring const& value) noexcept { m_downloadUrl = value; }

        hstring CoverUrl() const noexcept { return m_coverUrl; }
        void CoverUrl(hstring const& value) noexcept { m_coverUrl = value; }

        hstring DetailUrl() const noexcept { return m_detailUrl; }
        void DetailUrl(hstring const& value) noexcept { m_detailUrl = value; }

        hstring ExtraText() const noexcept { return m_extraText; }
        void ExtraText(hstring const& value) noexcept { m_extraText = value; }

        bool VipOnly() const noexcept { return m_vipOnly; }
        void VipOnly(bool value) noexcept { m_vipOnly = value; }

        hstring MasterId() const noexcept { return m_masterId; }
        void MasterId(hstring const& value) noexcept { m_masterId = value; }

        hstring LosslessId() const noexcept { return m_losslessId; }
        void LosslessId(hstring const& value) noexcept { m_losslessId = value; }

        winrt::Windows::Foundation::Collections::IVectorView<winrt::w_music::QualityChipItem> Qualities() const noexcept
        {
            return m_qualities.GetView();
        }

        winrt::Microsoft::UI::Xaml::Visibility VipVisibility() const noexcept
        {
            return m_vipOnly
                ? winrt::Microsoft::UI::Xaml::Visibility::Visible
                : winrt::Microsoft::UI::Xaml::Visibility::Collapsed;
        }

        /// Adds one download tier chip (net24 rows only).
        void AddQuality(hstring const& key, hstring const& label)
        {
            auto chip = winrt::make<winrt::w_music::implementation::QualityChipItem>();
            chip.Key(key);
            chip.Label(label);
            m_qualities.Append(std::move(chip));
        }

    private:
        hstring m_sourceId;
        hstring m_id;
        hstring m_title;
        hstring m_artist;
        hstring m_album;
        hstring m_durationText;
        hstring m_playUrl;
        hstring m_downloadUrl;
        hstring m_coverUrl;
        hstring m_detailUrl;
        hstring m_extraText;
        int64_t m_durationMs = 0;
        bool m_vipOnly = false;
        hstring m_masterId;
        hstring m_losslessId;
        winrt::Windows::Foundation::Collections::IVector<winrt::w_music::QualityChipItem> m_qualities{
            winrt::single_threaded_vector<winrt::w_music::QualityChipItem>()
        };
    };
} // namespace winrt::w_music::implementation

namespace winrt::w_music::factory_implementation
{
    struct OnlineTrackItem : OnlineTrackItemT<OnlineTrackItem, implementation::OnlineTrackItem>
    {
    };
}
