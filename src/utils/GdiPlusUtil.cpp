/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "BaseUtil.h"
#include <wincodec.h>
using namespace Gdiplus;
#include "GdiPlusUtil.h"

#include "ByteReader.h"
#include "TgaReader.h"
#include "WinUtil.h"

// Get width of each character and add them up.
// Doesn't seem to be any different than MeasureTextAccurate() i.e. it still
// underreports the width
RectF MeasureTextAccurate2(Graphics *g, Font *f, const WCHAR *s, size_t len)
{
    CrashIf(0 == len);
    FixedArray<Region, 1024> regionBuf(len);
    Region *r = regionBuf.Get();
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);
    RectF layoutRect;
    FixedArray<CharacterRange, 1024> charRangesBuf(len);
    CharacterRange *charRanges = charRangesBuf.Get();
    for (size_t i = 0; i < len; i++) {
        charRanges[i].First = i;
        charRanges[i].Length = 1;
    }
    sf.SetMeasurableCharacterRanges(len, charRanges);
    Status status = g->MeasureCharacterRanges(s, len, f, layoutRect, &sf, len, r);
    CrashIf(status != Ok);
    RectF bbox;
    REAL maxDy = 0;
    REAL totalDx = 0;
    for (size_t i = 0; i < len; i++) {
        r[i].GetBounds(&bbox, g);
        if (bbox.Height > maxDy)
            maxDy = bbox.Height;
        totalDx += bbox.Width;
    }
    bbox.Width = totalDx;
    bbox.Height = maxDy;
    return bbox;
}

// note: gdi+ seems to under-report the width, the longer the text, the
// bigger the difference. I'm trying to correct for that with those magic values
#define PER_CHAR_DX_ADJUST .2f
#define PER_STR_DX_ADJUST  1.f

// http://www.codeproject.com/KB/GDI-plus/measurestring.aspx
RectF MeasureTextAccurate(Graphics *g, Font *f, const WCHAR *s, size_t len)
{
    if (0 == len)
        return RectF(0, 0, 0, 0); // TODO: should set height to font's height
    // note: frankly, I don't see a difference between those StringFormat variations
    StringFormat sf(StringFormat::GenericTypographic());
    sf.SetFormatFlags(sf.GetFormatFlags() | StringFormatFlagsMeasureTrailingSpaces);
    //StringFormat sf(StringFormat::GenericDefault());
    //StringFormat sf;
    RectF layoutRect;
    CharacterRange cr(0, len);
    sf.SetMeasurableCharacterRanges(1, &cr);
    Region r;
    Status status = g->MeasureCharacterRanges(s, len, f, layoutRect, &sf, 1, &r);
    CrashIf(status != Ok);
    RectF bbox;
    r.GetBounds(&bbox, g);
    if (bbox.Width != 0)
        bbox.Width += PER_STR_DX_ADJUST + (PER_CHAR_DX_ADJUST * (float)len);
    return bbox;
}

// this usually reports size that is too large
RectF MeasureTextStandard(Graphics *g, Font *f, const WCHAR *s, size_t len)
{
    RectF bbox;
    PointF pz(0,0);
    g->MeasureString(s, len, f, pz, &bbox);
    return bbox;
}

