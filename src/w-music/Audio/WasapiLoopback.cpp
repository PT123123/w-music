#include "pch.h"

#include "Audio/WasapiLoopback.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>

#pragma comment(lib, "ole32.lib")

namespace wm::app
{
    namespace
    {
        // Avoid pulling in ksmedia.h just for these two well-known values.
        constexpr GUID kSubtypeIeeeFloat{ 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 } };
        constexpr std::uint16_t kWaveFormatExtensible = 0xFFFE;
        constexpr std::uint16_t kWaveFormatIeeeFloat = 0x0003;

        struct FormatInfo
        {
            std::uint32_t channels = 2;
            std::uint32_t sampleRate = 48000;
            std::uint16_t bitsPerSample = 16;
            bool isFloat = false;
        };

        FormatInfo Describe(const WAVEFORMATEX* fmt)
        {
            FormatInfo info;
            info.channels = fmt->nChannels;
            info.sampleRate = fmt->nSamplesPerSec;
            info.bitsPerSample = fmt->wBitsPerSample;

            if (fmt->wFormatTag == kWaveFormatExtensible)
            {
                const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
                info.isFloat = IsEqualGUID(ext->SubFormat, kSubtypeIeeeFloat);
                if (ext->Samples.wValidBitsPerSample != 0)
                {
                    info.bitsPerSample = static_cast<std::uint16_t>(ext->Samples.wValidBitsPerSample);
                }
            }
            else
            {
                info.isFloat = fmt->wFormatTag == kWaveFormatIeeeFloat;
            }
            return info;
        }

        void ConvertToFloat(const BYTE* data, std::uint32_t frames, const FormatInfo& info, std::vector<float>& out)
        {
            const std::size_t count = static_cast<std::size_t>(frames) * info.channels;
            out.assign(count, 0.0f);

            if (info.isFloat && info.bitsPerSample == 32)
            {
                const auto* src = reinterpret_cast<const float*>(data);
                std::memcpy(out.data(), src, count * sizeof(float));
                return;
            }

            if (!info.isFloat && info.bitsPerSample == 16)
            {
                const auto* src = reinterpret_cast<const std::int16_t*>(data);
                for (std::size_t i = 0; i < count; ++i)
                {
                    out[i] = static_cast<float>(src[i]) / 32768.0f;
                }
                return;
            }

            if (!info.isFloat && info.bitsPerSample == 32)
            {
                const auto* src = reinterpret_cast<const std::int32_t*>(data);
                for (std::size_t i = 0; i < count; ++i)
                {
                    out[i] = static_cast<float>(src[i]) / 2147483648.0f;
                }
                return;
            }

            if (!info.isFloat && info.bitsPerSample == 24)
            {
                for (std::size_t i = 0; i < count; ++i)
                {
                    const std::size_t base = i * 3;
                    std::int32_t value = static_cast<std::int32_t>(data[base])
                        | (static_cast<std::int32_t>(data[base + 1]) << 8)
                        | (static_cast<std::int32_t>(data[base + 2]) << 16);
                    if (value & 0x800000)
                    {
                        value |= ~0xFFFFFF; // sign extend
                    }
                    out[i] = static_cast<float>(value) / 8388608.0f;
                }
                return;
            }

            // Unsupported layout: hand back silence rather than garbage.
        }

