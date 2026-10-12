#include <windows.h>
#include <gdiplus.h>
#include <vector>
#include <iostream>
#include <fstream>
#include <cstdint>
#include <cmath>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using namespace Gdiplus;

// Helper to get CLSID of image encoder
int GetEncoderClsid(const WCHAR* format, CLSID* pClsid) {
    UINT num = 0;
    UINT size = 0;
    GetImageEncodersSize(&num, &size);
    if (size == 0) return -1;

    std::vector<BYTE> memory(size);
    ImageCodecInfo* pImageCodecInfo = reinterpret_cast<ImageCodecInfo*>(memory.data());
    GetImageEncoders(num, size, pImageCodecInfo);

    for (UINT j = 0; j < num; ++j) {
        if (wcscmp(pImageCodecInfo[j].MimeType, format) == 0) {
            *pClsid = pImageCodecInfo[j].Clsid;
            return j;
        }
    }
    return -1;
}

// Draw the Material Design QuickDNS shield logo on any Graphics context of size W x H
void RenderMaterialLogo(Graphics& g, float w, float h) {
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    // Padding around shield (approx 6% margin so it sits comfortably inside the icon canvas)
    float pad = w * 0.06f;
    float left = pad;
    float right = w - pad;
    float top = pad + h * 0.02f;
    float bottom = h - pad;
    float midX = w * 0.5f;

    // Shield outline points
    // Modern geometric shield silhouette
    GraphicsPath shieldPath;
    float cornerR = w * 0.08f;
    float shoulderY = top + h * 0.42f;

    // Top edge with rounded corners
    shieldPath.AddArc(left, top, cornerR * 2.0f, cornerR * 2.0f, 180.0f, 90.0f);
    shieldPath.AddLine(left + cornerR, top, right - cornerR, top);
    shieldPath.AddArc(right - cornerR * 2.0f, top, cornerR * 2.0f, cornerR * 2.0f, 270.0f, 90.0f);

    // Right side straight down to shoulder
    shieldPath.AddLine(right, top + cornerR, right, shoulderY);

    // Right curve down to shield bottom tip
    shieldPath.AddBezier(
        PointF(right, shoulderY),
        PointF(right, shoulderY + h * 0.28f),
        PointF(midX + w * 0.12f, bottom - h * 0.08f),
        PointF(midX, bottom)
    );

    // Left curve from bottom tip up to left shoulder
    shieldPath.AddBezier(
        PointF(midX, bottom),
        PointF(midX - w * 0.12f, bottom - h * 0.08f),
        PointF(left, shoulderY + h * 0.28f),
        PointF(left, shoulderY)
    );

    // Left side straight up to top corner
    shieldPath.AddLine(left, shoulderY, left, top + cornerR);
    shieldPath.CloseFigure();

    // 1. Drop shadow / subtle ambient elevation (Material Design style)
    {
        GraphicsPath shadowPath;
        Matrix shadowMat;
        shadowMat.Translate(0.0f, h * 0.03f);
        shadowPath.AddPath(&shieldPath, FALSE);
        shadowPath.Transform(&shadowMat);

        SolidBrush shadowBrush(Color(70, 0, 0, 0));
        g.FillPath(&shadowBrush, &shadowPath);
    }

    // 2. Base Shield Fill - Material Deep Slate
    // Left half: Slate 800 (#1E293B), Right half: Slate 900 (#0F172A) for crisp geometric duotone depth
    {
        // Clip to left side
        g.SetClip(&shieldPath);
        RectF leftHalf(0.0f, 0.0f, midX, h);
        SolidBrush leftBrush(Color(255, 30, 41, 59)); // #1E293B
        g.FillRectangle(&leftBrush, leftHalf);

        // Right half
        RectF rightHalf(midX, 0.0f, w - midX, h);
        SolidBrush rightBrush(Color(255, 15, 23, 42)); // #0F172A
        g.FillRectangle(&rightBrush, rightHalf);
        g.ResetClip();
    }

    // 3. Crisp Material Cyan Border Stroke (#38BDF8 / #00E5FF)
    float borderWidth = w * 0.045f;
    Pen borderPen(Color(255, 56, 189, 248), borderWidth);
    borderPen.SetLineJoin(LineJoinRound);
    g.DrawPath(&borderPen, &shieldPath);

    // Inner subtle inset border for precision
    {
        GraphicsPath innerPath;
        Matrix innerMat;
        float scaleF = 1.0f - (borderWidth * 1.5f / w);
        innerMat.Translate(midX, h * 0.48f);
        innerMat.Scale(scaleF, scaleF);
        innerMat.Translate(-midX, -h * 0.48f);
        innerPath.AddPath(&shieldPath, FALSE);
        innerPath.Transform(&innerMat);

        Pen innerPen(Color(80, 56, 189, 248), w * 0.008f);
        g.DrawPath(&innerPen, &innerPath);
    }

    // 4. Horizontal Pulse / Heartbeat Wave across the shield (Material Mint #34D399)
    {
        float pulseY = h * 0.52f;
        float pLeft = left + w * 0.08f;
        float pRight = right - w * 0.08f;
        float pMid = midX;
        float pW = pRight - pLeft;

        GraphicsPath pulsePath;
        std::vector<PointF> pulsePts = {
            PointF(pLeft, pulseY),
            PointF(pLeft + pW * 0.22f, pulseY),
            PointF(pLeft + pW * 0.28f, pulseY - h * 0.04f),
            PointF(pLeft + pW * 0.34f, pulseY + h * 0.05f),
            PointF(pLeft + pW * 0.38f, pulseY),
            PointF(pRight - pW * 0.38f, pulseY),
            PointF(pRight - pW * 0.34f, pulseY - h * 0.06f),
            PointF(pRight - pW * 0.28f, pulseY + h * 0.04f),
            PointF(pRight - pW * 0.22f, pulseY),
            PointF(pRight, pulseY)
        };

        pulsePath.AddLines(pulsePts.data(), static_cast<INT>(pulsePts.size()));

        Pen pulsePen(Color(200, 52, 211, 153), w * 0.022f); // Emerald #34D399
        pulsePen.SetLineJoin(LineJoinRound);
        pulsePen.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
        g.DrawPath(&pulsePen, &pulsePath);
    }

    // 5. Bold Dynamic Material Lightning Bolt in the center
    // Material Cyan #00E5FF & Vibrant Electric Gradient / Flat Planes
    {
        GraphicsPath boltPath;
        // Defined relative to center
        float bx = midX;
        float by = h * 0.47f;
        float bw = w * 0.32f;
        float bh = h * 0.54f;

        std::vector<PointF> boltPts = {
            PointF(bx + bw * 0.18f, by - bh * 0.44f), // Top right start
            PointF(bx - bw * 0.28f, by - bh * 0.02f), // Jog down-left
            PointF(bx - bw * 0.02f, by - bh * 0.02f), // Middle step right
            PointF(bx - bw * 0.20f, by + bh * 0.46f), // Bottom sharp tip
            PointF(bx + bw * 0.30f, by + bh * 0.06f), // Jog up-right
            PointF(bx + bw * 0.06f, by + bh * 0.06f), // Middle step left
            PointF(bx + bw * 0.34f, by - bh * 0.44f)  // Close back to top
        };

        boltPath.AddPolygon(boltPts.data(), static_cast<INT>(boltPts.size()));

        // Material Duotone split for the lightning bolt:
        // Left side lighter Cyan (#00E5FF), Right side electric sky (#0284C7)
        g.SetClip(&boltPath);
        RectF boltLeftHalf(0.0f, 0.0f, midX + w * 0.02f, h);
        SolidBrush boltLeftBrush(Color(255, 0, 229, 255)); // Bright Electric Cyan #00E5FF
        g.FillRectangle(&boltLeftBrush, boltLeftHalf);

        RectF boltRightHalf(midX + w * 0.02f, 0.0f, w, h);
        SolidBrush boltRightBrush(Color(255, 14, 165, 233)); // Sky 500 #0EA5E9
        g.FillRectangle(&boltRightBrush, boltRightHalf);
        g.ResetClip();

        // Crisp lightning contour
        Pen boltPen(Color(255, 255, 255, 255), w * 0.012f);
        boltPen.SetLineJoin(LineJoinMiter);
        g.DrawPath(&boltPen, &boltPath);
    }
}