RectF MeasureTextQuick(Graphics *g, Font *f, const WCHAR *s, size_t len)
{
    static Vec<Font *> fontCache;
    static Vec<bool> fixCache;

    RectF bbox;
    g->MeasureString(s, len, f, PointF(0, 0), &bbox);
    int idx = fontCache.Find(f);
    if (-1 == idx) {
        LOGFONTW lfw;
        Status ok = f->GetLogFontW(g, &lfw);
        bool isItalicOrMonospace = Ok != ok || lfw.lfItalic ||
                                   str::Eq(lfw.lfFaceName, L"Courier New") ||
                                   str::Find(lfw.lfFaceName, L"Consol") ||
                                   str::EndsWith(lfw.lfFaceName, L"Mono") ||
                                   str::EndsWith(lfw.lfFaceName, L"Typewriter");
        fontCache.Append(f);
        fixCache.Append(isItalicOrMonospace);
        idx = (int)fontCache.Count() - 1;
    }
    // most documents look good enough with these adjustments
    if (!fixCache.At(idx)) {
        REAL correct = 0;
        for (size_t i = 0; i < len; i++) {
            switch (s[i]) {
            case 'i': case 'l': correct += 0.2f; break;
            case 't': case 'f': case 'I': correct += 0.1f; break;
            case '.': case ',': case '!': correct += 0.1f; break;
            }
        }
        bbox.Width *= (1.0f - correct / len) * 0.99f;
    }
    bbox.Height *= 0.95f;
    return bbox;
}

RectF MeasureText(Graphics *g, Font *f, const WCHAR *s, size_t len, TextMeasureAlgorithm algo)
{
    if (-1 == len)
        len = str::Len(s);
    if (algo)
        return algo(g, f, s, len);
    //RectF bbox = MeasureTextStandard(g, f, s, len);
    RectF bbox = MeasureTextAccurate(g, f, s, len);
    //RectF bbox = MeasureTextAccurate2(g, f, s, len);
    return bbox;
}

// returns number of characters of string s that fits in a given width dx
// note: could be speed up a bit because in our use case we already know
// the width of the whole string so we could supply it to the function, but
// this shouldn't happen often, so that's fine. It's also possible that
// a smarter approach is possible, but this usually only does 3 MeasureText
// calls, so it's not that bad
int StringLenForWidth(Graphics *g, Font *f, const WCHAR *s, size_t len, float dx, TextMeasureAlgorithm algo)
{
    RectF r = MeasureText(g, f, s, len, algo);
    if (r.Width <= dx)
        return len;
    // make the best guess of the length that fits
    size_t n = (int)((dx / r.Width) * (float)len);
    CrashIf((0 == n) || (n > len));
    r = MeasureText(g, f, s, n, algo);
    // find the length len of s that fits within dx iff width of len+1 exceeds dx
    int dir = 1; // increasing length
    if (r.Width > dx)
        dir = -1; // decreasing length
    for (;;) {
        n += dir;
        r = MeasureText(g, f, s, n, algo);
        if (1 == dir) {
            // if advancing length, we know that previous string did fit, so if
            // the new one doesn't fit, the previous length was the right one
            if (r.Width > dx)
                return n - 1;
        } else {
            // if decreasing length, we know that previous string didn't fit, so if
            // the one one fits, it's of the correct length
            if (r.Width < dx)
                return n;
        }
    }
}

// TODO: not quite sure why spaceDx1 != spaceDx2, using spaceDx2 because
// is smaller and looks as better spacing to me
REAL GetSpaceDx(Graphics *g, Font *f, TextMeasureAlgorithm algo)
{
    RectF bbox;
#if 0
    bbox = MeasureText(g, f, L" ", 1, algo);
    REAL spaceDx1 = bbox.Width;
    return spaceDx1;
#else
    // this method seems to return (much) smaller size that measuring
    // the space itself
    bbox = MeasureText(g, f, L"wa", 2, algo);
    REAL l1 = bbox.Width;
    bbox = MeasureText(g, f, L"w a", 3, algo);
    REAL l2 = bbox.Width;
    REAL spaceDx2 = l2 - l1;
    return spaceDx2;
#endif
}

#define COL_CLOSE_X RGB(0xa0, 0xa0, 0xa0)
#define COL_CLOSE_X_HOVER RGB(0xf9, 0xeb, 0xeb)  // white-ish
#define COL_CLOSE_HOVER_BG RGB(0xC1, 0x35, 0x35) // red-ish

