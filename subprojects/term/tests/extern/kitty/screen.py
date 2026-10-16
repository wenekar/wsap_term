
import ctypes
import os

_lib = ctypes.CDLL(os.environ.get('WSAP_KITTY_LIB', os.path.join(os.path.dirname(__file__), 'libkitty_bridge.so')))

class TermCell(ctypes.Structure):
    _fields_ = [
        ('text', ctypes.c_uint32),
        ('fg', ctypes.c_uint32),
        ('bg', ctypes.c_uint32),
        ('attrs', ctypes.c_uint16),
        ('width', ctypes.c_uint8),
        ('reserved', ctypes.c_uint8),
    ]

assert ctypes.sizeof(TermCell) == 16

REPLY_FN = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.POINTER(ctypes.c_char), ctypes.c_size_t)

_lib.kitty_term_new.restype = ctypes.c_void_p
_lib.kitty_term_new.argtypes = [ctypes.c_int, ctypes.c_int, REPLY_FN, ctypes.c_void_p]
_lib.kitty_term_free.restype = None
_lib.kitty_term_free.argtypes = [ctypes.c_void_p]
_lib.kitty_term_resize.restype = ctypes.c_bool
_lib.kitty_term_resize.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
_lib.kitty_term_write.restype = None
_lib.kitty_term_write.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_size_t]
_lib.kitty_term_cell.restype = ctypes.c_bool
_lib.kitty_term_cell.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                 ctypes.POINTER(TermCell)]
_lib.kitty_term_cell_text.restype = ctypes.c_int
_lib.kitty_term_cell_text.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int,
                                      ctypes.POINTER(ctypes.c_uint32), ctypes.c_int]
for _name, _type in {
    'cols': ctypes.c_int,
    'rows': ctypes.c_int,
    'cursor_x': ctypes.c_int,
    'cursor_y': ctypes.c_int,
    'cursor_style': ctypes.c_int,
    'cursor_blink': ctypes.c_bool,
    'application_cursor': ctypes.c_bool,
}.items():
    _fn = getattr(_lib, 'kitty_term_' + _name)
    _fn.argtypes = [ctypes.c_void_p]
    _fn.restype = _type

WSAP_VTCORE_DEFAULT_FG = 0xFFFFFFFF
WSAP_VTCORE_DEFAULT_BG = 0xFF1E1E2E

WSAP_VTCORE_ATTR_BOLD            = 1 << 0
WSAP_VTCORE_ATTR_ITALIC          = 1 << 1
WSAP_VTCORE_ATTR_FAINT           = 1 << 10
WSAP_VTCORE_ATTR_STRIKE          = 1 << 3
WSAP_VTCORE_ATTR_OVERLINE        = 1 << 4
WSAP_VTCORE_ATTR_LINK            = 1 << 15
WSAP_VTCORE_ATTR_EMOJI           = 1 << 5
WSAP_VTCORE_ATTR_TEXT            = 1 << 6
WSAP_VTCORE_UNDERLINE_FLAGS      = (1 << 2, 1 << 11, 1 << 12, 1 << 13, 1 << 14)

_CLUSTER_CAP = 32

for _name, _type in {
    'scroll_top': ctypes.c_int,
    'scroll_bottom': ctypes.c_int,
    'cur_fg': ctypes.c_uint32,
    'cur_bg': ctypes.c_uint32,
    'bold': ctypes.c_bool,
    'faint': ctypes.c_bool,
    'italic': ctypes.c_bool,
    'strike': ctypes.c_bool,
    'inverse': ctypes.c_bool,
    'underline': ctypes.c_uint8,
}.items():
    _fn = getattr(_lib, 'kitty_screen_' + _name)
    _fn.argtypes = [ctypes.c_void_p]
    _fn.restype = _type

_lib.kitty_screen_row_length.argtypes = [ctypes.c_void_p, ctypes.c_int]
_lib.kitty_screen_row_length.restype = ctypes.c_int
_lib.kitty_screen_cell_is_tab.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
_lib.kitty_screen_cell_is_tab.restype = ctypes.c_bool