// Convert 32-bit ARGB Bitmap to PNG file
bool SaveBitmapToPng(Bitmap& bmp, const std::wstring& filePath) {
    CLSID pngClsid;
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0) return false;
    Status st = bmp.Save(filePath.c_str(), &pngClsid, nullptr);
    return (st == Ok);
}

// Write multi-resolution .ico file
#pragma pack(push, 1)
struct ICONDIR {
    uint16_t idReserved;
    uint16_t idType;      // 1 for ICO
    uint16_t idCount;     // number of images
};

struct ICONDIRENTRY {
    uint8_t  bWidth;      // 0 = 256
    uint8_t  bHeight;     // 0 = 256
    uint8_t  bColorCount;
    uint8_t  bReserved;
    uint16_t wPlanes;
    uint16_t wBitCount;
    uint32_t dwBytesInRes;
    uint32_t dwImageOffset;
};
#pragma pack(pop)

bool BuildMultiResIco(const std::vector<std::pair<int, std::vector<uint8_t>>>& pngStreams, const std::wstring& outIcoPath) {
    std::ofstream out(outIcoPath.c_str(), std::ios::binary);
    if (!out.is_open()) return false;

    ICONDIR header;
    header.idReserved = 0;
    header.idType = 1;
    header.idCount = static_cast<uint16_t>(pngStreams.size());
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));

    uint32_t offset = sizeof(ICONDIR) + sizeof(ICONDIRENTRY) * header.idCount;

    for (const auto& item : pngStreams) {
        int size = item.first;
        const auto& bytes = item.second;

        ICONDIRENTRY entry;
        entry.bWidth = (size == 256) ? 0 : static_cast<uint8_t>(size);
        entry.bHeight = (size == 256) ? 0 : static_cast<uint8_t>(size);
        entry.bColorCount = 0;
        entry.bReserved = 0;
        entry.wPlanes = 1;
        entry.wBitCount = 32;
        entry.dwBytesInRes = static_cast<uint32_t>(bytes.size());
        entry.dwImageOffset = offset;

        out.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        offset += static_cast<uint32_t>(bytes.size());
    }

    for (const auto& item : pngStreams) {
        out.write(reinterpret_cast<const char*>(item.second.data()), item.second.size());
    }

    return true;
}

