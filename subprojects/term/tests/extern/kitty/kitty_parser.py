
_GROUND = 0
_ESC = 1
_CSI = 2
_OSC = 3
_DCS = 4
_APC = 5
_PM = 6
_SOS = 7
_STR_ESC = 8
_CHARSET = 9
_ESC_PCT = 10

_CSI_PARAM = 0
_CSI_PREFIX = 1
_CSI_INTER = 2
_CSI_FINAL = 3
_CSI_OTHER = 4

_MAX_PARAMS = 256

class _CSIState:

    __slots__ = ('prefix', 'intermediate', 'final', 'params', 'nparams', 'subparams',
                 'num_digits', 'mult', 'accum', 'phase')

    def __init__(self):
        self.reset()

    def reset(self):
        self.prefix = 0
        self.intermediate = 0
        self.final = 0
        self.params = [0] * _MAX_PARAMS
        self.nparams = 0
        self.subparams = 0
        self.num_digits = 0
        self.mult = 1
        self.accum = 0
        self.phase = 0

    def add_digit(self, d):
        if self.num_digits >= 17:
            return
        self.accum = self.accum * 10 + d
        self.num_digits += 1

    def commit(self):
        if not self.num_digits:
            return True
        if self.nparams >= _MAX_PARAMS:
            return False
        v = self.mult * self.accum
        self.params[self.nparams] = ((v + 2**31) % 2**32) - 2**31
        self.nparams += 1
        self.num_digits = 0
        self.mult = 1
        self.accum = 0
        return True