// Draws the 'x' close button in regular state or onhover state
// Tries to mimic visual style of Chrome tab close button
void DrawCloseButton(DRAWITEMSTRUCT *dis)
{
    RectI r(RectI::FromRECT(dis->rcItem));
    ScopedMem<TCHAR> s(win::GetText(dis->hwndItem));
    bool onHover = str::Eq(s, BUTTON_HOVER_TEXT);

    Graphics g(dis->hDC);
    g.SetCompositingQuality(CompositingQualityHighQuality);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetPageUnit(UnitPixel);

    Color c;
    c.SetFromCOLORREF(GetSysColor(COLOR_BTNFACE)); // hoping this is always the right color
    SolidBrush bgBrush(c);
    g.FillRectangle(&bgBrush, r.x, r.y, r.dx, r.dy);

    // in onhover state, background is a red-ish circle
    if (onHover) {
        c.SetFromCOLORREF(COL_CLOSE_HOVER_BG);
        SolidBrush b(c);
        g.FillEllipse(&b, r.x, r.y, r.dx-2, r.dy-2);
    }

    // draw 'x'
    c.SetFromCOLORREF(onHover ? COL_CLOSE_X_HOVER : COL_CLOSE_X);
    Pen p(c, 2);
    if (onHover) {
        g.DrawLine(&p, Point(4,      4), Point(r.dx-6, r.dy-6));
        g.DrawLine(&p, Point(r.dx-6, 4), Point(4,      r.dy-6));
    } else {
        g.DrawLine(&p, Point(4,      5), Point(r.dx-6, r.dy-5));
        g.DrawLine(&p, Point(r.dx-6, 5), Point(4,      r.dy-5));
    }
}

void GetBaseTransform(Matrix& m, RectF pageRect, float zoom, int rotation)
{
    rotation = rotation % 360;
    if (rotation < 0) rotation = rotation + 360;
    if (90 == rotation)
        m.Translate(0, -pageRect.Height, MatrixOrderAppend);
    else if (180 == rotation)
        m.Translate(-pageRect.Width, -pageRect.Height, MatrixOrderAppend);
    else if (270 == rotation)
        m.Translate(-pageRect.Width, 0, MatrixOrderAppend);
    else if (0 == rotation)
        m.Translate(0, 0, MatrixOrderAppend);
    else
        CrashIf(true);

    m.Scale(zoom, zoom, MatrixOrderAppend);
    m.Rotate((REAL)rotation, MatrixOrderAppend);
}

Bitmap *ImageFromHdPhotoStream(IStream *stream)
{
    ScopedCom com;

#define HR(hr) if (FAILED(hr)) return NULL;
    ScopedComPtr<IWICImagingFactory> pFactory;
    HR(CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_ALL,
                        IID_IWICImagingFactory, (void **)&pFactory));
    ScopedComPtr<IWICBitmapDecoder> pDecoder;
    HR(pFactory->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnDemand,
                                         &pDecoder));
    ScopedComPtr<IWICBitmapFrameDecode> srcFrame;
    HR(pDecoder->GetFrame(0, &srcFrame));
    ScopedComPtr<IWICFormatConverter> pConverter;
    HR(pFactory->CreateFormatConverter(&pConverter));
    HR(pConverter->Initialize(srcFrame, GUID_WICPixelFormat32bppBGRA,
                              WICBitmapDitherTypeNone, NULL, 0.f, WICBitmapPaletteTypeCustom));

    UINT w, h;
    HR(pConverter->GetSize(&w, &h));
    double xres, yres;
    HR(pConverter->GetResolution(&xres, &yres));
    Bitmap bmp(w, h, PixelFormat32bppARGB);
    BitmapData bmpData;
    Status ok = bmp.LockBits(&Rect(0, 0, w, h), ImageLockModeWrite, PixelFormat32bppARGB, &bmpData);
    if (ok != Ok)
        return NULL;
    HR(pConverter->CopyPixels(NULL, bmpData.Stride, w * h * 4, (BYTE *)bmpData.Scan0));
    bmp.UnlockBits(&bmpData);
    bmp.SetResolution((REAL)xres, (REAL)yres);
