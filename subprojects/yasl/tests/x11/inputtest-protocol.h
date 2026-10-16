/* Subset of Xorg's xf86-input-inputtest protocol 1.1 used by this test.
 * https://www.x.org/releases/individual/xserver/xorg-server-21.1.25.tar.xz
 *
 * Copyright © 2020 Povilas Kanapickas <povilas@radix.lt>
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of Red Hat
 * not be used in advertising or publicity pertaining to distribution
 * of the software without specific, written prior permission. Red
 * Hat makes no representations about the suitability of this software
 * for any purpose. It is provided "as is" without express or implied
 * warranty.
 *
 * THE AUTHORS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN
 * NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#ifndef YASL_PRIVATE_TEST_INPUTTEST_PROTOCOL_H
#define YASL_PRIVATE_TEST_INPUTTEST_PROTOCOL_H

#include <stdint.h>

enum xf86ITResponseType {
    XF86IT_RESPONSE_SERVER_VERSION,
    XF86IT_RESPONSE_SYNC_FINISHED,
};

typedef struct {
    uint32_t length;
    enum xf86ITResponseType type;
} xf86ITResponseHeader;

typedef struct {
    xf86ITResponseHeader header;
    uint16_t major, minor;
} xf86ITResponseServerVersion;

enum xf86ITEventType {
    XF86IT_EVENT_CLIENT_VERSION,
    XF86IT_EVENT_WAIT_FOR_SYNC,
    XF86IT_EVENT_MOTION,
    XF86IT_EVENT_PROXIMITY,
    XF86IT_EVENT_BUTTON,
};

typedef struct {
    uint32_t length;
    enum xf86ITEventType type;
} xf86ITEventHeader;

typedef struct {
    uint32_t has_unaccelerated;
    uint8_t mask[8];
    double valuators[64];
    double unaccelerated[64];
} xf86ITValuatorData;

typedef struct {
    xf86ITEventHeader header;
    uint16_t major, minor;
} xf86ITEventClientVersion;

typedef struct { xf86ITEventHeader header; } xf86ITEventWaitForSync;

typedef struct {
    xf86ITEventHeader header;
    uint32_t is_absolute;
    xf86ITValuatorData valuators;
} xf86ITEventMotion;

typedef struct {
    xf86ITEventHeader header;
    uint32_t is_prox_in;
    xf86ITValuatorData valuators;
} xf86ITEventProximity;

typedef struct {
    xf86ITEventHeader header;
    int32_t is_absolute;
    int32_t button;
    uint32_t is_press;
    xf86ITValuatorData valuators;
} xf86ITEventButton;

#endif
