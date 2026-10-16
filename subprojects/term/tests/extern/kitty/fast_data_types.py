
import base64 as _base64

CURSOR_BLOCK = 0
VT_PARSER_BUFFER_SIZE = 8192

has_avx2 = False
has_sse4_2 = False

def base64_decode(data) -> bytes:
    if isinstance(data, str):
        data = data.encode('utf-8')
    data = bytes(data)
    data = data.rstrip(b'=').strip()
    missing = len(data) % 4
    if missing:
        data += b'=' * (4 - missing)
    return _base64.b64decode(data)

def base64_encode(data) -> bytes:
    if isinstance(data, str):
        data = data.encode('utf-8')
    return _base64.b64encode(bytes(data)).rstrip(b'=')

def test_find_either_of_two_bytes(buf, a, b, which=0, align_offset=0):
    if isinstance(buf, str):
        buf = buf.encode('latin-1')
    buf = bytes(buf)
    ia = int(a)
    ib = int(b)
    for i, byte in enumerate(buf):
        if byte == ia or byte == ib:
            return i
    return -1

_utf8_pending = bytearray()

def test_utf8_decode_to_sentinel(data, which=0):
    global _utf8_pending

    if which == -1:
        _utf8_pending = bytearray()
        return (False, '', 0)

    initial_pending = len(_utf8_pending)
    buf = bytes(_utf8_pending) + bytes(data)
    _utf8_pending = bytearray()

    result = []
    i = 0
    n = len(buf)
    found_sentinel = False

    while i < n:
        byte = buf[i]

        if byte == 0x1B:
            found_sentinel = True
            i += 1
            break

        if byte < 0x80:
            result.append(chr(byte))
            i += 1
            continue

        if byte < 0xC0:
            result.append('\ufffd')
            i += 1
            continue

        if byte < 0xC2:
            result.append('\ufffd')
            i += 1
            continue

        if byte < 0xE0:
            if i + 1 < n:
                nb = buf[i + 1]
                if 0x80 <= nb <= 0xBF:
                    cp = ((byte & 0x1F) << 6) | (nb & 0x3F)
                    result.append(chr(cp))
                    i += 2
                    continue
                result.append('\ufffd')
                i += 1
                continue
            _utf8_pending.extend(buf[i:])
            i = n
            break

        if byte < 0xF0:
            if byte == 0xE0:
                lo, hi = 0xA0, 0xBF
            elif byte == 0xED:
                lo, hi = 0x80, 0x9F
            else:
                lo, hi = 0x80, 0xBF

            if i + 1 < n:
                nb = buf[i + 1]
                if lo <= nb <= hi:
                    if i + 2 < n:
                        nb2 = buf[i + 2]
                        if 0x80 <= nb2 <= 0xBF:
                            cp = ((byte & 0x0F) << 12) | ((nb & 0x3F) << 6) | (nb2 & 0x3F)
                            result.append(chr(cp))
                            i += 3
                            continue
                        result.append('\ufffd')
                        i += 2
                        continue
                    _utf8_pending.extend(buf[i:])
                    i = n
                    break
                result.append('\ufffd')
                i += 1
                continue
            _utf8_pending.extend(buf[i:])
            i = n
            break

        if byte < 0xF5:
            if byte == 0xF0:
                lo, hi = 0x90, 0xBF
            elif byte == 0xF4:
                lo, hi = 0x80, 0x8F
            else:
                lo, hi = 0x80, 0xBF

            if i + 1 < n:
                nb = buf[i + 1]
                if lo <= nb <= hi:
                    if i + 2 < n:
                        nb2 = buf[i + 2]
                        if 0x80 <= nb2 <= 0xBF:
                            if i + 3 < n:
                                nb3 = buf[i + 3]
                                if 0x80 <= nb3 <= 0xBF:
                                    cp = ((byte & 0x07) << 18) | ((nb & 0x3F) << 12) | ((nb2 & 0x3F) << 6) | (nb3 & 0x3F)
                                    result.append(chr(cp))
                                    i += 4
                                    continue
                                result.append('\ufffd')
                                i += 3
                                continue
                            _utf8_pending.extend(buf[i:])
                            i = n
                            break
                        result.append('\ufffd')
                        i += 2
                        continue
                    _utf8_pending.extend(buf[i:])
                    i = n
                    break
                result.append('\ufffd')
                i += 1
                continue
            _utf8_pending.extend(buf[i:])
            i = n
            break

        result.append('\ufffd')
        i += 1

    num_consumed = max(0, i - initial_pending)
    return (found_sentinel, ''.join(result), num_consumed)
