#include "video_windows.hpp"

#include "nso_album_sync/windows_compat.hpp"
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace nso {
namespace {

using Microsoft::WRL::ComPtr;
constexpr LONGLONG kTicksPerSecond = 10'000'000;  // Media Foundation uses 100 ns units

void check(HRESULT result, const char* step) {
    if (FAILED(result)) {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
        throw std::runtime_error(std::string(step) + " failed (" + code + ")");
    }
}

struct MediaFoundation {
    HRESULT com;
    HRESULT started;
    MediaFoundation() : com(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), started(MFStartup(MF_VERSION, MFSTARTUP_FULL)) {
        check(started, "Starting Windows media support");
    }
    ~MediaFoundation() {
        if (SUCCEEDED(started)) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
    }
    MediaFoundation(const MediaFoundation&) = delete;
    MediaFoundation& operator=(const MediaFoundation&) = delete;
};

struct Format {
    UINT32 width = 0, height = 0;          // Visible picture size
    UINT32 rate_num = 30, rate_den = 1;
    LONGLONG duration = 0;
};

ComPtr<IMFSourceReader> open_reader(const std::filesystem::path& video) {
    ComPtr<IMFAttributes> attributes;
    check(MFCreateAttributes(&attributes, 1), "Preparing the video reader");
    // Lets the reader convert to RGB and scale or crop the picture for us.
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    check(MFCreateSourceReaderFromURL(video.c_str(), attributes.Get(), &reader), "Opening the video");
    return reader;
}

Format read_format(IMFSourceReader* reader) {
    Format format;
    ComPtr<IMFMediaType> native;
    check(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &native), "Reading the video format");
    MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &format.width, &format.height);
    MFVideoArea area{};
    UINT32 size = 0;
    if (SUCCEEDED(native->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), &size)) &&
        size == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
        format.width = static_cast<UINT32>(area.Area.cx);
        format.height = static_cast<UINT32>(area.Area.cy);
    }
    if (FAILED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &format.rate_num, &format.rate_den)) ||
        !format.rate_num || !format.rate_den) {
        format.rate_num = 30;
        format.rate_den = 1;
    }
    PROPVARIANT duration;
    PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &duration))) {
        format.duration = static_cast<LONGLONG>(duration.uhVal.QuadPart);
    }
    PropVariantClear(&duration);
    if (!format.width || !format.height) throw std::runtime_error("The video has no picture");
    return format;
}

// Asks the reader for top-down 32-bit RGB frames at the visible size, falling
// back to the decoder's own frame size if that size isn't accepted.
ComPtr<IMFMediaType> use_rgb32(IMFSourceReader* reader, UINT32 width, UINT32 height) {
    const auto request = [&](bool sized) {
        ComPtr<IMFMediaType> type;
        check(MFCreateMediaType(&type), "Preparing the video format");
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (sized) MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
        return reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr, type.Get());
    };
    if (FAILED(request(true))) check(request(false), "Decoding the video");
    ComPtr<IMFMediaType> current;
    check(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &current), "Decoding the video");
    return current;
}

void seek(IMFSourceReader* reader, LONGLONG position) {
    PROPVARIANT value;
    PropVariantInit(&value);
    value.vt = VT_I8;
    value.hVal.QuadPart = position;
    check(reader->SetCurrentPosition(GUID_NULL, value), "Seeking in the video");
}

LONGLONG ticks(double seconds) {
    return static_cast<LONGLONG>(std::llround(seconds * kTicksPerSecond));
}

void throw_if_cancelled(const std::atomic<bool>& cancelled) {
    if (cancelled.load()) throw std::runtime_error("Cancelled");
}

// Runs `write` and deletes a partial output if it fails or is cancelled.
template <typename Write>
void writing(const std::filesystem::path& output, Write write) {
    try {
        write();
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(output, ignored);
        throw;
    }
}

}  // namespace

