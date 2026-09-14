#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace wm::app
{
    /// Captures what the machine is playing (WASAPI loopback) and hands the raw
    /// PCM frames to a callback, which is used to drive the spectrum analyser.
    ///
    /// Runs entirely on a background thread; the callback is invoked there, so
    /// the consumer is responsible for marshalling to the UI thread.
    class WasapiLoopback
    {
    public:
        using FrameCallback = std::function<void(const float* samples, std::size_t frames, int channels, std::uint32_t sampleRate)>;

        WasapiLoopback() = default;
        ~WasapiLoopback();

        WasapiLoopback(const WasapiLoopback&) = delete;
        WasapiLoopback& operator=(const WasapiLoopback&) = delete;

        bool Start(FrameCallback callback);
        void Stop();
        bool IsRunning() const noexcept { return m_running.load(); }

        std::wstring LastError() const;

    private:
        void ThreadMain();

        std::thread m_thread;
        std::atomic<bool> m_running{ false };
        std::atomic<bool> m_stopRequested{ false };
        FrameCallback m_callback;
        mutable std::mutex m_errorMutex;
        std::wstring m_lastError;
    };
}
