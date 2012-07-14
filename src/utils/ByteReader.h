/* Copyright 2012 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#ifndef ByteReader_h
#define ByteReader_h

class ByteReader {
    const uint8_t *d;
    size_t len;

    // Unpacks a structure from the data according to the given format
    // e.g. the format "32b2w6d" unpacks 32 Bytes, 2 16-bit Words and 6 32-bit Dwords
    bool Unpack(char *strct, size_t size, const char *format, bool LE, size_t off) {
        int repeat = 0;
        size_t idx = 0;
        for (const char *c = format; *c; c++) {
            if (isdigit((wint_t)*c)) {
                repeat = atoi(c);
                for (c++; isdigit((wint_t)*c); c++);
            }
            switch (*c) {
            case 'b':
                if (off + idx + 1 > len || idx + 1 > size)
                    return false;
                *(uint8_t *)(strct + idx) = Byte(off + idx);
                idx += 1;
                break;
            case 'w':
                if (off + idx + 2 > len || idx + 2 > size)
                    return false;
                *(uint16_t *)(strct + idx) = LE ? WordLE(off + idx) : WordBE(off + idx);
                idx += 2;
                break;
            case 'd':
                if (off + idx + 4 > len || idx + 4 > size)
                    return false;
                *(uint32_t *)(strct + idx) = LE ? DWordLE(off + idx) : DWordBE(off + idx);
                idx += 4;
                break;
            default:
                return false;
            }
            if (--repeat > 0)
                c--;
        }
        return idx == size;
    }

public:
    ByteReader(const char *data, size_t len) :
        d((const uint8_t *)data), len(len) { }
    ByteReader(const unsigned char *data, size_t len) :
        d((const uint8_t *)data), len(len) { }

    uint8_t Byte(size_t off) {
        if (off < len)
            return d[off];
        return 0;
    }

    uint16_t WordLE(size_t off) {
        if (off + 2 <= len)
            return d[off] | (d[off + 1] << 8);
        return 0;
    }

    uint16_t WordBE(size_t off) {
        if (off + 2 <= len)
            return (d[off] << 8) | d[off + 1];
        return 0;
    }

    uint32_t DWordLE(size_t off) {
        if (off + 4 <= len)
            return d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | (d[off + 3] << 24);
        return 0;
    }

    uint32_t DWordBE(size_t off) {
        if (off + 4 <= len)
            return (d[off] << 24) | (d[off + 1] << 16) | (d[off + 2] << 8) | d[off + 3];
        return 0;
    }

    const char *Find(size_t off, uint8_t byte) {
        if (off >= len)
            return NULL;
        return (const char *)memchr(d + off, byte, len - off);
    }

    bool UnpackLE(void *strct, size_t size, const char *format, size_t off=0) {
        return Unpack((char *)strct, size, format, true, off);
    }

    bool UnpackBE(void *strct, size_t size, const char *format, size_t off=0) {
        return Unpack((char *)strct, size, format, false, off);
    }
};

#endif