class KittyParser:

    def __init__(self):
        self.state = _GROUND
        self.text = ''
        self.utf8_pending = b''
        self.csi = _CSIState()
        self.str_buf = bytearray()
        self.str_type = 0
        self.charset_slot = 0

    def _flush_text(self, cb):
        if self.text:
            cb(0, 'draw', self.text)
            self.text = ''

    def _emit(self, cb, *args):
        self._flush_text(cb)
        cb(0, *args)

    def _kill_pending(self):
        if self.utf8_pending:
            self.utf8_pending = b''
            self.text += '\ufffd'

    @staticmethod
    def _control_event(cb, b):
        if b == 0x0a:
            cb(0, 'screen_linefeed')
        elif b == 0x0d:
            cb(0, 'screen_carriage_return')
        elif b == 0x08:
            cb(0, 'screen_backspace')
        elif b == 0x09:
            cb(0, 'screen_tab')

    def _decode_utf8(self, data, cb):
        buf = self.utf8_pending + bytes(data)
        self.utf8_pending = b''
        i = 0
        n = len(buf)
        while i < n:
            b = buf[i]
            if b < 0x20:
                return i
            if b < 0x80:
                self.text += chr(b)
                i += 1
            elif b < 0xC2:
                self.text += '\ufffd'
                i += 1
            elif b < 0xE0:
                if i + 1 < n:
                    b2 = buf[i + 1]
                    if 0x80 <= b2 <= 0xBF:
                        self.text += chr((b & 0x1F) << 6 | (b2 & 0x3F))
                        i += 2
                    else:
                        self.text += '\ufffd'
                        i += 1
                else:
                    self.utf8_pending = buf[i:]
                    return n
            elif b < 0xF0:
                if i + 2 < n:
                    b2, b3 = buf[i + 1], buf[i + 2]
                    if b == 0xE0:
                        valid = 0xA0 <= b2 <= 0xBF
                    elif b == 0xED:
                        valid = 0x80 <= b2 <= 0x9F
                    else:
                        valid = 0x80 <= b2 <= 0xBF
                    if valid and 0x80 <= b3 <= 0xBF:
                        self.text += chr((b & 0x0F) << 12 | (b2 & 0x3F) << 6 | (b3 & 0x3F))
                        i += 3
                    elif 0x80 <= b2 <= 0xBF:
                        self.text += '\ufffd'
                        i += 2
                    else:
                        self.text += '\ufffd'
                        i += 1
                elif i + 1 < n:
                    b2 = buf[i + 1]
                    if 0x80 <= b2 <= 0xBF:
                        self.utf8_pending = buf[i:]
                        return n
                    else:
                        self.text += '\ufffd'
                        i += 1
                else:
                    self.utf8_pending = buf[i:]
                    return n
            elif b < 0xF5:
                if i + 3 < n:
                    b2, b3, b4 = buf[i + 1], buf[i + 2], buf[i + 3]
                    if b == 0xF0:
                        valid2 = 0x90 <= b2 <= 0xBF
                    elif b == 0xF4:
                        valid2 = 0x80 <= b2 <= 0x8F
                    else:
                        valid2 = 0x80 <= b2 <= 0xBF
                    if valid2 and 0x80 <= b3 <= 0xBF and 0x80 <= b4 <= 0xBF:
                        cp = (b & 0x07) << 18 | (b2 & 0x3F) << 12 | (b3 & 0x3F) << 6 | (b4 & 0x3F)
                        self.text += chr(cp)
                        i += 4
                    elif 0x80 <= b2 <= 0xBF:
                        if 0x80 <= b3 <= 0xBF:
                            self.text += '\ufffd'
                            i += 3
                        else:
                            self.text += '\ufffd'
                            i += 2
                    else:
                        self.text += '\ufffd'
                        i += 1
                else:
                    self.utf8_pending = buf[i:]
                    return n
            else:
                self.text += '\ufffd'
                i += 1
        return n

    def _csi_count(self, idx=0):
        if idx < self.csi.nparams and self.csi.params[idx] > 0:
            return self.csi.params[idx]
        return 1

    def _format_sgr(self):
        if self.csi.nparams == 0:
            return ['0']
        params = self.csi.params[:self.csi.nparams]
        subparams = self.csi.subparams
        groups = []
        current = []
        i = 0
        while i < len(params):
            p = params[i]
            next_has_colon = (i + 1 < len(params)) and bool(subparams & (1 << (i + 1)))
            if p in (38, 48, 58):
                if current:
                    groups.append(';'.join(current))
                    current = []
                sub = [str(p)]
                i += 1
                while i < len(params) and params[i] not in (38, 48, 58):
                    sub.append(str(params[i]))
                    i += 1
                groups.append(':'.join(sub))
            elif next_has_colon:
                if current:
                    groups.append(';'.join(current))
                    current = []
                sub = [str(p)]
                i += 1
                while i < len(params) and (subparams & (1 << i)):
                    sub.append(str(params[i]))
                    i += 1
                groups.append(':'.join(sub))
            else:
                current.append(str(p))
                i += 1
        if current:
            groups.append(';'.join(current))
        return groups

    def _dispatch_csi(self, screen, cb):
        c = self.csi
        final = chr(c.intermediate) if c.intermediate else ''
        if c.intermediate:
            if c.intermediate == ord(' ') and c.prefix == 0:
                if c.final == ord('q'):
                    if c.nparams > 0 and c.params[0] in (3, 4):
                        self._emit(cb, 'screen_set_cursor', c.params[0], ord(' '))
                    elif c.nparams > 0 and c.params[0] in (1, 2, 5, 6):
                        self._emit(cb, 'screen_set_cursor', c.params[0], ord(' '))
                    elif c.nparams == 0:
                        self._emit(cb, 'screen_set_cursor', 0, ord(' '))
                elif c.nparams > 0 and c.params[0] == 3:
                    if c.final == ord('@'):
                        self._emit(cb, 'Shift left escape code not implemented')
                    elif c.final == ord('A'):
                        self._emit(cb, 'Shift right escape code not implemented')
                    elif c.final == ord('S'):
                        self._emit(cb, 'Select presentation directions escape code not implemented')
                return
            if c.intermediate == ord('!') and c.prefix == 0:
                return
            if c.intermediate == ord('$'):
                if c.final == ord('p'):
                    mode = c.params[0] if c.nparams > 0 else 0
                    private = 1 if c.prefix == ord('?') else 0
                    self._emit(cb, 'report_mode_status', mode, private)
                elif c.final == ord('r') and c.prefix == 0:
                    for event_str in self._format_deccara():
                        self._emit(cb, 'deccara', event_str)
                return
            if c.intermediate == ord('*') and c.prefix == 0:
                if c.nparams > 0:
                    self._emit(cb, 'screen_decsace', c.params[0])
                else:
                    self._emit(cb, 'screen_decsace', 0)
                return
            if c.intermediate == ord('+') and c.final == ord('T'):
                self._emit(cb, 'screen_reverse_scroll_and_fill_from_scrollback',
                           self._csi_count())
                return
            return

        f = c.final
        pfx = c.prefix

        if f == ord('@'):
            if c.nparams > 1:
                self._emit(cb, f'CSI code @ has {c.nparams} > 1 parameters')
                return
            if c.nparams == 1 and c.params[0] < 0:
                self._emit(cb, f'CSI code @ is not allowed to have negative parameter ({c.params[0]})')
                return
            self._emit(cb, 'screen_insert_characters', self._csi_count())
            return

        if f == ord('H') or f == ord('f'):
            row = c.params[0] if c.nparams > 0 else 1
            col = c.params[1] if c.nparams > 1 else 1
            self._emit(cb, 'screen_cursor_position', row, col)
            return

        if f == ord('J'):
            mode = c.params[0] if c.nparams > 0 else 0
            private = 1 if pfx == ord('?') else 0
            self._emit(cb, 'screen_erase_in_display', mode, private)
            return

        if f == ord('K'):
            mode = c.params[0] if c.nparams > 0 else 0
            private = 1 if pfx == ord('?') else 0
            self._emit(cb, 'screen_erase_in_line', mode, private)
            return

        if f == ord('h'):
            private = 1 if pfx == ord('?') else 0
            if c.nparams == 0:
                return
            for i in range(c.nparams):
                self._emit(cb, 'screen_set_mode', c.params[i], private)
            return

        if f == ord('l'):
            private = 1 if pfx == ord('?') else 0
            if c.nparams == 0:
                return
            for i in range(c.nparams):
                self._emit(cb, 'screen_reset_mode', c.params[i], private)
            return

        if f == ord('c'):
            if pfx == ord('>'):
                self._emit(cb, 'report_device_attributes', c.params[0] if c.nparams > 0 else 0, 62)
            elif pfx == ord('='):
                self._emit(cb, 'report_device_attributes', c.params[0] if c.nparams > 0 else 0, 61)
            elif pfx == 0:
                self._emit(cb, 'report_device_attributes', c.params[0] if c.nparams > 0 else 0, 0)
            return

        if f == ord('m'):
            for grp in self._format_sgr():
                self._emit(cb, 'select_graphic_rendition', grp)
            return

        if f == ord('n'):
            mode = c.params[0] if c.nparams > 0 else 0
            private = 1 if pfx == ord('?') else 0
            self._emit(cb, 'report_device_status', mode, private)
            return

        if f == ord('r'):
            if pfx:
                return
            if c.nparams == 0:
                self._emit(cb, 'screen_set_margins', 0, 0)
            else:
                top = c.params[0] if c.params[0] > 0 else 1
                bottom = c.params[1] if c.nparams > 1 and c.params[1] > 0 else 0
                self._emit(cb, 'screen_set_margins', top, bottom)
            return

        if f == ord('b'):
            if c.nparams > 1:
                return
            self._emit(cb, 'screen_repeat_character', self._csi_count())
            return

        if f == ord('T'):
            if pfx == 0 and c.nparams <= 1:
                self._emit(cb, 'screen_reverse_scroll', self._csi_count())
            return

        if f == ord('t'):
            if pfx == 0 and c.nparams > 0:
                self._emit(cb, 'screen_report_size', c.params[0], c.params[1] if c.nparams > 1 else 0)
            return

        if f == ord('p'):
            return

    def _format_deccara(self):
        c = self.csi
        if c.nparams == 0:
            return ['0;0;0;0;0']
        params = list(c.params[:c.nparams])
        while len(params) < 4:
            params.append(0)
        subparams = c.subparams
        rect = ';'.join(str(p) for p in params[:4])
        sgr_params = params[4:]
        if not sgr_params:
            return [f'{rect};0']
        groups = []
        current = []
        i = 0
        while i < len(sgr_params):
            p = sgr_params[i]
            next_has_colon = (i + 1 < len(sgr_params)) and bool(subparams & (1 << (i + 5)))
            if p in (38, 48, 58):
                if current:
                    groups.append(';'.join(current))
                    current = []
                sub = [str(p)]
                i += 1
                while i < len(sgr_params) and (subparams & (1 << (i + 4))):
                    sub.append(str(sgr_params[i]))
                    i += 1
                groups.append(':'.join(sub))
            elif next_has_colon:
                if current:
                    groups.append(';'.join(current))
                    current = []
                sub = [str(p)]
                i += 1
                while i < len(sgr_params) and (subparams & (1 << (i + 4))):
                    sub.append(str(sgr_params[i]))
                    i += 1
                groups.append(':'.join(sub))
            else:
                current.append(str(p))
                i += 1
        if current:
            groups.append(';'.join(current))
        return [f'{rect};{g}' for g in groups]

    def _dispatch_osc(self, screen, cb):
        payload = bytes(self.str_buf).decode('utf-8', errors='replace')
        sep = payload.find(';')
        if sep < 0:
            i = 0
            while i < len(payload) and payload[i].isdigit():
                i += 1
            if i == 0:
                code = 0
                rest = payload
            elif i == len(payload):
                code = int(payload)
                rest = ''
            else:
                code = int(payload[:i])
                rest = payload[i:]
        else:
            code_str = payload[:sep]
            rest = payload[sep + 1:]
            try:
                code = int(code_str) if code_str else 0
            except ValueError:
                code = -1

        if code == 0:
            self._emit(cb, 'set_title', rest)
            self._emit(cb, 'set_icon', rest)
        elif code == 1:
            self._emit(cb, 'set_icon', rest)
        elif code == 2:
            self._emit(cb, 'set_title', rest)
        elif code == 8:
            if not rest:
                self._emit(cb, 'set_active_hyperlink', None, None)
            elif ';' not in rest:
                self._emit(cb, 'Ignoring malformed OSC 8 code')
            else:
                params, uri = rest.split(';', 1)
                if not uri:
                    uri = None
                link_id = None
                if params:
                    for kv in params.split(':'):
                        if kv.startswith('id=') and link_id is None:
                            link_id = kv[3:]
                self._emit(cb, 'set_active_hyperlink', link_id, uri)
        elif code == 9:
            self._emit(cb, 'desktop_notify', 9, rest)
        elif code == 99:
            self._emit(cb, 'desktop_notify', 99, rest)
        elif code == 52:
            self._emit(cb, 'clipboard_control', 52, rest)
        elif code == 110:
            self._emit(cb, 'set_dynamic_color', 110, rest)
        elif code == 22:
            self._emit(cb, 'set_dynamic_color', 22, rest)
        else:
            pass

    def _dispatch_dcs(self, screen, cb):
        payload = bytes(self.str_buf)
        if not payload:
            return

        if payload.startswith(b'+q'):
            data = payload[2:].decode('ascii', errors='replace')
            self._emit(cb, 'screen_request_capabilities', 43, data)
            return

        if payload.startswith(b'$q'):
            data = payload[2:].decode('ascii', errors='replace')
            self._emit(cb, 'screen_request_capabilities', ord('$'), data)
            return

        if payload.startswith(b'@kitty-cmd'):
            data = payload[len(b'@kitty-cmd'):].decode('utf-8', errors='replace')
            self._emit(cb, 'handle_remote_cmd', data)
            return

        if payload.startswith(b'@kitty-print|'):
            data = payload[len(b'@kitty-print|'):].decode('ascii', errors='replace')
            self._emit(cb, 'handle_remote_print', data)
            return

        if payload == b'=1s':
            self._emit(cb, 'screen_start_pending_mode')
            return
        if payload == b'=2s':
            self._emit(cb, 'screen_stop_pending_mode')
            return

    def _dispatch_graphics(self, screen, cb):
        payload = bytes(self.str_buf)
        if not payload:
            return

        key_map = {
            'a': 'action', 'd': 'delete_action', 't': 'transmission_type',
            'c': 'compressed', 'f': 'format', 'm': 'more', 'i': 'id',
            's': 'data_width', 'o': 'data_offset', 'w': 'width',
            'h': 'height', 'x': 'x_offset', 'y': 'y_offset',
            'v': 'data_height', 'u': 'data_sz', 'C': 'cursor_movement',
            'n': 'num_cells', 'r': 'num_lines', 'X': 'cell_x_offset',
            'Y': 'cell_y_offset', 'z': 'z_index', 'p': 'placement_id',
            'I': 'image_number', 'q': 'quiet', 'U': 'unicode_placement',
            'P': 'parent_id', 'Q': 'parent_placement_id',
            'O': 'offset_from_parent_x', 'N': 'offset_from_parent_y',
        }

        semi = payload.find(b';')
        if semi >= 0:
            cmd_str = payload[:semi]
            b64_payload = payload[semi + 1:]
        else:
            cmd_str = payload
            b64_payload = b''

        result = {}
        for f in 'action delete_action transmission_type compressed'.split():
            result[f] = b'\0'
        for f in ('format more id data_sz data_offset width height x_offset y_offset '
                  'data_height data_width cursor_movement num_cells num_lines '
                  'cell_x_offset cell_y_offset z_index placement_id image_number '
                  'quiet unicode_placement parent_id parent_placement_id '
                  'offset_from_parent_x offset_from_parent_y').split():
            result[f] = 0
        try:
            from fast_data_types import base64_decode
            result[''] = base64_decode(b64_payload).decode('utf-8', errors='replace')
        except Exception:
            result[''] = b64_payload.decode('ascii', errors='replace')

        i = 0
        n = len(cmd_str)
        while i < n:
            key_start = i
            while i < n and cmd_str[i] != ord('='):
                ch = cmd_str[i]
                if ch == ord(','):
                    self._emit(cb, 'Malformed GraphicsCommand control block, invalid key character: 0x%02x' % ch)
                    return
                if chr(ch) not in key_map:
                    self._emit(cb, 'Malformed GraphicsCommand control block, invalid key character: 0x%02x' % ch)
                    return
                i += 1
            if i >= n:
                self._emit(cb, 'Malformed GraphicsCommand control block, no = after key')
                return
            key = cmd_str[key_start:i].decode('ascii')
            i += 1

            val_start = i
            full_key = key_map.get(key, key)
            is_char_key = full_key in ('action', 'delete_action', 'transmission_type', 'compressed')
            while i < n and cmd_str[i] != ord(','):
                ch = cmd_str[i]
                if ch == ord('='):
                    if i == val_start:
                        self._emit(cb, 'Malformed GraphicsCommand control block, expecting an integer value for key: %s' % key)
                    else:
                        self._emit(cb, 'Malformed GraphicsCommand control block, expecting a , or semi-colon after a value, found: 0x%02x' % ch)
                    return
                if is_char_key:
                    if i > val_start:
                        self._emit(cb, 'Malformed GraphicsCommand control block, expecting a , or semi-colon after a value, found: 0x%02x' % ch)
                        return
                elif not (ord('0') <= ch <= ord('9') or ch == ord('-')):
                    self._emit(cb, 'Malformed GraphicsCommand control block, expecting an integer value for key: %s' % key)
                    return
                i += 1

            val_str = cmd_str[val_start:i].decode('ascii')
            if not val_str:
                self._emit(cb, 'Malformed GraphicsCommand control block, expecting an integer value')
                return
            if is_char_key:
                result[full_key] = val_str.encode('ascii')
            else:
                try:
                    val = int(val_str)
                except ValueError:
                    self._emit(cb, 'Malformed GraphicsCommand control block, expecting an integer value for key: %s' % key)
                    return
                if val > 0xFFFFFFFF:
                    self._emit(cb, 'Malformed GraphicsCommand control block, number is too large')
                    return
                result[full_key] = val

            if i < n and cmd_str[i] == ord(','):
                i += 1

        self._emit(cb, 'graphics_command', result)

    def _dispatch_esc(self, ch, cb):
        if ch == ord('D'):
            self._emit(cb, 'screen_index')
        elif ch == ord('E'):
            self._emit(cb, 'screen_nel')
        elif ch == ord('c'):
            self._emit(cb, 'screen_reset')
        elif ch == ord('M'):
            self._emit(cb, 'screen_reverse_index')
        elif ch == ord('7'):
            self._emit(cb, 'screen_save_cursor')
        elif ch == ord('8'):
            self._emit(cb, 'screen_restore_cursor')
        elif ch == ord('H'):
            self._emit(cb, 'screen_set_tab')
        elif ch == ord('['):
            pass
        elif ch == ord(']'):
            pass
        elif ch == ord('P'):
            pass
        elif ch == ord('_'):
            pass
        elif ch == ord('^'):
            pass
        elif ch == ord('X'):
            pass
        elif ch == ord('(') or ch == ord(')'):
            pass
        elif ch == ord('='):
            self._emit(cb, 'screen_application_keypad', True)
        elif ch == ord('>'):
            self._emit(cb, 'screen_application_keypad', False)
        elif ch == ord('.'):
            pass
        else:
            self._emit(cb, 'Unknown char after ESC: 0x%x' % ch)

    def parse(self, screen, data, cb):
        if isinstance(data, str):
            data = data.encode('utf-8')
        data = bytes(data)

        screen.write(data)

        i = 0
        n = len(data)
        while i < n:
            b = data[i]

            if self.state == _GROUND:
                if b == 0x1b:
                    self._kill_pending()
                    self._flush_text(cb)
                    self.state = _ESC
                    i += 1
                elif b < 0x20:
                    self._kill_pending()
                    self._flush_text(cb)
                    self._control_event(cb, b)
                    i += 1
                else:
                    consumed = self._decode_utf8(data[i:], cb)
                    i += consumed

            elif self.state == _ESC:
                if b == 0x1b:
                    self._emit(cb, 'Unknown char after ESC: 0x1b')
                    i += 1
                elif b < 0x20:
                    self._flush_text(cb)
                    self._control_event(cb, b)
                    self.state = _GROUND
                    i += 1
                elif b == ord('['):
                    self.csi.reset()
                    self.state = _CSI
                    i += 1
                elif b == ord(']'):
                    self.str_buf.clear()
                    self.str_type = _OSC
                    self.state = _OSC
                    i += 1
                elif b == ord('P'):
                    self.str_buf.clear()
                    self.str_type = _DCS
                    self.state = _DCS
                    i += 1
                elif b == ord('_'):
                    self.str_buf.clear()
                    self.str_type = _APC
                    self.state = _APC
                    i += 1
                elif b == ord('^'):
                    self.str_buf.clear()
                    self.str_type = _PM
                    self.state = _PM
                    i += 1
                elif b == ord('X'):
                    self.str_buf.clear()
                    self.str_type = _SOS
                    self.state = _SOS
                    i += 1
                elif b == ord('(') or b == ord(')'):
                    self.charset_slot = 0 if b == ord('(') else 1
                    self.state = _CHARSET
                    i += 1
                elif b == ord('.'):
                    self.state = _CHARSET
                    self.charset_slot = -1
                    i += 1
                elif b == ord('%'):
                    self.state = _ESC_PCT
                    i += 1
                else:
                    self._dispatch_esc(b, cb)
                    self.state = _GROUND
                    i += 1

            elif self.state == _ESC_PCT:
                self.state = _GROUND
                if b in (ord('G'), ord('@'), ord('/')):
                    i += 1
                else:
                    self.text += '%'

            elif self.state == _CHARSET:
                if self.charset_slot == -1:
                    if b == 0x1b:
                        self._emit(cb, 'Unhandled charset related escape code: 0x2e 0x1b')
                        self.state = _GROUND
                    else:
                        self._emit(cb, 'Unhandled charset related escape code: 0x2e 0x%02x' % b)
                        self.state = _GROUND
                    i += 1
                else:
                    self._emit(cb, 'screen_designate_charset', self.charset_slot, b)
                    if self.charset_slot == 1:
                        self._emit(cb, 'screen_change_charset', 1)
                    self.state = _GROUND
                    i += 1

            elif self.state == _CSI:
                c = self.csi
                if b == 0x1b:
                    self._flush_text(cb)
                    self.state = _ESC
                    i += 1
                elif b < 0x20:
                    self._flush_text(cb)
                    self._control_event(cb, b)
                    i += 1
                elif c.phase == 2:
                    if 0x40 <= b <= 0x7e:
                        c.final = b
                        self._dispatch_csi(screen, cb)
                    else:
                        self._emit(cb, 'Invalid character in CSI: %s (0x%x), ignoring the sequence'
                                   % (chr(b) if 0x21 <= b <= 0x7e else '0x%x' % b, b))
                    self.state = _GROUND
                    i += 1
                elif c.phase == 0:
                    if b == ord(';'):
                        if c.nparams < _MAX_PARAMS:
                            c.params[c.nparams] = 0
                            c.nparams += 1
                        c.phase = 1
                    elif 0x30 <= b <= 0x39:
                        c.add_digit(b - 0x30)
                        c.phase = 1
                    elif b in (ord('?'), ord('>'), ord('<'), ord('=')):
                        c.prefix = b
                        c.phase = 1
                    elif 0x20 <= b <= 0x2f:
                        if b == ord('-'):
                            c.mult = -1
                            c.num_digits = 1
                            c.phase = 1
                        else:
                            c.intermediate = b
                            c.phase = 2
                    elif 0x40 <= b <= 0x7e:
                        c.final = b
                        self._dispatch_csi(screen, cb)
                        self.state = _GROUND
                    else:
                        self._emit(cb, 'Invalid character in CSI: %s (0x%x), ignoring the sequence'
                                   % (chr(b) if 0x21 <= b <= 0x7e else '0x%x' % b, b))
                        self.state = _GROUND
                    i += 1
                else:
                    if 0x30 <= b <= 0x39:
                        c.add_digit(b - 0x30)
                    elif b == ord(';'):
                        if not c.num_digits:
                            c.num_digits = 1
                        if not c.commit():
                            self._emit(cb, 'CSI escape code has too many parameters, ignoring it')
                            self.state = _GROUND
                    elif b == ord(':'):
                        if c.commit():
                            c.subparams |= 1 << c.nparams
                        else:
                            self._emit(cb, 'CSI escape code has too many parameters, ignoring it')
                            self.state = _GROUND
                    elif 0x20 <= b <= 0x2f:
                        if b == ord('-') and c.num_digits == 0:
                            c.mult = -1
                            c.num_digits = 1
                        elif c.commit():
                            c.intermediate = b
                            c.phase = 2
                        else:
                            self._emit(cb, 'CSI escape code has too many parameters, ignoring it')
                            self.state = _GROUND
                    elif 0x40 <= b <= 0x7e:
                        if c.num_digits == 1 and c.intermediate == 0 and c.mult == -1:
                            c.num_digits = 0
                            c.intermediate = ord('-')
                        if c.commit():
                            c.final = b
                            self._dispatch_csi(screen, cb)
                        else:
                            self._emit(cb, 'CSI escape code has too many parameters, ignoring it')
                        self.state = _GROUND
                    else:
                        self._emit(cb, 'Invalid character in CSI: %s (0x%x), ignoring the sequence'
                                   % (chr(b) if 0x21 <= b <= 0x7e else '0x%x' % b, b))
                        self.state = _GROUND
                    i += 1

            elif self.state in (_OSC, _DCS, _APC, _PM, _SOS):
                if b == 0x07:
                    self._finish_string(screen, cb)
                    self.state = _GROUND
                    i += 1
                elif b == 0x1b:
                    self.state = _STR_ESC
                    i += 1
                elif b < 0x20:
                    self.str_buf.clear()
                    self.state = _GROUND
                    self._control_event(cb, b)
                    i += 1
                else:
                    self.str_buf.append(b)
                    i += 1

            elif self.state == _STR_ESC:
                if b == ord('\\'):
                    self._finish_string(screen, cb)
                    self.state = _GROUND
                    i += 1
                else:
                    self.str_buf.clear()
                    self.state = _GROUND
                    self.state = _ESC
                    i += 0
                    if b == 0x1b:
                        self._emit(cb, 'Unknown char after ESC: 0x1b')
                    elif b < 0x20:
                        self._control_event(cb, b)
                        self.state = _GROUND
                    else:
                        self._dispatch_esc(b, cb)
                        self.state = _GROUND
                    i += 1

        self._flush_text(cb)

    def _finish_string(self, screen, cb):
        st = self.str_type
        if st == _OSC:
            self._dispatch_osc(screen, cb)
        elif st == _DCS:
            self._dispatch_dcs(screen, cb)
        elif st == _APC:
            payload = bytes(self.str_buf)
            if payload.startswith(b'G'):
                self.str_buf = bytearray(payload[1:])
                self._dispatch_graphics(screen, cb)
            else:
                code = payload[0] if payload else 0
                self._emit(cb, 'Unrecognized APC code: 0x%02x' % code)
        elif st == _PM:
            code = self.str_buf[0] if self.str_buf else 0
            self._emit(cb, 'Unrecognized PM code: 0x%02x' % code)
        elif st == _SOS:
            code = self.str_buf[0] if self.str_buf else 0
            self._emit(cb, 'Unrecognized SOS code: 0x%02x' % code)
        self.str_buf.clear()

def parse_bytes(screen, data, callback=None):
    if callback is None:
        callback = lambda *args: None
    if not hasattr(screen, '_kitty_parser'):
        screen._kitty_parser = KittyParser()

    def wrapped_cb(wid, *args):
        if args and args[0] == 'set_title':
            screen.callbacks.titlebuf.append(args[1])
        elif args and args[0] == 'set_icon':
            screen.callbacks.iconbuf += args[1] if isinstance(args[1], str) else str(args[1], 'utf-8')
        elif args and args[0] == 'desktop_notify':
            screen.callbacks.notifications.append((args[1], args[2]))
        elif args and args[0] == 'set_dynamic_color':
            val = args[2] if isinstance(args[2], str) else str(args[2], 'utf-8')
            screen.callbacks.colorbuf += val
        elif args and args[0] == 'handle_remote_print':
            try:
                from fast_data_types import base64_decode
                screen.callbacks.printbuf.append(base64_decode(args[1]).decode('utf-8'))
            except Exception:
                screen.callbacks.printbuf.append(args[1])
        callback(wid, *args)

    screen._kitty_parser.parse(screen, data, wrapped_cb)
