import struct, sys
data = open(sys.argv[1], "rb").read()
OFF = 0x10000  # EBOOT VA = file offset + 0x10000
be = lambda o: struct.unpack_from(">I", data, o)[0]
is_ila_r2 = lambda x: (x >> 25) == 0x21 and (x & 0x7F) == 2
magic = struct.pack(">I", 0x55011055)
i = data.find(magic)
n = 0
while i >= 0:
    h = i - 0x2C
    if h >= 0 and h % 4 == 0 and all(is_ila_r2(be(h + 4 * k)) for k in range(4)):
        va = h + OFF
        tag = "in-range" if 0x01DCD000 <= va < 0x01E87000 else "OUTSIDE"
        print(f"{va:08X} type={be(h+0x28)} entry=0x{be(h+0x10):X} size=0x{be(h+0x14):X} {tag}")
        n += 1
    i = data.find(magic, i + 1)
print("total", n)
