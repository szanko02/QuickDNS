#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <gdiplus.h>
#include <iostream>
#include <vector>
#include <queue>
#include <cmath>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")

int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> memory(size);
    auto* pImageCodecInfo = reinterpret_cast<Gdiplus::ImageCodecInfo*>(memory.data());
    Gdiplus::GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return j;
        }
    }
    return -1;
}

#pragma pack(push, 2)
struct ICONDIR {
    WORD idReserved;
    WORD idType;
    WORD idCount;
};

struct ICONDIRENTRY {
    BYTE bWidth;
    BYTE bHeight;
    BYTE bColorCount;
    BYTE bReserved;
    WORD wPlanes;
    WORD wBitCount;
    DWORD dwBytesInRes;
    DWORD dwImageOffset;
};
#pragma pack(pop)

std::vector<BYTE> SaveBitmapToPngBytes(Gdiplus::Bitmap* bmp, const CLSID& pngClsid) {
    IStream* pStream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &pStream) != S_OK) return {};

    bmp->Save(pStream, &pngClsid, nullptr);

    STATSTG stat;
    pStream->Stat(&stat, STATFLAG_NONAME);
    ULONG size = static_cast<ULONG>(stat.cbSize.QuadPart);

    std::vector<BYTE> buffer(size);
    LARGE_INTEGER liZero{};
    pStream->Seek(liZero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    pStream->Read(buffer.data(), size, &read);
    pStream->Release();

    return buffer;
}

