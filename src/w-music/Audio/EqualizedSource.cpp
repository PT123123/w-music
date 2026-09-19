#include "pch.h"

#include "Audio/EqualizedSource.h"

// pch 里没有 MediaProperties —— AudioEncodingProperties / MediaEncodingSubtypes
// 的 consume 模板实现全在这个头里，缺了它就是 C3779。
#include <winrt/Windows.Media.MediaProperties.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Media::Core;
using namespace Windows::Media::MediaProperties;
using namespace Windows::Storage::Streams;

namespace wm::app
{
    namespace
    {
        constexpr int64_t kHnsPerSecond = 10000000;

        // winrt::Windows::Foundation::TimeSpan is projected as a std::chrono
        // duration with 100 ns ticks -- construct from the raw count.
        TimeSpan ToTimeSpanHns(int64_t hns)
        {
            return TimeSpan{ static_cast<TimeSpan::rep>(hns) };
        }

        int64_t ToHns(TimeSpan const& value)
        {
            return static_cast<int64_t>(value.count());
        }

        // MFStartup / MFShutdown are process-wide and ref-counted.
        std::mutex g_mfMutex;
        int g_mfUsers = 0;

        void MfAddRef()
        {
            std::lock_guard<std::mutex> lock(g_mfMutex);
            if (g_mfUsers++ == 0)
            {
                MFStartup(MF_VERSION, MFSTARTUP_LITE);
            }
        }

        void MfRelease()
        {
            std::lock_guard<std::mutex> lock(g_mfMutex);
            if (--g_mfUsers == 0)
            {
                MFShutdown();
            }
        }
    } // namespace

    EqualizedSource::EqualizedSource(wm::core::Equalizer& equalizer, std::wstring path)
        : m_equalizer(equalizer), m_path(std::move(path))
    {
    }

    EqualizedSource::~EqualizedSource()
    {
        Stop();
        if (m_thread.joinable())
        {
            m_thread.join();
        }
        if (m_reader != nullptr)
        {
            m_reader->Release();
            m_reader = nullptr;
            MfRelease();
        }
    }

