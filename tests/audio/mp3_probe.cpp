// Feasibility probe: can Media Foundation encode PCM -> MP3 on this machine (no FFmpeg)?
// Writes 1 second of a 440 Hz tone to mp3_probe_out.mp3 via a Sink Writer with an MFAudioFormat_MP3
// output type. Prints OK + byte size, or the failing HRESULT. This decides the download engine's MP3 path.
#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

static const wchar_t* kOut = L"mp3_probe_out.mp3";

int wmain() {
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::printf("MFStartup failed 0x%08lX\n", hr);
        return 1;
    }

    const uint32_t sampleRate = 44100, channels = 2, bitsPerSample = 16;

    // Output MP3 type at 320 kbps.
    ComPtr<IMFMediaType> outType;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    outType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_MP3);
    outType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    outType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    outType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 320000 / 8);

    // Input PCM type.
    ComPtr<IMFMediaType> inType;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    inType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    inType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sampleRate);
    inType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
    inType->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, bitsPerSample);
    inType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * bitsPerSample / 8);
    inType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, sampleRate * channels * bitsPerSample / 8);

    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    hr = MFCreateSinkWriterFromURL(kOut, nullptr, attrs.Get(), &writer);
    if (FAILED(hr)) {
        std::printf("MFCreateSinkWriterFromURL failed 0x%08lX\n", hr);
        MFShutdown();
        return 1;
    }

    DWORD streamIndex = 0;
    hr = writer->AddStream(outType.Get(), &streamIndex);
    if (FAILED(hr)) {
        std::printf("AddStream(MP3) failed 0x%08lX  (no MP3 encoder MFT on this machine)\n", hr);
        MFShutdown();
        return 2;
    }
    hr = writer->SetInputMediaType(streamIndex, inType.Get(), nullptr);
    if (FAILED(hr)) {
        std::printf("SetInputMediaType(PCM) failed 0x%08lX\n", hr);
        MFShutdown();
        return 3;
    }
    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        std::printf("BeginWriting failed 0x%08lX\n", hr);
        MFShutdown();
        return 4;
    }

    // 1 second of a 440 Hz tone.
    const uint32_t frames = sampleRate;
    std::vector<int16_t> pcm(frames * channels);
    for (uint32_t i = 0; i < frames; ++i) {
        const double s = std::sin(2.0 * 3.14159265358979 * 440.0 * i / sampleRate);
        const int16_t v = static_cast<int16_t>(s * 12000);
        pcm[i * 2] = v;
        pcm[i * 2 + 1] = v;
    }
    const DWORD bytes = static_cast<DWORD>(pcm.size() * sizeof(int16_t));

    ComPtr<IMFMediaBuffer> buf;
    MFCreateMemoryBuffer(bytes, &buf);
    BYTE* dst = nullptr;
    buf->Lock(&dst, nullptr, nullptr);
    memcpy(dst, pcm.data(), bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);

    ComPtr<IMFSample> sample;
    MFCreateSample(&sample);
    sample->AddBuffer(buf.Get());
    sample->SetSampleTime(0);
    const LONGLONG durHns = 10'000'000LL; // 1 s
    sample->SetSampleDuration(durHns);

    hr = writer->WriteSample(streamIndex, sample.Get());
    if (FAILED(hr)) {
        std::printf("WriteSample failed 0x%08lX\n", hr);
        MFShutdown();
        return 5;
    }
    hr = writer->Finalize();
    if (FAILED(hr)) {
        std::printf("Finalize failed 0x%08lX\n", hr);
        MFShutdown();
        return 6;
    }
    writer.Reset();
    MFShutdown();

    // Report size.
    HANDLE f = CreateFileW(kOut, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    LARGE_INTEGER sz{};
    if (f != INVALID_HANDLE_VALUE) {
        GetFileSizeEx(f, &sz);
        CloseHandle(f);
    }
    std::printf("OK: MP3 encoding works. Wrote %s (%lld bytes)\n", "mp3_probe_out.mp3", sz.QuadPart);
    return sz.QuadPart > 0 ? 0 : 7;
}