int main() {
    ULONG_PTR token = 0;
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&token, &input, nullptr) != Gdiplus::Ok) {
        std::cerr << "Failed to init GDI+" << std::endl;
        return 1;
    }

    const wchar_t* srcPath = L"C:\\Users\\SWEETY\\.gemini\\antigravity-cli\\brain\\b586bf1c-c888-4572-903c-9f6fd6bafed3\\quickdns_shield_icon_1791764720061.jpg";
    const wchar_t* dstPngLogo = L"C:\\Users\\SWEETY\\QuickDNS\\res\\app_logo.png";
    const wchar_t* dstPngIcon = L"C:\\Users\\SWEETY\\QuickDNS\\res\\app_icon.png";
    const wchar_t* dstIco = L"C:\\Users\\SWEETY\\QuickDNS\\res\\app_icon.ico";

    Gdiplus::Bitmap* srcBmp = Gdiplus::Bitmap::FromFile(srcPath);
    if (!srcBmp || srcBmp->GetLastStatus() != Gdiplus::Ok) {
        std::cerr << "Failed to load source image: " << srcBmp->GetLastStatus() << std::endl;
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }

    int width = srcBmp->GetWidth();
    int height = srcBmp->GetHeight();
    std::cout << "Loaded source image: " << width << "x" << height << std::endl;

    Gdiplus::Rect rect(0, 0, width, height);
    Gdiplus::BitmapData bmpData;
    srcBmp->LockBits(&rect, Gdiplus::ImageLockModeRead, PixelFormat32bppARGB, &bmpData);

    const auto* srcPixels = reinterpret_cast<const uint32_t*>(bmpData.Scan0);
    int strideElements = bmpData.Stride / 4;

    // 1. Flood fill from outer image borders to distinguish outside background from inside shield
    std::vector<uint8_t> isOutside(width * height, 0);
    std::queue<std::pair<int, int>> q;

    auto pushBorder = [&](int x, int y) {
        if (!isOutside[y * width + x]) {
            isOutside[y * width + x] = 1;
            q.push({x, y});
        }
    };

    for (int x = 0; x < width; ++x) {
        pushBorder(x, 0);
        pushBorder(x, height - 1);
    }
    for (int y = 0; y < height; ++y) {
        pushBorder(0, y);
        pushBorder(width - 1, y);
    }

    const int dx[4] = { 1, -1, 0, 0 };
    const int dy[4] = { 0, 0, 1, -1 };

    while (!q.empty()) {
        auto [cx, cy] = q.front();
        q.pop();

        for (int i = 0; i < 4; ++i) {
            int nx = cx + dx[i];
            int ny = cy + dy[i];

            if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
                if (!isOutside[ny * width + nx]) {
                    uint32_t c = srcPixels[ny * strideElements + nx];
                    double b = static_cast<double>(c & 0xFF);
                    double g = static_cast<double>((c >> 8) & 0xFF);
                    double r = static_cast<double>((c >> 16) & 0xFF);
                    double maxVal = std::max({ r, g, b });
                    double chroma = (g + b) * 0.5 - r;

                    // A pixel is a shield/lightning barrier if it has bright metallic reflection or strong neon chroma
                    bool isBarrier = (maxVal > 115.0) || (chroma > 25.0);

                    if (!isBarrier) {
                        isOutside[ny * width + nx] = 1;
                        q.push({nx, ny});
                    }
                }
            }
        }
    }

    // 2. Generate destination 32-bit ARGB pixels
    Gdiplus::Bitmap dstBmp(width, height, PixelFormat32bppARGB);
    Gdiplus::BitmapData dstData;
    dstBmp.LockBits(&rect, Gdiplus::ImageLockModeWrite, PixelFormat32bppARGB, &dstData);
    auto* dstPixels = reinterpret_cast<uint32_t*>(dstData.Scan0);
    int dstStrideElements = dstData.Stride / 4;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            uint32_t c = srcPixels[y * strideElements + x];
            double b = static_cast<double>(c & 0xFF);
            double g = static_cast<double>((c >> 8) & 0xFF);
            double r = static_cast<double>((c >> 16) & 0xFF);
            double maxVal = std::max({ r, g, b });
            double chroma = (g + b) * 0.5 - r;

            if (!isOutside[y * width + x]) {
                // INSIDE the shield or on the bright metallic rim: 100% solid
                dstPixels[y * dstStrideElements + x] = c | 0xFF000000;
            } else {
                // OUTSIDE the shield: background or pulse glow
                if (chroma >= 14.0 && (g > 60.0 || b > 60.0)) {
                    // Vibrant neon glow on pulse wave
                    double alpha = std::clamp((chroma - 10.0) / 35.0, 0.0, 1.0);
                    uint8_t a = static_cast<uint8_t>(alpha * 255.0);

                    double invA = (alpha > 0.01) ? (1.0 / alpha) : 1.0;
                    uint8_t outR = static_cast<uint8_t>(std::clamp(r * invA, 0.0, 255.0));
                    uint8_t outG = static_cast<uint8_t>(std::clamp(g * invA, 0.0, 255.0));
                    uint8_t outB = static_cast<uint8_t>(std::clamp(b * invA, 0.0, 255.0));

                    dstPixels[y * dstStrideElements + x] = (static_cast<uint32_t>(a) << 24) |
                                                           (static_cast<uint32_t>(outR) << 16) |
                                                           (static_cast<uint32_t>(outG) << 8) |
                                                           static_cast<uint32_t>(outB);
                } else if (maxVal > 70.0) {
                    // Outer rim anti-aliasing edge
                    double alpha = std::clamp((maxVal - 70.0) / 45.0, 0.0, 1.0);
                    uint8_t a = static_cast<uint8_t>(alpha * 255.0);

                    dstPixels[y * dstStrideElements + x] = (static_cast<uint32_t>(a) << 24) |
                                                           (static_cast<uint32_t>(r) << 16) |
                                                           (static_cast<uint32_t>(g) << 8) |
                                                           static_cast<uint32_t>(b);
                } else {
                    // Pure background
                    dstPixels[y * dstStrideElements + x] = 0;
                }
            }
        }
    }

    srcBmp->UnlockBits(&bmpData);
    delete srcBmp;

    dstBmp.UnlockBits(&dstData);

    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0) {
        std::cerr << "Failed to find PNG encoder" << std::endl;
        Gdiplus::GdiplusShutdown(token);
        return 1;
    }

    // Save 1024x1024 transparent PNG logo for README and documentation
    dstBmp.Save(dstPngLogo, &pngClsid, nullptr);
    std::cout << "Saved: " << "res/app_logo.png" << std::endl;

    // Save 256x256 transparent PNG icon
    Gdiplus::Bitmap bmp256(256, 256, PixelFormat32bppARGB);
    {
        Gdiplus::Graphics g(&bmp256);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(&dstBmp, 0, 0, 256, 256);
    }
    bmp256.Save(dstPngIcon, &pngClsid, nullptr);
    std::cout << "Saved: " << "res/app_icon.png" << std::endl;

    // Generate multi-resolution .ICO containing 256, 128, 64, 48, 32, 16 sizes
    std::vector<int> iconSizes = { 256, 128, 64, 48, 32, 16 };
    std::vector<std::vector<BYTE>> pngStreams;
    for (int sz : iconSizes) {
        Gdiplus::Bitmap subBmp(sz, sz, PixelFormat32bppARGB);
        Gdiplus::Graphics g(&subBmp);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        g.SetSmoothingMode(Gdiplus::SmoothingModeHighQuality);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHighQuality);
        g.DrawImage(&dstBmp, 0, 0, sz, sz);
        pngStreams.push_back(SaveBitmapToPngBytes(&subBmp, pngClsid));
    }

    // Build .ICO file structure
    ICONDIR iconDir{};
    iconDir.idReserved = 0;
    iconDir.idType = 1; // 1 = Icon
    iconDir.idCount = static_cast<WORD>(iconSizes.size());

    DWORD offset = sizeof(ICONDIR) + sizeof(ICONDIRENTRY) * iconSizes.size();
    std::vector<ICONDIRENTRY> entries(iconSizes.size());

    for (size_t i = 0; i < iconSizes.size(); ++i) {
        int sz = iconSizes[i];
        entries[i].bWidth = (sz >= 256) ? 0 : static_cast<BYTE>(sz);
        entries[i].bHeight = (sz >= 256) ? 0 : static_cast<BYTE>(sz);
        entries[i].bColorCount = 0;
        entries[i].bReserved = 0;
        entries[i].wPlanes = 1;
        entries[i].wBitCount = 32;
        entries[i].dwBytesInRes = static_cast<DWORD>(pngStreams[i].size());
        entries[i].dwImageOffset = offset;
        offset += static_cast<DWORD>(pngStreams[i].size());
    }

    FILE* fp = nullptr;
    if (_wfopen_s(&fp, dstIco, L"wb") == 0 && fp) {
        fwrite(&iconDir, sizeof(iconDir), 1, fp);
        fwrite(entries.data(), sizeof(ICONDIRENTRY), entries.size(), fp);
        for (const auto& s : pngStreams) {
            fwrite(s.data(), 1, s.size(), fp);
        }
        fclose(fp);
        std::cout << "Successfully saved multi-resolution transparent .ICO: res/app_icon.ico" << std::endl;
    }

    Gdiplus::GdiplusShutdown(token);
    return 0;
}
