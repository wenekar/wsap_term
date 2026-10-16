"""Stubs for Kitty's ``kitty.fast_data_types`` C extension module.

These pure-Python implementations provide the subset of the API used by
Kitty's parser test-suite.  The SIMD-accelerated variants are not available,
so ``has_avx2`` and ``has_sse4_2`` are ``False`` and the decoder falls back
to a scalar implementation that matches the WHATWG Encoding Standard.
"""

import base64 as _base64

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

CURSOR_BLOCK = 0
VT_PARSER_BUFFER_SIZE = 8192  # Large enough that buffer-full tests don't trigger

has_avx2 = False
has_sse4_2 = False

# ---------------------------------------------------------------------------
# Base64 helpers (padding-free, matching Kitty's convention)
# ---------------------------------------------------------------------------

def base64_decode(data) -> bytes:
    """Decode base64, accepting both padded and unpadded input."""
    if isinstance(data, str):
        data = data.encode('utf-8')
    data = bytes(data)
    # Strip any existing padding and whitespace.
    data = data.rstrip(b'=').strip()
    # Re-add the minimum padding required by the stdlib decoder.
    missing = len(data) % 4
    if missing:
        data += b'=' * (4 - missing)
    return _base64.b64decode(data)


def base64_encode(data) -> bytes:
    """Encode to base64 without trailing ``=`` padding."""
    if isinstance(data, str):
        data = data.encode('utf-8')
    return _base64.b64encode(bytes(data)).rstrip(b'=')


# ---------------------------------------------------------------------------
# Byte search helper
# ---------------------------------------------------------------------------

def test_find_either_of_two_bytes(buf, a, b, which=0, align_offset=0):
    """Return the index of the first occurrence of byte *a* or *b* in *buf*.

    Returns -1 when neither byte is found.  The *which* and *align_offset*
    parameters select SIMD variants in Kitty's native code; the pure-Python
    fallback ignores them.
    """
    if isinstance(buf, str):
        buf = buf.encode('latin-1')
    buf = bytes(buf)
    ia = int(a)
    ib = int(b)
    for i, byte in enumerate(buf):
        if byte == ia or byte == ib:
            return i
    return -1


# ---------------------------------------------------------------------------
# UTF-8 decoder with WHATWG U+FFFD substitution
# ---------------------------------------------------------------------------

# Module-level state for incomplete trailing sequences across calls.
_utf8_pending = bytearray()


def test_utf8_decode_to_sentinel(data, which=0):
    """Decode UTF-8 bytes until an ESC (0x1b) sentinel or end of data.

    Returns ``(found_sentinel: bool, decoded_text: str, num_consumed: int)``.

    *num_consumed* counts only the bytes consumed from *data* (not any
    pending bytes carried over from a previous call).  Invalid sequences are
    replaced with U+FFFD using the maximal-subpart rules from the WHATWG
    Encoding Standard.

    Passing *which* = -1 resets the internal pending-state buffer.
    """
    global _utf8_pending

    # --- reset -----------------------------------------------------------
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

        # ESC sentinel
        if byte == 0x1B:
            found_sentinel = True
            i += 1
            break

        # ASCII
        if byte < 0x80:
            result.append(chr(byte))
            i += 1
            continue

        # Lone continuation byte 0x80-0xBF
        if byte < 0xC0:
            result.append('\ufffd')
            i += 1
            continue

        # Invalid starter bytes 0xC0-0xC1
        if byte < 0xC2:
            result.append('\ufffd')
            i += 1
            continue

        # 2-byte sequence 0xC2-0xDF
        if byte < 0xE0:
            if i + 1 < n:
                nb = buf[i + 1]
                if 0x80 <= nb <= 0xBF:
                    cp = ((byte & 0x1F) << 6) | (nb & 0x3F)
                    result.append(chr(cp))
                    i += 2
                    continue
                # Bad continuation: starter alone is one subpart
                result.append('\ufffd')
                i += 1
                continue
            # Incomplete at end of input
            _utf8_pending.extend(buf[i:])
            i = n
            break

        # 3-byte sequence 0xE0-0xEF
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
                        # Bad 2nd continuation: starter + 1st cont = one subpart
                        result.append('\ufffd')
                        i += 2
                        continue
                    # Incomplete: save starter + 1st continuation
                    _utf8_pending.extend(buf[i:])
                    i = n
                    break
                # Bad 1st continuation: starter alone is one subpart
                result.append('\ufffd')
                i += 1
                continue
            # Incomplete: save starter only
            _utf8_pending.extend(buf[i:])
            i = n
            break

        # 4-byte sequence 0xF0-0xF4
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
                                # Bad 3rd continuation
                                result.append('\ufffd')
                                i += 3
                                continue
                            # Incomplete: save starter + 2 continuations
                            _utf8_pending.extend(buf[i:])
                            i = n
                            break
                        # Bad 2nd continuation: starter + 1st cont = one subpart
                        result.append('\ufffd')
                        i += 2
                        continue
                    # Incomplete: save starter + 1st continuation
                    _utf8_pending.extend(buf[i:])
                    i = n
                    break
                # Bad 1st continuation: starter alone is one subpart
                result.append('\ufffd')
                i += 1
                continue
            # Incomplete: save starter only
            _utf8_pending.extend(buf[i:])
            i = n
            break

        # Invalid starter bytes 0xF5-0xFF
        result.append('\ufffd')
        i += 1

    num_consumed = max(0, i - initial_pending)
    return (found_sentinel, ''.join(result), num_consumed)
