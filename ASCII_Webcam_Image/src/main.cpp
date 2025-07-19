#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <Shlwapi.h>
#include <wincodec.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <vector>
#include <algorithm>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "Mfplat.lib")
#pragma comment(lib, "Mfreadwrite.lib")
#pragma comment(lib, "Mf.lib")
#pragma comment(lib, "Mfuuid.lib")
#pragma comment(lib, "Shlwapi.lib")

const char* WINDOW_TITLE = "ASCII Webcam";
const char CLASS_NAME[] = "ASCIIWebcamClass";
const char* ASCII_CHARS = "@$#8&WM#*oahkbdpqwmZO0QLCJUYXzcvunxrjft/\\|()1{}[]?-_+~<>i!lI;:,^`'. ";
const int NUM_ASCII_LEVELS = strlen(ASCII_CHARS) - 1;
const int ASCII_WIDTH = 59;
const int ASCII_HEIGHT = 48;

HWND hwnd_button, hwnd_output;
std::atomic<bool> capturing = false;
std::atomic<bool> running = false;
std::thread captureThread, convertThread;
std::mutex bufferMutex;

IMFSourceReader* pReader = nullptr;
UINT32 width = 0, height = 0;

std::vector<BYTE> jpegData;
std::string asciiFrame;

// Converts MJPEG input buffer into a raw RGB32 buffer using WIC
bool ConvertMJPEGtoRGB32(BYTE* inputData, DWORD inputSize, std::vector<BYTE>& output, int outWidth, int outHeight) {
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    bool success = false;

    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) return false;
    if (FAILED(factory->CreateStream(&stream))) goto cleanup;
    if (FAILED(stream->InitializeFromMemory(inputData, inputSize))) goto cleanup;
    if (FAILED(factory->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder))) goto cleanup;
    if (FAILED(decoder->GetFrame(0, &frame))) goto cleanup;
    if (FAILED(factory->CreateFormatConverter(&converter))) goto cleanup;
    if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat32bppRGB, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom))) goto cleanup;

    output.resize(outWidth * outHeight * 4);
    if (FAILED(converter->CopyPixels(NULL, outWidth * 4, output.size(), output.data()))) goto cleanup;

    success = true;

cleanup:
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return success;
}

// Converts RGB image into ASCII art representation (fixed resolution)
std::string RGBtoASCII(const std::vector<BYTE>& data, int imgW, int imgH) {
    int asciiW = std::min(imgW, ASCII_WIDTH);
    int asciiH = std::min(imgH, ASCII_HEIGHT);
    int cellW = std::max(1, imgW / asciiW);
    int cellH = std::max(1, imgH / asciiH);

    std::string output;
    for (int y = 0; y < asciiH; ++y) {
        for (int x = 0; x < asciiW; ++x) {
            int sum = 0, count = 0;
            for (int cy = 0; cy < cellH; ++cy) {
                int py = y * cellH + cy;
                if (py >= imgH) continue;
                for (int cx = 0; cx < cellW; ++cx) {
                    int px = x * cellW + cx;
                    if (px >= imgW) continue;
                    int idx = (py * imgW + px) * 4;
                    if (idx + 2 >= (int)data.size()) continue;
                    BYTE r = data[idx + 0];
                    BYTE g = data[idx + 1];
                    BYTE b = data[idx + 2];
                    sum += (r + g + b) / 3;
                    count++;
                }
            }
            int avg = count ? (sum / count) : 0;
            int index = (avg * NUM_ASCII_LEVELS) / 255;
            output += ASCII_CHARS[index];
        }
        output += "\r\n";
    }
    return output;
}

