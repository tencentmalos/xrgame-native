#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Submit one IME commit to the nested client via an ephemeral X11 selection.

Text arrives on stdin, is never logged or written to disk/Android clipboard,
and the selection disappears when this bounded helper exits. No CEF debugging
port or access to Steam authentication is needed. Only the client uses this.
"""
import ctypes as c
import os
import select
import sys
import time


class Request(c.Structure):
    _fields_ = [('type', c.c_int), ('serial', c.c_ulong), ('send_event', c.c_int),
                ('display', c.c_void_p), ('owner', c.c_ulong), ('requestor', c.c_ulong),
                ('selection', c.c_ulong), ('target', c.c_ulong), ('property', c.c_ulong), ('time', c.c_ulong)]


class Selection(c.Structure):
    _fields_ = [('type', c.c_int), ('serial', c.c_ulong), ('send_event', c.c_int),
                ('display', c.c_void_p), ('requestor', c.c_ulong), ('selection', c.c_ulong),
                ('target', c.c_ulong), ('property', c.c_ulong), ('time', c.c_ulong)]


class Property(c.Structure):
    _fields_ = [('type', c.c_int), ('serial', c.c_ulong), ('send_event', c.c_int),
                ('display', c.c_void_p), ('window', c.c_ulong), ('atom', c.c_ulong),
                ('time', c.c_ulong), ('state', c.c_int)]


class Event(c.Union):
    _fields_ = [('type', c.c_int), ('request', Request), ('selection', Selection), ('property', Property), ('pad', c.c_long * 24)]


def paste(text):
    if not text or len(text) > 16384 or b'\0' in text:
        return 64
    text.decode('utf-8', errors='strict')
    x = c.CDLL('libX11.so.6')
    ptr, ulong, integer = c.c_void_p, c.c_ulong, c.c_int
    def bind(lib, name, result, *args):
        f = getattr(lib, name); f.restype = result; f.argtypes = list(args); return f
    open_display = bind(x, 'XOpenDisplay', ptr, c.c_char_p)
    close = bind(x, 'XCloseDisplay', integer, ptr)
    root = bind(x, 'XDefaultRootWindow', ulong, ptr)
    window = bind(x, 'XCreateSimpleWindow', ulong, ptr, ulong, integer, integer, c.c_uint, c.c_uint, c.c_uint, ulong, ulong)
    atom = bind(x, 'XInternAtom', ulong, ptr, c.c_char_p, integer)
    owner = bind(x, 'XSetSelectionOwner', integer, ptr, ulong, ulong, ulong)
    property_change = bind(x, 'XChangeProperty', integer, ptr, ulong, ulong, ulong, integer, integer, ptr, integer)
    send = bind(x, 'XSendEvent', integer, ptr, ulong, integer, c.c_long, c.POINTER(Event))
    pending = bind(x, 'XPending', integer, ptr)
    next_event = bind(x, 'XNextEvent', integer, ptr, c.POINTER(Event))
    select_input = bind(x, 'XSelectInput', integer, ptr, ulong, c.c_long)
    sync = bind(x, 'XSync', integer, ptr, integer)
    fd = bind(x, 'XConnectionNumber', integer, ptr)
    display = open_display(b':1')
    if not display: return 69
    try:
        w = window(display, root(display), 0, 0, 1, 1, 0, 0, 0)
        clipboard, utf8, targets, atom_type, string, plain, text_atom = [atom(display, name, 0) for name in (b'CLIPBOARD', b'UTF8_STRING', b'TARGETS', b'ATOM', b'STRING', b'text/plain;charset=utf-8', b'TEXT')]
        owner(display, clipboard, w, 0)
        sync(display, 0)
        # The Android X input path owns keyboard focus. XTEST on Xephyr's
        # separate virtual device does not reach Steam's focused input reliably.
        print('ready', flush=True)
        deadline = time.monotonic() + 3
        consumed = None
        consumed_at = None
        while time.monotonic() < deadline:
            if consumed_at is not None and time.monotonic() - consumed_at >= .3:
                return 0
            if not pending(display):
                select.select([fd(display)], [], [], min(.1, max(0, deadline - time.monotonic())))
                continue
            event = Event(); next_event(display, c.byref(event))
            if event.type == 28 and consumed == (event.property.window, event.property.atom) and event.property.state == 1:
                # Chromium may prefetch on owner-change before processing Ctrl+V.
                # Continue serving repeat requests until the paste has drained.
                consumed_at = time.monotonic()
            if event.type == 29: return 75  # Another owner superseded this commit.
            if event.type != 30: continue
            r = event.request
            if consumed_at is not None: consumed_at = time.monotonic()
            prop = r.property or r.target
            delivered = r.target in (utf8, plain, text_atom) or (r.target == string and text.isascii())
            if r.selection != clipboard: prop = 0
            elif r.target == targets:
                formats = [targets, utf8, plain, text_atom] + ([string] if text.isascii() else [])
                supported = (ulong * len(formats))(*formats)
                property_change(display, r.requestor, prop, atom_type, 32, 0, c.cast(supported, ptr), len(formats))
            elif delivered:
                select_input(display, r.requestor, 1 << 22)
                data = c.create_string_buffer(text)
                property_change(display, r.requestor, prop, string if r.target == string else utf8, 8, 0, c.cast(data, ptr), len(text))
            else: prop = 0
            reply = Event()
            reply.selection = Selection(31, 0, 1, display, r.requestor, r.selection, r.target, prop, r.time)
            send(display, r.requestor, 0, 0, c.byref(reply)); sync(display, 0)
            if delivered and prop: consumed = (r.requestor, prop)
        return 74
    finally:
        close(display)


if __name__ == '__main__':
    # No exception diagnostic may include submitted text.
    try: result = paste(sys.stdin.buffer.read(16385))
    except Exception: result = 70
    sys.exit(result)
