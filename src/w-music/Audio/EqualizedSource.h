#pragma once

/// MediaStreamSource that decodes a media file / http stream through
/// MediaFoundation (IMFSourceReader), runs the PCM through
/// wm::core::Equalizer, and re-publishes the filtered float samples as PCM to
/// the player pipeline. This is the only way to insert DSP into
/// MediaPlayer's output on this platform (MediaPlayer exposes no frame
/// hook-up).
///
/// Threading: Open() + the decode loop run on a background thread; the
/// MediaStreamSource callbacks are invoked by the pipeline worker threads.
/// All queue access is serialised by m_mutex. Event handlers hold a weak_ptr,
/// so dropping the shared_ptr from the view-model detaches the source safely.

#include "pch.h"

#include <wm/core/Equalizer.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>

struct IMFSourceReader;

namespace wm::app
{
    class EqualizedSource : public std::enable_shared_from_this<EqualizedSource>
    {
    public:
        EqualizedSource(wm::core::Equalizer& equalizer, std::wstring path);
        ~EqualizedSource();

        EqualizedSource(const EqualizedSource&) = delete;
        EqualizedSource& operator=(const EqualizedSource&) = delete;

        /// Blocks while MediaFoundation opens the stream and negotiates the
        /// float PCM output format. Returns false when anything failed -- the
        /// caller must then fall back to a plain MediaSource (no EQ).
        bool Open();

        /// The wrapped source to hand to MediaSource::CreateFromMediaStreamSource.
        winrt::Windows::Media::Core::MediaStreamSource Source() const noexcept { return m_streamSource; }

    private:
        struct Chunk
        {
            std::vector<float> pcm; // interleaved, m_channels wide
            int64_t posHns = 0;     // 100 ns units, stream-relative
            int64_t durHns = 0;
        };

        /// A sample request that arrived with an empty queue. Request and
        /// deferral are stored (under m_mutex!) as a pair so the producer can
        /// finish them without racing a late GetDeferral().
        struct Deferred
        {
            winrt::Windows::Media::Core::MediaStreamSourceSampleRequest request{ nullptr };
            winrt::Windows::Media::Core::MediaStreamSourceSampleRequestDeferral deferral{ nullptr };
        };

        /// One finished delivery: a chunk (or an explicit end) for a request
        /// that was deferred earlier. Built under the lock, completed outside.
        struct Delivery
        {
            winrt::Windows::Media::Core::MediaStreamSourceSampleRequest request{ nullptr };
            winrt::Windows::Media::Core::MediaStreamSourceSampleRequestDeferral deferral{ nullptr };
            Chunk chunk;
            bool discontinuous = false;
            bool end = false; // deliver a null sample (end of stream)
        };

        bool OpenReader();
        bool NegotiateFloatFormat();
        void ReadLoop();
        void Stop();

        winrt::Windows::Media::Core::MediaStreamSample MakeSample(Chunk const& chunk, bool discontinuous);
        void CompleteDeliveries(std::vector<Delivery>& deliveries);
        /// Flushes every pending deferral with an end-of-stream null sample;
        /// call after setting m_eof / m_failed / m_stop.
        void DrainDeferralsAsEnd();

        void OnSampleRequested(winrt::Windows::Media::Core::MediaStreamSource const& sender,
                               winrt::Windows::Media::Core::MediaStreamSourceSampleRequestedEventArgs const& args);
        void OnStarting(winrt::Windows::Media::Core::MediaStreamSource const& sender,
                        winrt::Windows::Media::Core::MediaStreamSourceStartingEventArgs const& args);
        void OnClosed(winrt::Windows::Media::Core::MediaStreamSource const& sender,
                      winrt::Windows::Media::Core::MediaStreamSourceClosedEventArgs const& args);

        static constexpr std::size_t MaxQueuedFloats = 6u * 48u * 1024u * 2u; // ~6 s of stereo

        wm::core::Equalizer& m_equalizer;
        const std::wstring m_path;

        IMFSourceReader* m_reader = nullptr;
        uint32_t m_sampleRate = 0;
        uint32_t m_channels = 0;
        bool m_outputIsFloat = false;
        int64_t m_durationHns = 0;

        winrt::Windows::Media::Core::MediaStreamSource m_streamSource{ nullptr };

        std::thread m_thread;
        std::condition_variable m_cv;
        std::mutex m_mutex;
        std::deque<Chunk> m_queue;
        std::deque<Deferred> m_deferrals;
        std::optional<int64_t> m_seekTo; // set by Starting, consumed by ReadLoop
        bool m_discontinuityPending = false;
        int64_t m_nextPosHns = 0;        // fallback timestamp when the reader reports -1
        bool m_eof = false;
        bool m_failed = false;
        bool m_stop = false;
    };
} // namespace wm::app