std::optional<VideoInfo> probe_video(const std::filesystem::path& video) {
    try {
        MediaFoundation media;
        const auto reader = open_reader(video);
        const auto format = read_format(reader.Get());
        return VideoInfo{static_cast<double>(format.duration) / kTicksPerSecond, format.width, format.height};
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void export_video_clip(const std::filesystem::path& video, const std::filesystem::path& output,
                       double start_seconds, double end_seconds, const std::atomic<bool>& cancelled) {
    MediaFoundation media;
    writing(output, [&] {
        const auto reader = open_reader(video);
        const auto format = read_format(reader.Get());
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        const auto video_in = use_rgb32(reader.Get(), format.width, format.height);
        UINT32 width = 0, height = 0;
        MFGetAttributeSize(video_in.Get(), MF_MT_FRAME_SIZE, &width, &height);

        // Sound is kept when it's in a format Windows' AAC encoder accepts.
        ComPtr<IMFMediaType> audio_in;
        {
            ComPtr<IMFMediaType> native;
            if (SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &native))) {
                const auto rate = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
                const auto channels = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
                ComPtr<IMFMediaType> pcm;
                if ((rate == 44100 || rate == 48000) && (channels == 1 || channels == 2) &&
                    SUCCEEDED(MFCreateMediaType(&pcm))) {
                    pcm->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
                    pcm->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
                    pcm->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
                    pcm->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
                    pcm->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
                    if (SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, pcm.Get())) &&
                        SUCCEEDED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE))) {
                        reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &audio_in);
                    }
                }
            }
        }

        ComPtr<IMFAttributes> writer_attributes;
        check(MFCreateAttributes(&writer_attributes, 1), "Preparing the clip");
        writer_attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        ComPtr<IMFSinkWriter> writer;
        check(MFCreateSinkWriterFromURL(output.c_str(), nullptr, writer_attributes.Get(), &writer), "Creating the clip");

        ComPtr<IMFMediaType> video_out;
        check(MFCreateMediaType(&video_out), "Preparing the clip");
        const double fps = static_cast<double>(format.rate_num) / format.rate_den;
        const auto bitrate = static_cast<UINT32>(std::clamp(width * height * fps * 0.12, 2'000'000.0, 24'000'000.0));
        video_out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        video_out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        video_out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        video_out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(video_out.Get(), MF_MT_FRAME_SIZE, width, height);
        MFSetAttributeRatio(video_out.Get(), MF_MT_FRAME_RATE, format.rate_num, format.rate_den);
        MFSetAttributeRatio(video_out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        DWORD video_stream = 0;
        check(writer->AddStream(video_out.Get(), &video_stream), "Setting up the H.264 encoder");
        check(writer->SetInputMediaType(video_stream, video_in.Get(), nullptr), "Setting up the H.264 encoder");

        DWORD audio_stream = 0;
        if (audio_in) {
            ComPtr<IMFMediaType> audio_out;
            check(MFCreateMediaType(&audio_out), "Preparing the clip");
            audio_out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            audio_out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            audio_out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            audio_out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, MFGetAttributeUINT32(audio_in.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000));
            audio_out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, MFGetAttributeUINT32(audio_in.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2));
            audio_out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbps
            check(writer->AddStream(audio_out.Get(), &audio_stream), "Setting up the AAC encoder");
            check(writer->SetInputMediaType(audio_stream, audio_in.Get(), nullptr), "Setting up the AAC encoder");
        }
        check(writer->BeginWriting(), "Starting the clip");

        const auto start = ticks(start_seconds), end = ticks(end_seconds);
        seek(reader.Get(), start);
        bool video_done = false, audio_done = !audio_in;
        int frames = 0;
        while (!video_done || !audio_done) {
            throw_if_cancelled(cancelled);
            DWORD index = 0, flags = 0;
            LONGLONG time = 0;
            ComPtr<IMFSample> sample;
            check(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &index, &flags, &time, &sample),
                  "Reading the video");
            ComPtr<IMFMediaType> type;
            GUID major{};
            if (FAILED(reader->GetCurrentMediaType(index, &type)) || FAILED(type->GetGUID(MF_MT_MAJOR_TYPE, &major))) continue;
            const bool is_video = major == MFMediaType_Video;
            bool& done = is_video ? video_done : audio_done;
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                done = true;
                continue;
            }
            if (!sample || time < start) continue;
            if (time >= end) {
                done = true;
                reader->SetStreamSelection(index, FALSE);
                continue;
            }
            sample->SetSampleTime(time - start);
            check(writer->WriteSample(is_video ? video_stream : audio_stream, sample.Get()), "Encoding the clip");
            if (is_video) ++frames;
        }
        if (!frames) throw std::runtime_error("There's no video in that part of the clip");
        check(writer->Finalize(), "Finishing the clip");
    });
}