        std::wstring DescribeHresult(HRESULT hr)
        {
            std::wstring message(64, L'\0');
            const DWORD written = FormatMessageW(
                FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                nullptr, static_cast<DWORD>(hr), 0, message.data(),
                static_cast<DWORD>(message.size()), nullptr);
            if (written == 0)
            {
                return L"HRESULT 0x" + std::to_wstring(static_cast<unsigned long>(hr));
            }
            message.resize(static_cast<std::size_t>(written));
            while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r'))
            {
                message.pop_back();
            }
            return message;
        }
    } // namespace

    WasapiLoopback::~WasapiLoopback()
    {
        Stop();
    }

    std::wstring WasapiLoopback::LastError() const
    {
        std::lock_guard<std::mutex> lock(m_errorMutex);
        return m_lastError;
    }

    void WasapiLoopback::Stop()
    {
        if (!m_running.load() && !m_thread.joinable())
        {
            return;
        }
        m_stopRequested.store(true);
        m_running.store(false);
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

    bool WasapiLoopback::Start(FrameCallback callback)
    {
        Stop();
        m_stopRequested.store(false);
        m_callback = std::move(callback);
        m_running.store(true);
        m_thread = std::thread([this] { ThreadMain(); });
        return true;
    }

    void WasapiLoopback::ThreadMain()
    {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
        {
            {
                std::lock_guard<std::mutex> lock(m_errorMutex);
                m_lastError = L"COM 初始化失败: " + DescribeHresult(hr);
            }
            m_running.store(false);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_errorMutex);
            m_lastError.clear();
        }

        winrt::com_ptr<IMMDeviceEnumerator> enumerator;
        winrt::com_ptr<IMMDevice> device;
        winrt::com_ptr<IAudioClient> client;
        winrt::com_ptr<IAudioCaptureClient> capture;
        WAVEFORMATEX* mixFormat = nullptr;
        HANDLE readyEvent = nullptr;

        auto cleanup = [&] {
            // IAudioCaptureClient has no Stop(); the shared stream is stopped
            // through IAudioClient.
            if (client) client->Stop();
            if (mixFormat) CoTaskMemFree(mixFormat);
            if (readyEvent) CloseHandle(readyEvent);
            CoUninitialize();
        };

        do
        {
            hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), enumerator.put_void());
            if (FAILED(hr)) break;

            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.put());
            if (FAILED(hr)) break;

            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void());
            if (FAILED(hr)) break;

            hr = client->GetMixFormat(&mixFormat);
            if (FAILED(hr) || mixFormat == nullptr) break;

            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                    AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    0, 0, mixFormat, nullptr);
            if (FAILED(hr)) break;

            readyEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (readyEvent == nullptr) break;

            hr = client->SetEventHandle(readyEvent);
            if (FAILED(hr)) break;

            hr = client->GetService(__uuidof(IAudioCaptureClient), capture.put_void());
            if (FAILED(hr)) break;

            hr = client->Start();
            if (FAILED(hr)) break;
        } while (false);

        if (FAILED(hr))
        {
            {
                std::lock_guard<std::mutex> lock(m_errorMutex);
                m_lastError = L"WASAPI 采集启动失败: " + DescribeHresult(hr);
            }
            cleanup();
            m_running.store(false);
            return;
        }

        const FormatInfo info = Describe(mixFormat);
        std::vector<float> frames;

        while (!m_stopRequested.load())
        {
            const DWORD waited = WaitForSingleObject(readyEvent, 100);
            if (waited == WAIT_TIMEOUT)
            {
                continue;
            }

            UINT32 packetFrames = 0;
            hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr))
            {
                break;
            }

            while (packetFrames != 0 && !m_stopRequested.load())
            {
                BYTE* buffer = nullptr;
                UINT32 framesAvailable = 0;
                DWORD flags = 0;

                hr = capture->GetBuffer(&buffer, &framesAvailable, &flags, nullptr, nullptr);
                if (FAILED(hr))
                {
                    break;
                }

                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 && framesAvailable > 0 && m_callback)
                {
                    ConvertToFloat(buffer, framesAvailable, info, frames);
                    m_callback(frames.data(), framesAvailable, static_cast<int>(info.channels), info.sampleRate);
                }

                capture->ReleaseBuffer(framesAvailable);

                if (FAILED(hr))
                {
                    break;
                }

                hr = capture->GetNextPacketSize(&packetFrames);
                if (FAILED(hr))
                {
                    break;
                }
            }
        }

        cleanup();
        m_running.store(false);
    }
}