#undef HR

    // hack to avoid the use of ::new (because there won't be a corresponding ::delete)
    return bmp.Clone(0, 0, w, h, PixelFormat32bppARGB);
}

enum ImgFormat {
    Img_Unknown, Img_BMP, Img_GIF, Img_JPEG,
    Img_JXR, Img_PNG, Img_TGA, Img_TIFF,
};

static ImgFormat GfxFormatFromData(const char *data, size_t len)
{
    if (!data || len < 8)
        return Img_Unknown;
    // check the most common formats first
    if (str::StartsWith(data, "\x89PNG\x0D\x0A\x1A\x0A"))
        return Img_PNG;
    if (str::StartsWith(data, "\xFF\xD8"))
        return Img_JPEG;
    if (str::StartsWith(data, "GIF87a") || str::StartsWith(data, "GIF89a"))
        return Img_GIF;
    if (str::StartsWith(data, "BM"))
        return Img_BMP;
    if (memeq(data, "MM\x00\x2A", 4) || memeq(data, "II\x2A\x00", 4))
        return Img_TIFF;
    if (tga::HasSignature(data, len))
        return Img_TGA;
    if (memeq(data, "\x49\x49\xBC\x01", 4) || memeq(data, "\x49\x49\xBC\x00", 4))
        return Img_JXR;
    return Img_Unknown;
}

const TCHAR *GfxFileExtFromData(const char *data, size_t len)
{
    switch (GfxFormatFromData(data, len)) {
    case Img_BMP:  return _T(".bmp");
    case Img_GIF:  return _T(".gif");
    case Img_JPEG: return _T(".jpg");
    case Img_JXR:  return _T(".jxr");
    case Img_PNG:  return _T(".png");
    case Img_TGA:  return _T(".tga");
    case Img_TIFF: return _T(".tif");
    default:       return NULL;
    }
}

bool IsGdiPlusNativeFormat(const char *data, size_t len)
{
    ImgFormat fmt = GfxFormatFromData(data, len);
    return Img_BMP == fmt || Img_GIF == fmt ||
           Img_JPEG == fmt || Img_PNG == fmt ||
           Img_TIFF == fmt;
}

// cf. http://stackoverflow.com/questions/4598872/creating-hbitmap-from-memory-buffer/4616394#4616394
Bitmap *BitmapFromData(const char *data, size_t len)
{
    ImgFormat format = GfxFormatFromData(data, len);
    if (Img_TGA == format)
        return tga::ImageFromData(data, len);

    ScopedComPtr<IStream> stream(CreateStreamFromData(data, len));
    if (!stream)
        return NULL;
    if (Img_JXR == format)
        return ImageFromHdPhotoStream(stream);

    Bitmap *bmp = Bitmap::FromStream(stream);
    if (bmp && bmp->GetLastStatus() != Ok) {
        delete bmp;
        bmp = NULL;
    }
    return bmp;
}

