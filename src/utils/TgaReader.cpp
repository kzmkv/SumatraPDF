/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#include "TgaReader.h"

using namespace Gdiplus;

namespace tga {

#define TGA_FOOTER_SIGNATURE "TRUEVISION-XFILE."

enum ImageType {
    Type_Palette = 1,
    Type_Truecolor = 2,
    Type_Grayscale = 3,
    Type_Palette_RLE = 9,
    Type_Truecolor_RLE = 10,
    Type_Grayscale_RLE = 11,
};

enum ImageFlag {
    Flag_Alpha = 0x0F,
    Flag_InvertX = 0x10,
    Flag_InvertY = 0x20,
    Flag_Reserved = 0xC0,
};

enum ImageAlpha {
    Alpha_Ignore = 0,
    Alpha_Normal = 3,
    Alpha_Premultiplied = 4,
};

#pragma pack(push)
#pragma pack(1)

struct TgaHeader {
    uint8_t     idLength;
    uint8_t     cmapType;
    uint8_t     imageType;
    uint16_t    cmapFirstEntry;
    uint16_t    cmapLength;
    uint8_t     cmapBitDepth;
    uint16_t    offsetX, offsetY;
    uint16_t    width, height;
    uint8_t     bitDepth;
    uint8_t     flags;
};

struct TgaFooter {
    uint32_t    extAreaOffset;
    uint32_t    devAreaOffset;
    const char  signature[18];
};

struct TgaExtArea {
    uint16_t    size;
    const char  author[41];
    const char  comments[4][81];
    uint16_t    dateTime[6];
    uint8_t     fields_14_to_15[47];
    const char  progName[41];
    uint16_t    progVersion;
    const char  progVersionC;
    uint32_t    fields_18_to_23[6];
    uint8_t     alphaType;
};

#pragma pack(pop)

STATIC_ASSERT(sizeof(TgaHeader) == 18, tgaHeaderSize);
STATIC_ASSERT(sizeof(TgaFooter) == 26, tgaFooterSize);
STATIC_ASSERT(sizeof(TgaExtArea) == 495, tgaExtAreaSize);

static inline uint16_t convLE(uint16_t x)
{
    uint8_t *data = (uint8_t *)&x;
    return data[0] | (data[1] << 8);
}

static inline uint32_t convLE(uint32_t x)
{
    uint8_t *data = (uint8_t *)&x;
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

static bool HasVersion2Footer(const char *data, size_t len)
{
    if (len < sizeof(TgaHeader) + sizeof(TgaFooter))
        return false;
    TgaFooter *footerLE = (TgaFooter *)(data + len - sizeof(TgaFooter));
    return str::EqN(footerLE->signature, TGA_FOOTER_SIGNATURE, sizeof(footerLE->signature));
}

static TgaExtArea *GetExtAreaPtr(const char *data, size_t len)
{
    if (!HasVersion2Footer(data, len))
        return NULL;
    TgaFooter *footerLE = (TgaFooter *)(data + len - sizeof(TgaFooter));
    if (convLE(footerLE->extAreaOffset) < sizeof(TgaHeader) ||
        convLE(footerLE->extAreaOffset) + sizeof(TgaExtArea) + sizeof(TgaFooter) > len) {
        return NULL;
    }
    TgaExtArea *extAreaLE = (TgaExtArea *)(data + convLE(footerLE->extAreaOffset));
    if (convLE(extAreaLE->size) < sizeof(TgaExtArea))
        return NULL;
    return extAreaLE;
}

// note: we only support the more common bit depths:
// http://www.ryanjuckett.com/programming/graphics/26-parsing-colors-in-a-tga-file
static PixelFormat GetPixelFormat(TgaHeader *headerLE, ImageAlpha aType=Alpha_Normal)
{
    int bits;
    if (Type_Palette == headerLE->imageType || Type_Palette_RLE == headerLE->imageType) {
        if (1 != headerLE->cmapType || 8 != headerLE->bitDepth && 16 != headerLE->bitDepth)
            return 0;
        bits = headerLE->cmapBitDepth;
    }
    else if (Type_Truecolor == headerLE->imageType || Type_Truecolor_RLE == headerLE->imageType) {
        bits = headerLE->bitDepth;
    }
    else if (Type_Grayscale == headerLE->imageType || Type_Grayscale_RLE == headerLE->imageType) {
        if (8 != headerLE->bitDepth || (headerLE->flags & Flag_Alpha))
            return 0;
        // using a non-indexed format so that we don't have to bother with a palette
        return PixelFormat24bppRGB;
    }
    else
        return 0;

    int alphaBits = (headerLE->flags & Flag_Alpha);
    if (15 == bits && 0 == alphaBits)
        return PixelFormat16bppRGB555;
    if (16 == bits && (0 == alphaBits || Alpha_Ignore == aType))
        return PixelFormat16bppRGB555;
    if (16 == bits && 1 == alphaBits)
        return PixelFormat16bppARGB1555;
    if (24 == bits && 0 == alphaBits)
        return PixelFormat24bppRGB;
    if (32 == bits && (0 == alphaBits || Alpha_Ignore == aType))
        return PixelFormat32bppRGB;
    if (32 == bits && 8 == alphaBits && Alpha_Normal == aType)
        return PixelFormat32bppARGB;
    if (32 == bits && 8 == alphaBits && Alpha_Premultiplied == aType)
        return PixelFormat32bppPARGB;
    return 0;
}

static ImageAlpha GetAlphaType(const char *data, size_t len)
{
    TgaExtArea *extAreaLE = GetExtAreaPtr(data, len);
    if (!extAreaLE)
        return Alpha_Normal;

    switch (extAreaLE->alphaType) {
    case Alpha_Normal:          return Alpha_Normal;
    case Alpha_Premultiplied:   return Alpha_Premultiplied;
    default:                    return Alpha_Ignore;
    }
}

// checks whether this could be data for a TGA image
bool HasSignature(const char *data, size_t len)
{
    if (HasVersion2Footer(data, len))
        return true;
    // fall back to checking for values that would be valid for a TGA image
    if (len < sizeof(TgaHeader))
        return false;
    TgaHeader *headerLE = (TgaHeader *)data;
    if (headerLE->cmapType != 0 && headerLE->cmapType != 1)
        return false;
    if ((headerLE->flags & Flag_Reserved))
        return false;
    if (!GetPixelFormat(headerLE))
        return false;
    return true;

}

static void SetImageProperty(Bitmap *bmp, PROPID id, const char *asciiValue)
{
    PropertyItem item;
    item.id = id;
    item.type = PropertyTagTypeASCII;
    item.value = (void *)asciiValue;
    item.length = str::Len(asciiValue) + 1;
    Status ok = bmp->SetPropertyItem(&item);
    CrashIf(ok != Ok);
}

static bool IsFieldSet(const char *field, size_t len, bool isBinary=false)
{
    for (size_t i = 0; i < len; i++) {
        if (field[i] && (isBinary || field[i] != ' '))
            return isBinary || '\0' == field[len - 1];
    }
    return false;
}

static void CopyMetadata(const char *data, size_t len, Bitmap *bmp)
{
    TgaExtArea *extAreaLE = GetExtAreaPtr(data, len);
    if (!extAreaLE)
        return;

    if (IsFieldSet(extAreaLE->author, sizeof(extAreaLE->author)))
        SetImageProperty(bmp, PropertyTagArtist, extAreaLE->author);
    if (IsFieldSet((const char *)extAreaLE->dateTime, sizeof(extAreaLE->dateTime), true)) {
        char dateTime[20];
        int count = _snprintf(dateTime, dimof(dateTime), "%04d-%02d-%02d %02d:%02d:%02d",
            convLE(extAreaLE->dateTime[2]), convLE(extAreaLE->dateTime[1]),
            convLE(extAreaLE->dateTime[0]), convLE(extAreaLE->dateTime[3]),
            convLE(extAreaLE->dateTime[4]), convLE(extAreaLE->dateTime[5]));
        if (19 == count)
            SetImageProperty(bmp, PropertyTagDateTime, dateTime);
    }
    if (IsFieldSet(extAreaLE->progName, sizeof(extAreaLE->progName))) {
        char software[49];
        str::BufSet(software, 41, extAreaLE->progName);
        if (convLE(extAreaLE->progVersion) != 0) {
            _snprintf(software + str::Len(software), 9, " %d.%d%c",
                convLE(extAreaLE->progVersion) / 100,
                convLE(extAreaLE->progVersion) % 100,
                extAreaLE->progVersionC != ' ' ? extAreaLE->progVersionC : '\0');
            software[48] = '\0';
        }
        SetImageProperty(bmp, PropertyTagSoftwareUsed, software);
    }
}

struct ReadState {
    const char *data;
    const char *end;
    ImageType type;
    int n;
    bool isRLE;
    int repeat;
    bool repeatSame;
    struct {
        int firstEntry;
        int length;
        const char *data;
        int n;
    } cmap;
    bool failed;
};

static inline void CopyPixel(char *dst, const char *src, int n)
{
    switch (n) {
    case 3: dst[2] = src[2]; // fall through
    case 2: *(uint16_t *)dst = *(uint16_t *)src; break;
    case 4: *(uint32_t *)dst = *(uint32_t *)src; break;
    default: CrashIf(true);
    }
}

static void ReadPixel(ReadState& s, char *dst)
{
    if (s.isRLE && 0 == s.repeat && s.data + s.n < s.end) {
        s.repeat = (*s.data & 0x7F) + 1;
        s.repeatSame = (*s.data & 0x80);
        s.data++;
    }
    else if (s.data + s.n > s.end) {
        s.failed = true;
        return;
    }

    int idx;
    switch (s.type) {
    case Type_Palette: case Type_Palette_RLE:
        idx = ((uint8_t)s.data[0] | (2 == s.n ? ((uint8_t)s.data[1] << 8) : 0)) - s.cmap.firstEntry;
        if (0 <= idx && idx < s.cmap.length)
            CopyPixel(dst, s.cmap.data + idx * s.cmap.n, s.cmap.n);
        break;
    case Type_Truecolor: case Type_Truecolor_RLE:
        CopyPixel(dst, s.data, s.n);
        break;
    case Type_Grayscale: case Type_Grayscale_RLE:
        dst[0] = dst[1] = dst[2] = s.data[0];
        break;
    }

    if (!s.isRLE || 0 == --s.repeat || !s.repeatSame)
        s.data += s.n;
}

Gdiplus::Bitmap *ImageFromData(const char *data, size_t len)
{
    if (len < sizeof(TgaHeader))
        return NULL;

    ReadState s = { 0 };
    TgaHeader *headerLE = (TgaHeader *)data;
    s.data = data + sizeof(TgaHeader) + headerLE->idLength;
    s.end = data + len;
    if (1 == headerLE->cmapType) {
        s.cmap.data = s.data;
        s.cmap.n = (headerLE->cmapBitDepth + 7) / 8;
        s.cmap.length = convLE(headerLE->cmapLength);
        s.cmap.firstEntry = convLE(headerLE->cmapFirstEntry);
        s.data += s.cmap.length * s.cmap.n;
    }
    s.type = (ImageType)headerLE->imageType;
    s.n = (headerLE->bitDepth + 7) / 8;
    s.isRLE = headerLE->imageType >= 8;

    PixelFormat format = GetPixelFormat(headerLE, GetAlphaType(data, len));
    if (!format)
        return NULL;

    int w = convLE(headerLE->width);
    int h = convLE(headerLE->height);
    int n = ((format >> 8) & 0x3F) / 8;
    bool invertX = (headerLE->flags & Flag_InvertX);
    bool invertY = (headerLE->flags & Flag_InvertY);

    Bitmap bmp(w, h, format);
    BitmapData bmpData;
    Status ok = bmp.LockBits(&Rect(0, 0, w, h), ImageLockModeWrite, format, &bmpData);
    if (ok != Ok)
        return NULL;
    for (int y = 0; y < h; y++) {
        char *rowOut = (char *)bmpData.Scan0 + bmpData.Stride * (invertY ? y : h - 1 - y);
        for (int x = 0; x < w; x++) {
            ReadPixel(s, rowOut + n * (invertX ? w - 1 - x : x));
        }
    }
    bmp.UnlockBits(&bmpData);
    if (s.failed)
        return NULL;
    CopyMetadata(data, len, &bmp);
    // hack to avoid the use of ::new (because there won't be a corresponding ::delete)
    return bmp.Clone(0, 0, w, h, format);
}

inline bool memeq3(const char *pix1, const char *pix2)
{
    return *(WORD *)pix1 == *(WORD *)pix2 && pix1[2] == pix2[2];
}

unsigned char *SerializeBitmap(HBITMAP hbmp, size_t *bmpBytesOut)
{
    BITMAP bmpInfo;
    GetObject(hbmp, sizeof(BITMAP), &bmpInfo);
    if ((ULONG)bmpInfo.bmWidth > USHRT_MAX || (ULONG)bmpInfo.bmHeight > USHRT_MAX)
        return 0;

    WORD w = (WORD)bmpInfo.bmWidth;
    WORD h = (WORD)bmpInfo.bmHeight;
    int stride = ((w * 3 + 3) / 4) * 4;
    ScopedMem<char> bmpData((char *)malloc(stride * h));
    if (!bmpData)
        return NULL;

    BITMAPINFO bmi = { 0 };
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 24;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC hDC = GetDC(NULL);
    if (!GetDIBits(hDC, hbmp, 0, h, bmpData, &bmi, DIB_RGB_COLORS)) {
        ReleaseDC(NULL, hDC);
        return NULL;
    }
    ReleaseDC(NULL, hDC);

    TgaHeader headerLE = { 0 };
    headerLE.imageType = Type_Truecolor_RLE;
    headerLE.width = convLE(w);
    headerLE.height = convLE(h);
    headerLE.bitDepth = 24;
    TgaFooter footerLE = { 0, 0, TGA_FOOTER_SIGNATURE };

    str::Str<char> tgaData;
    tgaData.Append((char *)&headerLE, sizeof(headerLE));
    for (int k = 0; k < h; k++) {
        char *line = bmpData + k * stride;
        for (int i = 0, j = 1; i < w; i += j, j = 1) {
            // determine the length of a run of identical pixels
            while (i + j < w && j < 128 && memeq3(line + i * 3, line + (i + j) * 3)) {
                j++;
            }
            if (j > 1) {
                tgaData.Append(j - 1 + 128);
                tgaData.Append(line + i * 3, 3);
            } else {
                // determine the length of a run of different pixels
                while (i + j < w && j < 128 && !memeq3(line + (i + j - 1) * 3, line + (i + j) * 3)) {
                    j++;
                }
                tgaData.Append(j - 1);
                tgaData.Append(line + i * 3, j * 3);
            }
        }
    }
    tgaData.Append((char *)&footerLE, sizeof(footerLE));

    if (bmpBytesOut)
        *bmpBytesOut = tgaData.Size();
    return (unsigned char *)tgaData.StealData();
}

}
