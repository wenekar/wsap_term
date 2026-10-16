"""Test harness for running Kitty's parser.py test suite.

Provides :class:`BaseTest`, :class:`CmdDump`, and re-exports ``parse_bytes``
from ``kitty_parser`` (written by a separate subagent).
"""

import unittest

from kitty_parser import parse_bytes


class CmdDump(list):
    """Collects parser events, matching Kitty's ``CmdDump``."""

    def __call__(self, window_id, *a):
        if a and a[0] == 'bytes':
            return
        if a and a[0] == 'error':
            a = a[1:]
        self.append(tuple(a))

    def get_result(self):
        current = ''
        q = []
        for args in self:
            if args[0] == 'draw':
                current += args[1]
            else:
                if current:
                    q.append(('draw', current))
                    current = ''
                q.append(args)
        if current:
            q.append(('draw', current))
        return tuple(q)


class BaseTest(unittest.TestCase):
    """Base test class matching Kitty's test API."""

    def ae(self, a, b, msg=None):
        self.assertEqual(a, b, msg)

    def create_screen(self, cols=5):
        from screen import Screen
        return Screen(cols, 5)  # 5 rows like Kitty