    void EqualizedSource::Stop()
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stop = true;
        }
        m_cv.notify_all();
        // 管线可能还挂着 SampleRequested 的 deferral，立刻以 EOS 收尾。
        DrainDeferralsAsEnd();
    }

    bool EqualizedSource::Open()
    {
        try
        {
            if (!OpenReader())
            {
                return false;
            }
            if (!NegotiateFloatFormat())
            {
                return false;
            }

            // CreatePcm 保证 Type=="Audio"（Type 属性只读），再把子类型改写成
            // float32（MFAudioFormat_Float），与读出的 PCM 一致。
            auto encoding = AudioEncodingProperties::CreatePcm(m_sampleRate, m_channels, 32);
            encoding.Subtype(MediaEncodingSubtypes::Float());
            encoding.Bitrate(m_sampleRate * m_channels * 32);

            AudioStreamDescriptor descriptor{ encoding };

            m_streamSource = MediaStreamSource{ descriptor };
            m_streamSource.CanSeek(true);
            m_streamSource.Duration(ToTimeSpanHns(m_durationHns));
            m_streamSource.BufferTime(ToTimeSpanHns(kHnsPerSecond)); // 1 s of buffering

            std::weak_ptr<EqualizedSource> weak{ weak_from_this() };
            m_streamSource.SampleRequested([weak](auto&& sender, auto&& args) {
                if (auto self = weak.lock())
                {
                    self->OnSampleRequested(sender, args);
                }
            });
            m_streamSource.Starting([weak](auto&& sender, auto&& args) {
                if (auto self = weak.lock())
                {
                    self->OnStarting(sender, args);
                }
            });
            m_streamSource.Closed([weak](auto&& sender, auto&& args) {
                if (auto self = weak.lock())
                {
                    self->OnClosed(sender, args);
                }
            });

            m_thread = std::thread([this] { ReadLoop(); });
            return true;
        }
        catch (...)
        {
            return false;
        }
    }

    bool EqualizedSource::OpenReader()
    {
        MfAddRef();
        HRESULT hr = MFCreateSourceReaderFromURL(m_path.c_str(), nullptr, &m_reader);
        if (FAILED(hr) || m_reader == nullptr)
        {
            m_reader = nullptr;
            MfRelease();
            return false;
        }

        // Audio only; video / timed metadata stay unselected.
        m_reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
        hr = m_reader->SetStreamSelection(MF_SOURCE_READER_FIRST_AUDIO_STREAM, TRUE);
        if (FAILED(hr))
        {
            return false;
        }

        // Duration: MF_PD_DURATION on the media source level (GetSourceAttributes
        // is IMFSourceReader2-only; GetPresentationAttribute is on the base
        // interface and gives the same attribute).
        PROPVARIANT var{};
        if (SUCCEEDED(m_reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var))
            && var.vt == VT_UI8)
        {
            m_durationHns = static_cast<int64_t>(var.uhVal.QuadPart);
        }
        PropVariantClear(&var);
        if (m_durationHns <= 0)
        {
            // Without a duration the player cannot end the track cleanly.
            return false;
        }
        return true;
    }

    bool EqualizedSource::NegotiateFloatFormat()
    {
        IMFMediaType* native = nullptr;
        if (FAILED(m_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &native)) || native == nullptr)
        {
            return false;
        }

        UINT32 rate = 0, channels = 0;
        native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
        native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
        native->Release();
        if (rate == 0 || channels == 0)
        {
            return false;
        }

        // Ask the decoder for float32; the source reader inserts the
        // audio-decoder controller for the int -> float conversion.
        // NOTE: the plain IMFSourceReader only has the 3-arg overload; the
        // actual negotiated type is read back with GetCurrentMediaType.
        IMFMediaType* proposed = nullptr;
        IMFMediaType* actual = nullptr;
        bool ok = false;
        if (SUCCEEDED(MFCreateMediaType(&proposed)))
        {
            proposed->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            proposed->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            if (SUCCEEDED(m_reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, proposed)))
            {
                if (SUCCEEDED(m_reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, &actual)) && actual != nullptr)
                {
                    GUID subtype{};
                    UINT32 outRate = 0, outChannels = 0;
                    actual->GetGUID(MF_MT_SUBTYPE, &subtype);
                    actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &outRate);
                    actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &outChannels);
                    if (outRate != 0 && outChannels != 0)
                    {
                        m_sampleRate = outRate;
                        m_channels = outChannels;
                        m_outputIsFloat = subtype == MFAudioFormat_Float;
                        ok = true;
                    }
                }
            }
            if (actual != nullptr)
            {
                actual->Release();
            }
            proposed->Release();
        }

        if (!ok)
        {
            return false;
        }
        m_equalizer.SetSampleRate(static_cast<double>(m_sampleRate));
        m_equalizer.Reset();
        return true;
    }

    void EqualizedSource::ReadLoop()
    {
        for (;;)
        {
            bool needSeek = false;
            int64_t seekHns = 0;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                if (m_stop)
                {
                    break;
                }
                if (m_seekTo)
                {
                    seekHns = *m_seekTo;
                    m_seekTo.reset();
                    needSeek = true;
                    m_queue.clear();
                    m_eof = false;
                    m_failed = false;
                }
                else if (!m_eof && !m_failed)
                {
                    std::size_t floats = 0;
                    for (auto const& chunk : m_queue)
                        floats += chunk.pcm.size();
                    if (floats > MaxQueuedFloats / 2)
                    {
                        m_cv.wait_for(lock, std::chrono::milliseconds{ 50 });
                        continue;
                    }
                }
            }

            IMFSourceReader* reader = nullptr;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                reader = m_reader;
            }
            if (reader == nullptr)
            {
                break;
            }

            if (needSeek)
            {
                // 同步 source reader 的 seek：SetCurrentPosition（GUID_NULL =
                // 100ns 绝对流时间）。文档：调用后直接 ReadSample 即从新位置续读。
                PROPVARIANT var{};
                var.vt = VT_I8;
                var.hVal.QuadPart = seekHns;
                if (FAILED(reader->SetCurrentPosition(GUID_NULL, var)))
                {
                    {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        m_failed = true;
                    }
                    DrainDeferralsAsEnd();
                    continue;
                }
                m_nextPosHns = seekHns;
            }

            if (m_eof || m_failed)
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv.wait_for(lock, std::chrono::milliseconds{ 100 });
                continue;
            }

            DWORD actualStream = 0xFFFFFFFF;
            DWORD flags = 0;
            LONGLONG timestamp = -1;
            IMFSample* mediaSample = nullptr;
            HRESULT hr = reader->ReadSample(
                static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &actualStream, &flags, &timestamp, &mediaSample);
            if (FAILED(hr))
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_failed = true;
                }
                DrainDeferralsAsEnd();
                continue;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_eof = true;
                }
                DrainDeferralsAsEnd();
                continue;
            }
            if (flags & MF_SOURCE_READERF_ERROR)
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_failed = true;
                }
                DrainDeferralsAsEnd();
                continue;
            }
            if (mediaSample == nullptr)
            {
                continue;
            }

            Chunk chunk;
            bool haveData = false;
            IMFMediaBuffer* buffer = nullptr;
            if (SUCCEEDED(mediaSample->ConvertToContiguousBuffer(&buffer)) && buffer != nullptr)
            {
                BYTE* data = nullptr;
                DWORD len = 0, maxLen = 0;
                if (SUCCEEDED(buffer->Lock(&data, &maxLen, &len)) && len > 0)
                {
                    std::size_t const bytesPerSample = m_outputIsFloat ? 4 : 2; // int16 fallback
                    std::size_t const frames = len / (bytesPerSample * m_channels);
                    if (frames > 0)
                    {
                        chunk.pcm.resize(frames * m_channels);
                        if (m_outputIsFloat)
                        {
                            std::memcpy(chunk.pcm.data(), data, frames * m_channels * sizeof(float));
                        }
                        else
                        {
                            auto const* ints = reinterpret_cast<const int16_t*>(data);
                            for (std::size_t i = 0; i < frames * m_channels; ++i)
                            {
                                chunk.pcm[i] = static_cast<float>(ints[i]) * (1.0f / 32768.0f);
                            }
                        }
                        chunk.posHns = timestamp >= 0 ? static_cast<int64_t>(timestamp) : m_nextPosHns;
                        chunk.durHns = static_cast<int64_t>(frames) * kHnsPerSecond / m_sampleRate;
                        m_nextPosHns = chunk.posHns + chunk.durHns;
                        if (timestamp >= 0)
                        {
                            m_nextPosHns = static_cast<int64_t>(timestamp) + chunk.durHns;
                        }
                        m_equalizer.Process(chunk.pcm.data(), frames, static_cast<int>(m_channels));
                        haveData = true;
                    }
                    buffer->Unlock();
                }
                buffer->Release();
            }
            mediaSample->Release();

            if (!haveData)
            {
                continue;
            }

            std::vector<Delivery> deliveries;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_queue.push_back(std::move(chunk));
                // 挂起的 deferral 必须连同数据一起交付 —— 只 Complete 不给
                // sample 等于提前 EOS。锁内配对，锁外完成（避免重入回调）。
                while (!m_queue.empty() && !m_deferrals.empty())
                {
                    Delivery delivery;
                    delivery.request = std::move(m_deferrals.front().request);
                    delivery.deferral = std::move(m_deferrals.front().deferral);
                    m_deferrals.pop_front();
                    delivery.chunk = std::move(m_queue.front());
                    m_queue.pop_front();
                    delivery.discontinuous = m_discontinuityPending;
                    m_discontinuityPending = false;
                    deliveries.push_back(std::move(delivery));
                }
            }
            CompleteDeliveries(deliveries);
        }
    }

    MediaStreamSample EqualizedSource::MakeSample(Chunk const& chunk, bool discontinuous)
    {
        auto const bytes = static_cast<UINT32>(chunk.pcm.size() * sizeof(float));
        Buffer buffer{ bytes };
        std::memcpy(buffer.data(), chunk.pcm.data(), bytes);
        buffer.Length(bytes);

        auto sample = MediaStreamSample::CreateFromBuffer(buffer, ToTimeSpanHns(chunk.posHns));
        sample.Duration(ToTimeSpanHns(chunk.durHns));
        if (discontinuous)
        {
            sample.Discontinuous(true); // first sample after a seek
        }
        return sample;
    }

    void EqualizedSource::CompleteDeliveries(std::vector<Delivery>& deliveries)
    {
        for (auto& delivery : deliveries)
        {
            try
            {
                if (delivery.end)
                {
                    delivery.request.Sample(nullptr); // null sample == end of stream
                }
                else
                {
                    delivery.request.Sample(MakeSample(delivery.chunk, delivery.discontinuous));
                }
                delivery.deferral.Complete();
            }
            catch (...)
            {
                // The pipeline is tearing this source down underneath us; the
                // dropped request dies with it.
            }
        }
        deliveries.clear();
    }

    void EqualizedSource::DrainDeferralsAsEnd()
    {
        std::vector<Delivery> deliveries;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_queue.empty())
            {
                return; // deferrals only exist while the queue is drained
            }
            for (auto& pending : m_deferrals)
            {
                Delivery delivery;
                delivery.request = std::move(pending.request);
                delivery.deferral = std::move(pending.deferral);
                delivery.end = true;
                deliveries.push_back(std::move(delivery));
            }
            m_deferrals.clear();
        }
        CompleteDeliveries(deliveries);
    }

    void EqualizedSource::OnSampleRequested(MediaStreamSource const&, MediaStreamSourceSampleRequestedEventArgs const& args)
    {
        auto const request = args.Request();
        Chunk chunk;
        bool haveChunk = false;
        bool end = false;
        bool discontinuous = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_queue.empty())
            {
                chunk = std::move(m_queue.front());
                m_queue.pop_front();
                haveChunk = true;
                discontinuous = m_discontinuityPending;
                m_discontinuityPending = false;
                m_cv.notify_all(); // the reader may be waiting for room.
            }
            else
            {
                end = m_eof || m_failed || m_stop;
                if (!end)
                {
                    // 请求与 deferral 必须在同一把锁里入队，否则读线程置位
                    // eof 后做的 drain 可能看不到这个 deferral，管线永久卡住。
                    m_deferrals.push_back(Deferred{ request, request.GetDeferral() });
                    return;
                }
            }
        }

        try
        {
            if (haveChunk)
            {
                request.Sample(MakeSample(chunk, discontinuous));
            }
            else
            {
                request.Sample(nullptr);
            }
        }
        catch (...)
        {
        }
    }

    void EqualizedSource::OnStarting(MediaStreamSource const&, MediaStreamSourceStartingEventArgs const& args)
    {
        auto const request = args.Request();
        // StartPosition 是 IReference<TimeSpan>，首次正常启动时可能为 null。
        TimeSpan target{};
        if (auto const position = request.StartPosition())
        {
            target = position.Value();
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_seekTo = ToHns(target);
            m_discontinuityPending = ToHns(target) != 0 || !m_queue.empty();
        }
        m_cv.notify_all();
        request.SetActualStartPosition(target);
        request.GetDeferral().Complete();
    }

    void EqualizedSource::OnClosed(MediaStreamSource const&, MediaStreamSourceClosedEventArgs const&)
    {
        // 这个 SDK 版本的 Closed 事件没有 deferral（Request 只暴露 Reason），
        // 停掉读线程即可。
        Stop();
    }
} // namespace wm::app