// adapted from http://cpansearch.perl.org/src/RJRAY/Image-Size-3.230/lib/Image/Size.pm
Size BitmapSizeFromData(const char *data, size_t len)
{
    Size result;
    ByteReader r(data, len);
    switch (GfxFormatFromData(data, len)) {
    case Img_BMP:
        if (len >= sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)) {
            BITMAPINFOHEADER bmi;
            bool ok = r.UnpackLE(&bmi, sizeof(bmi), "3d2w6d", sizeof(BITMAPFILEHEADER));
            CrashIf(!ok);
            result.Width = bmi.biWidth;
            result.Height = bmi.biHeight;
        }
        break;
    case Img_GIF:
        if (len >= 13) {
            // find the first image's actual size instead of using the
            // "logical screen" size which is sometimes too large
            size_t ix = 13;
            // skip the global color table
            if ((r.Byte(10) & 0x80))
                ix += 3 * (1 << ((r.Byte(10) & 0x07) + 1));
            while (ix + 8 < len) {
                if (r.Byte(ix) == '\x2c') {
                    result.Width = r.WordLE(ix + 5);
                    result.Height = r.WordLE(ix + 7);
                    break;
                }
                else if (r.Byte(ix) == '\x21' && r.Byte(ix + 1) == '\xF9')
                    ix += 8;
                else if (r.Byte(ix) == '\x21' && r.Byte(ix + 1) == '\xFE') {
                    const char *commentEnd = r.Find(ix + 2, '\0');
                    ix = commentEnd ? commentEnd - data + 1 : len;
                }
                else if (r.Byte(ix) == '\x21' && r.Byte(ix + 1) == '\x01' && ix + 15 < len) {
                    const char *textDataEnd = r.Find(ix + 15, '\0');
                    ix = textDataEnd ? textDataEnd - data + 1 : len;
                }
                else if (r.Byte(ix) == '\x21' && r.Byte(ix + 1) == '\xFF' && ix + 14 < len) {
                    const char *applicationDataEnd = r.Find(ix + 14, '\0');
                    ix = applicationDataEnd ? applicationDataEnd - data + 1 : len;
                }
                else
                    break;
            }
        }
        break;
    case Img_JPEG:
        // find the last start of frame marker for non-differential Huffman coding
        for (size_t ix = 2; ix + 9 < len && r.Byte(ix) == '\xFF'; ) {
            if ('\xC0' <= r.Byte(ix + 1) && r.Byte(ix + 1) <= '\xC3') {
                result.Width = r.WordBE(ix + 7);
                result.Height = r.WordBE(ix + 5);
            }
            ix += r.WordBE(ix + 2) + 2;
        }
        break;
    case Img_JXR:
        if (len >= 10 && r.DWordLE(4) == 8) {
            WORD ifdLen = r.WordLE(8);
            for (size_t i = 0; i < ifdLen && 10 + (i + 1) * 12 < len; i++) {
                size_t idx = 10 + i * 12;
                if (r.WordLE(idx + 0) == 0xBC80 && r.WordLE(idx + 2) == 4 && r.DWordLE(idx + 4) == 1)
                    result.Width = r.DWordLE(idx + 8);
                if (r.WordLE(idx + 0) == 0xBC80 && r.WordLE(idx + 2) == 3 && r.DWordLE(idx + 4) == 1)
                    result.Width = r.WordLE(idx + 8);
                if (r.WordLE(idx + 0) == 0xBC80 && r.WordLE(idx + 2) == 1 && r.DWordLE(idx + 4) == 1)
                    result.Width = r.Byte(idx + 8);
                if (r.WordLE(idx + 0) == 0xBC81 && r.WordLE(idx + 2) == 4 && r.DWordLE(idx + 4) == 1)
                    result.Height = r.DWordLE(idx + 8);
                if (r.WordLE(idx + 0) == 0xBC81 && r.WordLE(idx + 2) == 3 && r.DWordLE(idx + 4) == 1)
                    result.Height = r.WordLE(idx + 8);
                if (r.WordLE(idx + 0) == 0xBC81 && r.WordLE(idx + 2) == 1 && r.DWordLE(idx + 4) == 1)
                    result.Height = r.Byte(idx + 8);
            }
        }
        break;
    case Img_PNG:
        if (len >= 24 && str::StartsWith(data + 12, "IHDR")) {
            result.Width = r.DWordBE(16);
            result.Height = r.DWordBE(20);
        }
        break;
    case Img_TGA:
        if (len >= 16) {
            result.Width = r.WordLE(12);
            result.Height = r.WordLE(14);
        }
        break;
    case Img_TIFF:
        // TODO: speed this up (if necessary)
        break;
    }

    if (result.Empty()) {
        // let GDI+ extract the image size if we've failed
        // (currently happens for animated GIFs and for all TIFFs and JXRs)
        Bitmap *bmp = BitmapFromData(data, len);
        if (bmp)
            result = Size(bmp->GetWidth(), bmp->GetHeight());
        delete bmp;
    }

    return result;
}