// Initializes Media Foundation and selects the first MJPEG-compatible webcam
bool InitCapture() {
    if (FAILED(MFStartup(MF_VERSION))) return false;

    IMFAttributes* attr = nullptr;
    IMFActivate** devices = nullptr;
    UINT32 count = 0;

    MFCreateAttributes(&attr, 1);
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (FAILED(MFEnumDeviceSources(attr, &devices, &count)) || count == 0) return false;

    IMFMediaSource* source = nullptr;
    devices[0]->ActivateObject(IID_PPV_ARGS(&source));
    for (UINT32 i = 0; i < count; ++i) devices[i]->Release();
    CoTaskMemFree(devices);

    if (FAILED(MFCreateSourceReaderFromMediaSource(source, NULL, &pReader))) return false;
    source->Release();

    IMFMediaType* pType = nullptr;
    for (DWORD i = 0; SUCCEEDED(pReader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &pType)); ++i) {
        GUID subtype;
        pType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_MJPG) {
            pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType);
            pType->Release();
            break;
        }
        pType->Release();
    }

    IMFMediaType* pCurrent = nullptr;
    pReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrent);
    MFGetAttributeSize(pCurrent, MF_MT_FRAME_SIZE, &width, &height);
    pCurrent->Release();
    return true;
}

// Continuously reads MJPEG frames and stores raw data for decoding
void CaptureLoop() {
    while (capturing) {
        IMFSample* sample = nullptr;
        IMFMediaBuffer* buffer = nullptr;
        BYTE* pData = nullptr;
        DWORD streamIndex = 0, flags = 0;
        LONGLONG ts = 0;

        if (FAILED(pReader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &ts, &sample)) || !sample)
            continue;

        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) {
            sample->Release();
            continue;
        }

        DWORD max, cur;
        if (SUCCEEDED(buffer->Lock(&pData, &max, &cur))) {
            std::lock_guard<std::mutex> lock(bufferMutex);
            jpegData.assign(pData, pData + cur);
            buffer->Unlock();
        }

        buffer->Release();
        sample->Release();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Converts the latest MJPEG frame to ASCII and updates the UI
void ConvertLoop() {
    while (capturing) {
        std::vector<BYTE> local;
        {
            std::lock_guard<std::mutex> lock(bufferMutex);
            local = jpegData;
        }

        if (!local.empty()) {
            std::vector<BYTE> rgb;
            if (ConvertMJPEGtoRGB32(local.data(), local.size(), rgb, width, height)) {
                asciiFrame = RGBtoASCII(rgb, width, height);
                SetWindowTextA(hwnd_output, asciiFrame.c_str());
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

// Stops capture threads and releases Media Foundation
void ShutdownCapture() {
    capturing = false;
    if (captureThread.joinable()) captureThread.join();
    if (convertThread.joinable()) convertThread.join();
    if (pReader) { pReader->Release(); pReader = nullptr; }
    MFShutdown();
}

// Toggles the webcam stream (Start/Stop button)
void Toggle(HWND hwnd) {
    capturing = !capturing;
    if (capturing) {
        if (!InitCapture()) {
            MessageBoxA(hwnd, "InitCapture failed", "Error", MB_ICONERROR);
            capturing = false;
            return;
        }
        captureThread = std::thread(CaptureLoop);
        convertThread = std::thread(ConvertLoop);
        SetWindowTextA(hwnd_button, "Stop");
    }
    else {
        ShutdownCapture();
        SetWindowTextA(hwnd_button, "Start");
    }
}

// Main message handler for the application window
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        hwnd_button = CreateWindowA("BUTTON", "Start", WS_VISIBLE | WS_CHILD,
            200, 560, 100, 30, hwnd, NULL, NULL, NULL);
        hwnd_output = CreateWindowA("EDIT", "",
            WS_VISIBLE | WS_CHILD | WS_BORDER | ES_MULTILINE | ES_READONLY | ES_NOHIDESEL,
            10, 10, 480, 540, hwnd, NULL, NULL, NULL);
        SendMessage(hwnd_output, WM_SETFONT, (WPARAM)GetStockObject(OEM_FIXED_FONT), TRUE);
        return 0;
    case WM_COMMAND:
        if ((HWND)l == hwnd_button) Toggle(hwnd);
        return 0;
    case WM_DESTROY:
        ShutdownCapture();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, w, l);
}

// Entry point for the Win32 GUI application
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSA wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInst;
    wc.lpszClassName = CLASS_NAME;
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(
        0, CLASS_NAME, WINDOW_TITLE, WS_OVERLAPPEDWINDOW,     //corrupted title to lazy to fix it its 4 am i need to sleep
        CW_USEDEFAULT, CW_USEDEFAULT, 517, 640,
        NULL, NULL, hInst, NULL
    );
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    CoUninitialize();
    return 0;
}