std::vector<uint8_t> BitmapToPngBuffer(Bitmap& bmp) {
    CLSID pngClsid;
    GetEncoderClsid(L"image/png", &pngClsid);

    IStream* pStream = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &pStream);
    if (!pStream) return {};

    bmp.Save(pStream, &pngClsid, nullptr);

    STATSTG stat;
    pStream->Stat(&stat, STATFLAG_NONAME);
    ULONG streamSize = static_cast<ULONG>(stat.cbSize.QuadPart);

    std::vector<uint8_t> buffer(streamSize);
    LARGE_INTEGER li = {};
    pStream->Seek(li, STREAM_SEEK_SET, nullptr);
    ULONG bytesRead = 0;
    pStream->Read(buffer.data(), streamSize, &bytesRead);
    pStream->Release();

    return buffer;
}

int main() {
    GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    std::cout << "Rendering Material Design QuickDNS logo..." << std::endl;

    // 1. Generate 1024x1024 master transparent PNG
    {
        Bitmap bmp1024(1024, 1024, PixelFormat32bppARGB);
        Graphics g(&bmp1024);
        g.Clear(Color(0, 0, 0, 0)); // 100% Transparent
        RenderMaterialLogo(g, 1024.0f, 1024.0f);

        if (SaveBitmapToPng(bmp1024, L"res/app_logo.png")) {
            std::cout << "[SUCCESS] Created res/app_logo.png (1024x1024 Material Design)" << std::endl;
        } else {
            std::cerr << "[ERROR] Failed to save res/app_logo.png" << std::endl;
        }
    }

    // 2. Generate 256x256 transparent PNG
    {
        Bitmap bmp256(256, 256, PixelFormat32bppARGB);
        Graphics g(&bmp256);
        g.Clear(Color(0, 0, 0, 0));
        RenderMaterialLogo(g, 256.0f, 256.0f);

        if (SaveBitmapToPng(bmp256, L"res/app_icon.png")) {
            std::cout << "[SUCCESS] Created res/app_icon.png (256x256 Material Design)" << std::endl;
        } else {
            std::cerr << "[ERROR] Failed to save res/app_icon.png" << std::endl;
        }
    }

    // 3. Generate multi-resolution .ico (256, 128, 64, 48, 32, 16)
    {
        std::vector<int> sizes = { 256, 128, 64, 48, 32, 16 };
        std::vector<std::pair<int, std::vector<uint8_t>>> icoPngs;

        for (int sz : sizes) {
            Bitmap bmpSz(sz, sz, PixelFormat32bppARGB);
            Graphics g(&bmpSz);
            g.Clear(Color(0, 0, 0, 0));
            RenderMaterialLogo(g, static_cast<float>(sz), static_cast<float>(sz));

            auto pngBytes = BitmapToPngBuffer(bmpSz);
            if (!pngBytes.empty()) {
                icoPngs.push_back({ sz, pngBytes });
            }
        }

        if (BuildMultiResIco(icoPngs, L"res/app_icon.ico")) {
            std::cout << "[SUCCESS] Created res/app_icon.ico with multi-resolution PNG stream" << std::endl;
        } else {
            std::cerr << "[ERROR] Failed to build res/app_icon.ico" << std::endl;
        }
    }

    GdiplusShutdown(gdiplusToken);
    return 0;
}