void export_video_gif(const std::filesystem::path& video, const std::filesystem::path& output,
                      double start_seconds, double end_seconds, std::uint32_t target_width, double fps,
                      const std::atomic<bool>& cancelled) {
    MediaFoundation media;
    writing(output, [&] {
        const auto reader = open_reader(video);
        const auto format = read_format(reader.Get());
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
        const auto rgb = use_rgb32(reader.Get(), format.width, format.height);
        UINT32 frame_width = 0, frame_height = 0;
        MFGetAttributeSize(rgb.Get(), MF_MT_FRAME_SIZE, &frame_width, &frame_height);
        const UINT32 visible_width = std::min(format.width, frame_width);
        const UINT32 visible_height = std::min(format.height, frame_height);
        LONG default_stride = 0;
        UINT32 stored_stride = 0;
        if (SUCCEEDED(rgb->GetUINT32(MF_MT_DEFAULT_STRIDE, &stored_stride))) default_stride = static_cast<LONG>(stored_stride);
        else MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, frame_width, &default_stride);

        const UINT32 width = std::min(target_width, visible_width);
        const UINT32 height = std::max<UINT32>(2, static_cast<UINT32>(std::lround(
            static_cast<double>(visible_height) * width / visible_width)));

        ComPtr<IWICImagingFactory> factory;
        check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "Starting the GIF encoder");
        ComPtr<IWICStream> stream;
        check(factory->CreateStream(&stream), "Creating the GIF");
        check(stream->InitializeFromFilename(output.c_str(), GENERIC_WRITE), "Creating the GIF");
        ComPtr<IWICBitmapEncoder> encoder;
        check(factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder), "Starting the GIF encoder");
        check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Starting the GIF encoder");
        {
            // Loop forever (NETSCAPE2.0 extension) and declare the canvas size.
            ComPtr<IWICMetadataQueryWriter> metadata;
            if (SUCCEEDED(encoder->GetMetadataQueryWriter(&metadata))) {
                unsigned char application[] = "NETSCAPE2.0";
                unsigned char loop[] = {3, 1, 0, 0, 0};
                PROPVARIANT value;
                PropVariantInit(&value);
                value.vt = VT_UI1 | VT_VECTOR;
                value.caub.cElems = 11;
                value.caub.pElems = application;
                metadata->SetMetadataByName(L"/appext/Application", &value);
                value.caub.cElems = sizeof(loop);
                value.caub.pElems = loop;
                metadata->SetMetadataByName(L"/appext/Data", &value);
                PropVariantInit(&value);
                value.vt = VT_UI2;
                value.uiVal = static_cast<USHORT>(width);
                metadata->SetMetadataByName(L"/logscrdesc/Width", &value);
                value.uiVal = static_cast<USHORT>(height);
                metadata->SetMetadataByName(L"/logscrdesc/Height", &value);
            }
        }

        const auto start = ticks(start_seconds), end = ticks(end_seconds);
        const auto step = ticks(1.0 / fps);
        const auto delay = static_cast<USHORT>(std::max(2L, std::lround(100.0 / fps)));  // hundredths of a second
        seek(reader.Get(), start);
        LONGLONG next_frame = start;
        std::vector<BYTE> pixels(static_cast<std::size_t>(frame_width) * visible_height * 4);
        int frames = 0;
        for (;;) {
            throw_if_cancelled(cancelled);
            DWORD index = 0, flags = 0;
            LONGLONG time = 0;
            ComPtr<IMFSample> sample;
            check(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &index, &flags, &time, &sample),
                  "Reading the video");
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
            if (!sample || time < next_frame) continue;
            if (time >= end) break;
            while (next_frame <= time) next_frame += step;

            // Copy the frame top-down, whichever way the decoder stores it.
            ComPtr<IMFMediaBuffer> buffer;
            check(sample->ConvertToContiguousBuffer(&buffer), "Reading a frame");
            ComPtr<IMF2DBuffer> buffer2d;
            BYTE* first_row = nullptr;
            LONG pitch = 0;
            bool locked2d = SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&first_row, &pitch));
            BYTE* raw = nullptr;
            if (!locked2d) {
                DWORD length = 0;
                check(buffer->Lock(&raw, nullptr, &length), "Reading a frame");
                pitch = default_stride;
                first_row = pitch < 0 ? raw + static_cast<std::ptrdiff_t>(-pitch) * (frame_height - 1) : raw;
            }
            for (UINT32 row = 0; row < visible_height; ++row) {
                std::memcpy(pixels.data() + static_cast<std::size_t>(row) * frame_width * 4,
                            first_row + static_cast<std::ptrdiff_t>(row) * pitch, static_cast<std::size_t>(frame_width) * 4);
            }
            if (locked2d) buffer2d->Unlock2D();
            else buffer->Unlock();

            ComPtr<IWICBitmap> bitmap;
            check(factory->CreateBitmapFromMemory(frame_width, visible_height, GUID_WICPixelFormat32bppBGR,
                frame_width * 4, static_cast<UINT>(pixels.size()), pixels.data(), &bitmap), "Converting a frame");
            ComPtr<IWICBitmapClipper> clipper;
            const WICRect visible{0, 0, static_cast<INT>(visible_width), static_cast<INT>(visible_height)};
            check(factory->CreateBitmapClipper(&clipper), "Converting a frame");
            check(clipper->Initialize(bitmap.Get(), &visible), "Converting a frame");
            ComPtr<IWICBitmapScaler> scaler;
            check(factory->CreateBitmapScaler(&scaler), "Scaling a frame");
            check(scaler->Initialize(clipper.Get(), width, height, WICBitmapInterpolationModeFant), "Scaling a frame");
            ComPtr<IWICPalette> palette;
            check(factory->CreatePalette(&palette), "Choosing colours");
            check(palette->InitializeFromBitmap(scaler.Get(), 256, FALSE), "Choosing colours");
            ComPtr<IWICFormatConverter> indexed;
            check(factory->CreateFormatConverter(&indexed), "Choosing colours");
            check(indexed->Initialize(scaler.Get(), GUID_WICPixelFormat8bppIndexed, WICBitmapDitherTypeErrorDiffusion,
                palette.Get(), 0, WICBitmapPaletteTypeCustom), "Choosing colours");

            ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2> properties;
            check(encoder->CreateNewFrame(&frame, &properties), "Adding a frame");
            check(frame->Initialize(properties.Get()), "Adding a frame");
            check(frame->SetSize(width, height), "Adding a frame");
            WICPixelFormatGUID pixel_format = GUID_WICPixelFormat8bppIndexed;
            check(frame->SetPixelFormat(&pixel_format), "Adding a frame");
            check(frame->SetPalette(palette.Get()), "Adding a frame");
            ComPtr<IWICMetadataQueryWriter> frame_metadata;
            if (SUCCEEDED(frame->GetMetadataQueryWriter(&frame_metadata))) {
                PROPVARIANT value;
                PropVariantInit(&value);
                value.vt = VT_UI2;
                value.uiVal = delay;
                frame_metadata->SetMetadataByName(L"/grctlext/Delay", &value);
            }
            check(frame->WriteSource(indexed.Get(), nullptr), "Adding a frame");
            check(frame->Commit(), "Adding a frame");
            ++frames;
        }
        if (!frames) throw std::runtime_error("There's no video in that part of the clip");
        check(encoder->Commit(), "Finishing the GIF");
    });
}

}  // namespace nso
