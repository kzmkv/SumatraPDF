#include "ByteOrderDecoder.h"

#define ABC "abc"
static void ByteOrderTests()
{
    unsigned char d1[] = {
        0x00, 0x01,
        0x00, // to skip
        0x01, 0x00,
        0xff, 0xfe,
        0x00, 0x00, // to skip
        0x00, 0x00, 0x00, 0x01,
        0x01, 0x00, 0x00, 0x00,
        0xff, 0xff, 0xff, 0xfe,
        0x02, 0x00,
        'a', 'b', 'c'
    };

    {
        uint16 vu16; uint32 vu32;
        char b[3];
        ByteOrderDecoder d(d1, sizeof(d1), ByteOrderDecoder::LittleEndian);
        assert(0 == d.Offset());
        vu16 = d.UInt16();
        assert(2 == d.Offset());
        assert(vu16 == 0x100);
        d.Skip(1);
        assert(3 == d.Offset());
        vu16 = d.UInt16();
        assert(5 == d.Offset());
        assert(vu16 == 0x1);
        vu16 = d.UInt16();
        assert(7 == d.Offset());
        assert(vu16 == 0xfeff);
        d.Skip(2);
        assert(9 == d.Offset());

        vu32 = d.UInt32();
        assert(13 == d.Offset());
        assert(vu32 == 0x1000000);
        vu32 = d.UInt32();
        assert(17 == d.Offset());
        assert(vu32 == 1);
        vu32 = d.UInt32();
        assert(21 == d.Offset());
        assert(vu32 == 0xfeffffff);

        d.ChangeOrder(ByteOrderDecoder::BigEndian);
        vu16 = d.UInt16();
        assert(vu16 == 0x200);
        assert(23 == d.Offset());

        d.Bytes(b, 3);
        assert(memeq(ABC, b, 3));
        assert(26 == d.Offset());
    }

    {
        uint16 vu16; uint32 vu32;
        char b[3];
        ByteOrderDecoder d(d1, sizeof(d1), ByteOrderDecoder::BigEndian);
        vu16 = d.UInt16();
        assert(vu16 == 1);
        d.Skip(1);
        vu16 = d.UInt16();
        assert(vu16 == 0x100);
        vu16 = d.UInt16();
        assert(vu16 == 0xfffe);
        d.Skip(2);

        vu32 = d.UInt32();
        assert(vu32 == 1);
        vu32 = d.UInt32();
        assert(vu32 == 0x1000000);
        vu32 = d.UInt32();
        assert(vu32 == 0xfffffffe);

        d.ChangeOrder(ByteOrderDecoder::LittleEndian);
        vu16 = d.UInt16();
        assert(vu16 == 2);
        d.Bytes(b, 3);
        assert(memeq(ABC, b, 3));
        assert(26 == d.Offset());
    }

    {
        int16 v16; int32 v32;
        char b[3];
        ByteOrderDecoder d(d1, sizeof(d1), ByteOrderDecoder::LittleEndian);
        v16 = d.Int16();
        assert(v16 == 0x100);
        d.Skip(1);
        v16 = d.Int16();
        assert(v16 == 0x1);
        v16 = d.Int16();
        assert(v16 == -257);
        d.Skip(2);

        v32 = d.Int32();
        assert(v32 == 0x1000000);
        v32 = d.Int32();
        assert(v32 == 1);
        v32 = d.Int32();
        assert(v32 == -16777217);

        d.ChangeOrder(ByteOrderDecoder::BigEndian);
        v16 = d.Int16();
        assert(v16 == 0x200);
        d.Bytes(b, 3);
        assert(memeq(ABC, b, 3));
        assert(26 == d.Offset());
    }

    {
        int16 v16; int32 v32;
        char b[3];
        ByteOrderDecoder d(d1, sizeof(d1), ByteOrderDecoder::BigEndian);
        v16 = d.Int16();
        assert(v16 == 0x1);
        d.Skip(1);
        v16 = d.Int16();
        assert(v16 == 0x100);
        v16 = d.Int16();
        assert(v16 == -2);
        d.Skip(2);

        v32 = d.Int32();
        assert(v32 == 1);
        v32 = d.Int32();
        assert(v32 == 0x1000000);
        v32 = d.Int32();
        assert(v32 == -2);

        d.ChangeOrder(ByteOrderDecoder::LittleEndian);
        v16 = d.Int16();
        assert(v16 == 2);
        d.Bytes(b, 3);
        assert(memeq(ABC, b, 3));
        assert(26 == d.Offset());
    }
}

#undef ABC
