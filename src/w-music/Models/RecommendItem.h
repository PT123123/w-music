#pragma once

#include "RecommendItem.g.h"

namespace winrt::w_music::implementation
{
    /// One row produced by the local MIR recommendation engine (the
    /// music-recommend project, https://github.com/PT123123/music-recommend).
    /// Pure data record: the recommendation lists are rebuilt wholesale, so
    /// unlike TrackItem nothing here needs INotifyPropertyChanged. Playback
    /// reuses TrackItem, built from these fields by the recommend view model.
    struct RecommendItem : RecommendItemT<RecommendItem>
    {
        RecommendItem() = default;

        /// Engine-side content-hash track id (stable across renames), used for
        /// /v1/feed/feedback calls.
        hstring TrackId() const noexcept { return m_trackId; }
        void TrackId(hstring const& value) noexcept { m_trackId = value; }

        hstring Title() const noexcept { return m_title; }
        void Title(hstring const& value) noexcept { m_title = value; }

        hstring Artist() const noexcept { return m_artist; }
        void Artist(hstring const& value) noexcept { m_artist = value; }

        /// Collapsed when the file carries no artist tag (hides the empty line).
        winrt::Microsoft::UI::Xaml::Visibility HasArtist() const noexcept
        {
            return m_artist.empty()
                ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                : winrt::Microsoft::UI::Xaml::Visibility::Visible;
        }

        hstring Album() const noexcept { return m_album; }
        void Album(hstring const& value) noexcept { m_album = value; }

        hstring FilePath() const noexcept { return m_filePath; }
        void FilePath(hstring const& value) noexcept { m_filePath = value; }

        /// Recommendation score normalized to 0..1 by the engine.
        double Score() const noexcept { return m_score; }
        void Score(double value) noexcept { m_score = value; }

        /// "87" style percentage label rendered next to the row; "探索" when
        /// the engine returned no score (cold-start diversity pick).
        hstring ScoreText() const noexcept { return m_scoreText; }
        void ScoreText(hstring const& value) noexcept { m_scoreText = value; }

        /// Small caption under ScoreText: "匹配" normally, "冷启动" while the
        /// engine has no interest vector yet.
        hstring ScoreLabel() const noexcept { return m_scoreLabel; }
        void ScoreLabel(hstring const& value) noexcept { m_scoreLabel = value; }

        /// Human-readable reasons joined by "、", straight from the engine.
        hstring Reasons() const noexcept { return m_reasons; }
        void Reasons(hstring const& value) noexcept { m_reasons = value; }

        /// Collapsed when the engine had no reasons for this pair.
        winrt::Microsoft::UI::Xaml::Visibility ReasonsVisibility() const noexcept
        {
            return m_reasons.empty()
                ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                : winrt::Microsoft::UI::Xaml::Visibility::Visible;
        }

        /// "摇滚 · 国语" from the file's own tags (engine display.genre /
        /// display.language). Empty unless meta_source says a tag exists.
        hstring TagText() const noexcept { return m_tagText; }
        void TagText(hstring const& value) noexcept { m_tagText = value; }

        winrt::Microsoft::UI::Xaml::Visibility HasTagText() const noexcept
        {
            return m_tagText.empty()
                ? winrt::Microsoft::UI::Xaml::Visibility::Collapsed
                : winrt::Microsoft::UI::Xaml::Visibility::Visible;
        }

    private:
        hstring m_trackId;
        hstring m_title;
        hstring m_artist;
        hstring m_album;
        hstring m_filePath;
        double m_score = 0.0;
        hstring m_scoreText;
        hstring m_scoreLabel{ L"匹配" };
        hstring m_reasons;
        hstring m_tagText;
    };
} // namespace winrt::w_music::implementation

namespace winrt::w_music::factory_implementation
{
    struct RecommendItem : RecommendItemT<RecommendItem, implementation::RecommendItem>
    {
    };
}