_ANSI_COLORS = [
    0xFF585B70, 0xFFCC6666, 0xFFB5BD68, 0xFFF0C674,
    0xFF81A2BE, 0xFFB294BB, 0xFF8ABEB7, 0xFFC5C8C6,
    0xFF666666, 0xFFD54E53, 0xFFB9CA4A, 0xFFE7C547,
    0xFF7AA6DA, 0xFFC397D8, 0xFF70C0B1, 0xFFEAEAEA,
]

def _build_color_table():
    table = {}
    for i, rgb in enumerate(_ANSI_COLORS):
        table[rgb] = (i << 8) | 1
    levels = [0, 95, 135, 175, 215, 255]
    for i in range(16, 232):
        n = i - 16
        r, g, b = levels[n // 36], levels[(n // 6) % 6], levels[n % 6]
        rgb = 0xFF000000 | (r << 16) | (g << 8) | b
        table[rgb] = (i << 8) | 1
    for i in range(232, 256):
        gray = 8 + (i - 232) * 10
        rgb = 0xFF000000 | (gray << 16) | (gray << 8) | gray
        table[rgb] = (i << 8) | 1
    return table

_COLOR_TO_KITTY = _build_color_table()

WSAP_VTCORE_COLOR_REF = 0x01000000

def _convert_color(color):
    if (color >> 24) == (WSAP_VTCORE_COLOR_REF >> 24):
        slot = color & 0x1FF
        return 0 if slot >= 0x100 else (slot << 8) | 1
    if color == WSAP_VTCORE_DEFAULT_FG or color == WSAP_VTCORE_DEFAULT_BG:
        return 0
    kitty = _COLOR_TO_KITTY.get(color)
    if kitty is not None:
        return kitty
    r = (color >> 16) & 0xFF
    g = (color >> 8) & 0xFF
    b = color & 0xFF
    return (r << 24) | (g << 16) | (b << 8) | 2

def _read_property(ptr, name):
    return getattr(_lib, 'kitty_screen_' + name)(ptr)

class Cursor:

    def __init__(self, screen):
        self._screen = screen

    @property
    def _ptr(self):
        return self._screen._ptr

    @property
    def x(self):
        return _lib.kitty_term_cursor_x(self._screen._term)

    @property
    def y(self):
        return _lib.kitty_term_cursor_y(self._screen._term)

    @property
    def bold(self):
        return _read_property(self._ptr, 'bold')

    @property
    def italic(self):
        return _read_property(self._ptr, 'italic')

    @property
    def dim(self):
        return _read_property(self._ptr, 'faint')

    @property
    def reverse(self):
        return _read_property(self._ptr, 'inverse')

    @property
    def strikethrough(self):
        return _read_property(self._ptr, 'strike')

    @property
    def decoration(self):
        return _read_property(self._ptr, 'underline')

    @property
    def fg(self):
        return _convert_color(_read_property(self._ptr, 'cur_fg'))

    @property
    def bg(self):
        return _convert_color(_read_property(self._ptr, 'cur_bg'))

    @property
    def blink(self):
        return _lib.kitty_term_cursor_blink(self._screen._term)

    @property
    def shape(self):
        return _lib.kitty_term_cursor_style(self._screen._term)

class CellCursor:

    def __init__(self, cell):
        self._cell = cell

    @property
    def bold(self):
        return bool(self._cell.attrs & WSAP_VTCORE_ATTR_BOLD)

    @property
    def italic(self):
        return bool(self._cell.attrs & WSAP_VTCORE_ATTR_ITALIC)

    @property
    def dim(self):
        return bool(self._cell.attrs & WSAP_VTCORE_ATTR_FAINT)

    @property
    def reverse(self):
        return False

    @property
    def strikethrough(self):
        return bool(self._cell.attrs & WSAP_VTCORE_ATTR_STRIKE)

    @property
    def decoration(self):
        return next((i for i, flag in enumerate(WSAP_VTCORE_UNDERLINE_FLAGS, 1)
                     if self._cell.attrs & flag), 0)

    @property
    def fg(self):
        return _convert_color(self._cell.fg)

    @property
    def bg(self):
        return _convert_color(self._cell.bg)

class Line:

    def __init__(self, screen, y):
        self._screen = screen
        self._y = y

    def cursor_from(self, x):
        cell = TermCell()
        if not _lib.kitty_term_cell(self._screen._term, x, self._y, ctypes.byref(cell)):
            return CellCursor(TermCell())
        return CellCursor(cell)

    def __str__(self):
        length = min(_lib.kitty_screen_row_length(self._screen._term, self._y),
                     self._screen.columns)
        if length == 0:
            return ''
        result = []
        cell = TermCell()
        cps = (ctypes.c_uint32 * _CLUSTER_CAP)()
        for x in range(length):
            if not _lib.kitty_term_cell(self._screen._term, x, self._y, ctypes.byref(cell)):
                result.append(' ')
                continue
            if cell.width == 0:
                continue
            if _lib.kitty_screen_cell_is_tab(self._screen._term, x, self._y):
                result.append('\t')
                continue
            n = _lib.kitty_term_cell_text(self._screen._term, x, self._y, cps, _CLUSTER_CAP)
            n = min(n, _CLUSTER_CAP)
            result.append(''.join(chr(cp) for cp in cps[:n]))
        return ''.join(result)

class Callbacks:

    def __init__(self, screen=None):
        self._screen = screen
        self.wtcbuf = b''
        self.titlebuf = []
        self.iconbuf = ''
        self.colorbuf = ''
        self.notifications = []
        self.printbuf = []

    def clear(self):
        self.wtcbuf = b''
        self.titlebuf = []
        self.iconbuf = ''
        self.colorbuf = ''
        self.notifications = []
        self.printbuf = []
        if self._screen is not None:
            self._screen._response_buf.clear()

class Screen:

    def __init__(self, cols=80, rows=5):
        self._cols = cols
        self._rows = rows
        self._response_buf = bytearray()
        self._write_buf = bytearray()
        self._callbacks = Callbacks(self)
        self._resp_fn = REPLY_FN(self._on_response)
        self._term = _lib.kitty_term_new(cols, rows, self._resp_fn, None)
        if not self._term:
            raise RuntimeError('kitty_term_new failed')

    @property
    def _ptr(self):
        return ctypes.cast(self._term, ctypes.c_void_p).value

    def _on_response(self, ud, data, length):
        if data and length:
            self._response_buf.extend(data[:length])
        self._callbacks.wtcbuf = bytes(self._response_buf)

    def write(self, data):
        if data:
            _lib.kitty_term_write(self._term, bytes(data), len(data))

    def test_create_write_buffer(self):
        from fast_data_types import VT_PARSER_BUFFER_SIZE
        return VT_PARSER_BUFFER_SIZE - len(self._write_buf)

    def test_commit_write_buffer(self, data, write_buf):
        from fast_data_types import VT_PARSER_BUFFER_SIZE
        available = VT_PARSER_BUFFER_SIZE - len(self._write_buf)
        n = min(len(data), available)
        if n > 0:
            self._write_buf.extend(data[:n])
        return n

    def test_parse_written_data(self, cd=None):
        data = bytes(self._write_buf)
        self._write_buf.clear()
        from kitty_parser import parse_bytes
        if cd is not None:
            parse_bytes(self, data, cd)
        elif data:
            parse_bytes(self, data, lambda *a: None)

    @property
    def cursor(self):
        return Cursor(self)

    @property
    def callbacks(self):
        return self._callbacks

    @property
    def columns(self):
        return _lib.kitty_term_cols(self._term)

    @property
    def lines(self):
        return _lib.kitty_term_rows(self._term)

    @property
    def margin_top(self):
        return _read_property(self._ptr, 'scroll_top')

    @property
    def margin_bottom(self):
        return _read_property(self._ptr, 'scroll_bottom')

    @property
    def cursor_key_mode(self):
        return _lib.kitty_term_application_cursor(self._term)

    @cursor_key_mode.setter
    def cursor_key_mode(self, value):
        if value:
            self.write(b'\x1b[?1h')
        else:
            self.write(b'\x1b[?1l')

    def line(self, n):
        return Line(self, n)

    def reset(self):
        self.write(b'\x1bc')

    def cursor_move(self, n):
        self.write(b'\x1b[' + str(n).encode() + b'D')

    def close(self):
        if getattr(self, '_term', None):
            _lib.kitty_term_free(self._term)
            self._term = None

    def __del__(self):
        self.close()
