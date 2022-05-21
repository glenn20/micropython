/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017-2020 Nick Moore
 * Copyright (c) 2018 shawwwn <shawwwn1@gmail.com>
 * Copyright (c) 2020-2021 Glenn Moloney @glenn20
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "py/runtime.h"

#include "c_types.h"
#include "espnow.h"

#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/qstr.h"
#include "py/objstr.h"
#include "py/objarray.h"
#include "py/stream.h"
#include "esp_espnow.h"

#include "mpconfigport.h"

// Reduce code size by declaring all ring-buffer functions static
#define RING_BUFFER_INCLUDE_AS_STATIC
#include "shared/runtime/ring_buffer.h"
// Include the "static" ring_buffer function defs in this file.
// This reduces code size on the ESP8266 by 88 bytes.
#include "shared/runtime/ring_buffer.c"

// For the esp8266
#define ESP_NOW_MAX_DATA_LEN (250)
#define ESP_NOW_KEY_LEN (16)
#define ESP_NOW_ETH_ALEN (6)
#define ESP_NOW_SEND_SUCCESS (0)
#define ESP_ERR_ESPNOW_NO_MEM (-77777)
#define ESP_OK (0)
#define ESP_NOW_MAX_TOTAL_PEER_NUM (20)
#define ESP_NOW_MAX_ENCRYPT_PEER_NUM (6)
typedef int esp_err_t;

// Relies on gcc Variadic Macros and Statement Expressions
#define NEW_TUPLE(...) \
    ({mp_obj_t _z[] = {__VA_ARGS__}; mp_obj_new_tuple(MP_ARRAY_SIZE(_z), _z); })

static const uint8_t ESPNOW_MAGIC = 0x99;

// Use this for peeking at the header of the next packet in the buffer.
typedef struct {
    uint8_t magic;              // = ESPNOW_MAGIC
    uint8_t msg_len;            // Length of the message
} __attribute__((packed)) espnow_hdr_t;

// ESPNow packet format for the receive buffer.
typedef struct {
    espnow_hdr_t hdr;           // The header
    uint8_t peer[6];            // Peer address
    uint8_t msg[0];             // Message is up to 250 bytes
} __attribute__((packed)) espnow_pkt_t;

// The maximum length of an espnow packet (bytes)
static const size_t MAX_PACKET_LEN = (
    sizeof(espnow_pkt_t) + ESP_NOW_MAX_DATA_LEN);

// Enough for 2 full-size packets: 2 * (6 + 2 + 250) = 516 bytes
// Will allocate an additional 7 bytes for buffer overhead
static const size_t DEFAULT_RECV_BUFFER_SIZE = (
    2 * (sizeof(espnow_pkt_t) + ESP_NOW_MAX_DATA_LEN));

// Default timeout (millisec) to wait for incoming ESPNow messages (5 minutes).
static const size_t DEFAULT_RECV_TIMEOUT_MS = (5 * 60 * 1000);

// Number of milliseconds to wait (mp_hal_wait_ms()) in each loop
// while waiting for send or receive packet.
// Needs to be >15ms to permit yield to other tasks.
#define BUSY_WAIT_MS (25)

// The data structure for the espnow_singleton.
typedef struct _esp_espnow_obj_t {
    mp_obj_base_t base;
    buffer_t recv_buffer;           // A buffer for received packets
    espnow_pkt_t *irecv_packet;     // Storage for packet return from irecv()
    mp_obj_tuple_t *irecv_tuple;    // Preallocated tuple for irecv()
    mp_obj_tuple_t *none_tuple;     // Preallocated tuple for irecv()
    size_t recv_timeout_ms;         // Timeout for recv()/irecv()/poll()/ipoll()
    size_t tx_packets;              // Count of sent packets
    volatile size_t tx_responses;   // # of sent packet responses received
    volatile size_t tx_failures;    // # of sent packet responses failed
    // mp_obj_t recv_cb;               // Callback when a packet is received
} esp_espnow_obj_t;

// Initialised below.
const mp_obj_type_t esp_espnow_type;

// ### Initialisation and Config functions
//

static void check_esp_err(int e) {
    if (e != 0) {
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("ESP-Now Unknown Error 0x%04x"), e));
    }
}

#define INITIALISED         (1)

// Return a pointer to the ESPNow module singleton
// If state == INITIALISED check the device has been initialised.
// Raises OSError if not initialised and state == INITIALISED.
static esp_espnow_obj_t *_get_singleton(int state) {
    return MP_STATE_PORT(espnow_singleton);
}

// Allocate and initialise the ESPNow module as a singleton.
// Returns the initialised espnow_singleton.
STATIC mp_obj_t espnow_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {

    // The espnow_singleton must be defined in MICROPY_PORT_ROOT_POINTERS
    // (see mpconfigport.h) to prevent memory allocated here from being
    // garbage collected.
    // NOTE: on soft reset the espnow_singleton MUST be set to NULL and the
    // ESP-NOW functions de-initialised (see main.c).
    esp_espnow_obj_t *self = MP_STATE_PORT(espnow_singleton);
    if (self != NULL) {
        return self;
    }
    self = m_malloc0(sizeof(esp_espnow_obj_t));
    self->base.type = &esp_espnow_type;

    // Allocate and initialise the "callee-owned" tuple for irecv().
    espnow_pkt_t *pkt = (espnow_pkt_t *)m_malloc0(MAX_PACKET_LEN);
    // pkt->magic = ESPNOW_MAGIC;
    // pkt->msg_len = 0;
    // Build a tuple of byte strings. The first byte string points to
    // peer mac address and the second to message in the packet buffer.
    mp_obj_str_t *peer = MP_OBJ_TO_PTR(
        mp_obj_new_bytes(NULL, ESP_NOW_ETH_ALEN));
    peer->data = pkt->peer;
    mp_obj_str_t *msg = MP_OBJ_TO_PTR(
        mp_obj_new_bytes(NULL, ESP_NOW_MAX_DATA_LEN));
    msg->data = pkt->msg;
    self->irecv_packet = pkt;
    self->irecv_tuple = NEW_TUPLE(
        MP_OBJ_FROM_PTR(peer),
        MP_OBJ_FROM_PTR(msg));
    self->none_tuple = NEW_TUPLE(
        mp_const_none,
        mp_const_none);
    self->recv_timeout_ms = DEFAULT_RECV_TIMEOUT_MS;

    // Set the global singleton pointer for the espnow protocol.
    MP_STATE_PORT(espnow_singleton) = self;

    return self;
}

// Forward declare the send and recv ESPNow callbacks
STATIC void send_cb(uint8_t *mac_addr, uint8_t status);

STATIC void recv_cb(uint8_t *mac_addr, uint8_t *data, uint8_t len);

// ESPNow.init(): Initialise the data buffers and ESP-NOW functions.
// Initialise the Espressif ESPNOW software stack, register callbacks and
// allocate the recv data buffers.
// Returns None.
STATIC mp_obj_t espnow_init(size_t n_args, const mp_obj_t *args) {
    esp_espnow_obj_t *self = _get_singleton(0);
    if (n_args > 2 && (args[2] != mp_const_none)) {
        self->recv_timeout_ms = mp_obj_get_int(args[2]);
    }
    // if (n_args > 3) {
    //     self->recv_cb = args[3];
    // }
    if (self->recv_buffer == NULL) {    // Already initialised
        self->recv_buffer = buffer_init(
            (n_args > 1 && (args[1] != mp_const_none))
                ? mp_obj_get_int(args[1])
                : DEFAULT_RECV_BUFFER_SIZE
            );
        check_esp_err(esp_now_init());
        esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
        esp_now_register_recv_cb(recv_cb);
        esp_now_register_send_cb(send_cb);
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espnow_init_obj, 1, 4, espnow_init);

// ESPNow.deinit(): De-initialise the ESPNOW software stack, disable callbacks
// and deallocate the recv data buffers.
// Note: this function is called from main.c:mp_task() to cleanup before soft
// reset, so cannot be declared STATIC and must guard against self == NULL;.
mp_obj_t espnow_deinit(mp_obj_t _) {
    esp_espnow_obj_t *self = _get_singleton(0);
    if (self != NULL && self->recv_buffer != NULL) {
        esp_now_unregister_recv_cb();
        //esp_now_unregister_send_cb();
        esp_now_deinit();
        buffer_release(self->recv_buffer);
        self->recv_buffer = NULL;
        self->tx_packets = self->tx_responses;
    }
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(espnow_deinit_obj, espnow_deinit);

// ### The ESP_Now send and recv callback routines
//

// Callback triggered when a sent packet is acknowledged by the peer (or not).
// Just count the number of responses and number of failures.
// These are used in the send()/write() logic.
STATIC void send_cb(uint8_t *mac_addr, uint8_t status) {
    esp_espnow_obj_t *self = MP_STATE_PORT(espnow_singleton);
    self->tx_responses++;
    if (status != ESP_NOW_SEND_SUCCESS) {
        self->tx_failures++;
    }
}

// Callback triggered when an ESP-Now packet is received.
// Write the peer MAC address and the message into the recv_buffer as an
// ESPNow packet.
// If the buffer is full, drop the message and increment the dropped count.
// Schedules the user callback if one has been registered (ESPNow.config()).
STATIC void recv_cb(
    uint8_t *mac_addr, uint8_t *msg, uint8_t msg_len) {

    esp_espnow_obj_t *self = _get_singleton(0);
    buffer_t buf = self->recv_buffer;
    if (sizeof(espnow_pkt_t) + msg_len >= buffer_free(buf)) {
        return;
    }
    espnow_hdr_t header;
    header.magic = ESPNOW_MAGIC;
    header.msg_len = msg_len;

    buffer_put(buf, &header, sizeof(header));
    buffer_put(buf, mac_addr, ESP_NOW_ETH_ALEN);
    buffer_put(buf, msg, msg_len);
    // if (self->recv_cb != mp_const_none) {
    //     mp_sched_schedule(self->recv_cb, self);
    // }
}

// ### Handling espnow packets in the recv buffer
//

// ESPNow.irecv([timeout]):
// Like ESPNow.recv() but returns a "callee-owned" tuple of byte strings.
// This provides an allocation-free way to read successive messages.
// Beware: the tuple and bytestring storage is re-used between all calls
// to irecv().
// Takes an optional "timeout" argument in milliseconds.
// Default timeout is set with ESPNow.config(timeout=milliseconds).
// Returns (None, None) on timeout.
STATIC mp_obj_t espnow_irecv(size_t n_args, const mp_obj_t *args) {
    esp_espnow_obj_t *self = _get_singleton(INITIALISED);

    size_t timeout_ms = (
        (n_args > 1) ? mp_obj_get_int(args[1]) : self->recv_timeout_ms);

    // Get the peer and msg byte strings from the callee-owned tuple
    mp_obj_str_t *peer = MP_OBJ_TO_PTR(self->irecv_tuple->items[0]);
    mp_obj_str_t *msg = MP_OBJ_TO_PTR(self->irecv_tuple->items[1]);
    msg->len = msg->hash = peer->hash = 0;

    // Read the packet header from the incoming buffer
    espnow_pkt_t *pkt = self->irecv_packet;
    if (!buffer_recv(self->recv_buffer, pkt, sizeof(*pkt), timeout_ms)) {
        return self->none_tuple; // Timeout waiting for packet
    }
    // Check the message packet header format
    if (pkt->hdr.magic != ESPNOW_MAGIC ||
        pkt->hdr.msg_len > ESP_NOW_MAX_DATA_LEN) {
        mp_raise_ValueError(MP_ERROR_TEXT("ESP-Now: Bad packet"));
    }
    // Now read the message into the byte string.
    if (!buffer_get(self->recv_buffer, (byte *)msg->data, pkt->hdr.msg_len)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Buffer error"));
    }
    msg->len = pkt->hdr.msg_len;
    return MP_OBJ_FROM_PTR(self->irecv_tuple);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espnow_irecv_obj, 1, 2, espnow_irecv);

// Used by _do_espnow_send() for sends() with sync==True.
// Wait till all pending sent packet responses have been received.
// ie. self->tx_responses == self->tx_packets.
// Return the number of responses where status != ESP_NOW_SEND_SUCCESS.
static void _wait_for_pending_responses(esp_espnow_obj_t *self) {
    // Note: the send timeout is just a fallback - in normal operation
    // we should never reach that timeout.
    for (int i = 0; i < 90 && self->tx_responses < self->tx_packets; i++) {
        // Won't yield unless delay > portTICK_PERIOD_MS (10ms)
        mp_hal_delay_ms(BUSY_WAIT_MS);
    }
}

// Return C pointer to byte memory string/bytes/bytearray in obj.
// Raise ValueError if the length does not match expected len.
static uint8_t *_get_bytes_len(mp_obj_t obj, size_t len) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(obj, &bufinfo, MP_BUFFER_READ);
    if (bufinfo.len != len) {
        mp_raise_ValueError(MP_ERROR_TEXT("wrong length"));
    }
    return (uint8_t *)bufinfo.buf;
}

// ESPNow.send(peer_addr, message, [sync (=true)])
// ESPNow.send(message)
// Send a message to the peer's mac address. Optionally wait for a response.
// If peer_addr == None, send to all registered peers.
// If sync == True, wait for response after sending.
// Returns:
//   True  if sync==False and message sent successfully.
//   True  if sync==True and message is received successfully by all recipients
//   False if sync==True and message is not received by at least one recipient
// Raises: EAGAIN if the internal espnow buffers are full.
STATIC mp_obj_t espnow_send(size_t n_args, const mp_obj_t *args) {
    esp_espnow_obj_t *self = _get_singleton(INITIALISED);

    // Get a pointer to the buffer of obj
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_READ);

    // Bugfix: esp_now_send() generates a panic if message buffer points
    // to an address in ROM (eg. a statically interned QSTR).
    // See https://github.com/glenn20/micropython-espnow-images/issues/7
    // Fix: if message is in ROM, copy to a temp buffer on the stack.
    char temp[bufinfo.len];
    byte *p = (byte *)bufinfo.buf;
    if (p < MP_STATE_MEM(gc_pool_start) || MP_STATE_MEM(gc_pool_end) < p) {
        // If buffer is not in GC pool copy from ROM to stack
        memcpy(temp, bufinfo.buf, bufinfo.len);
        bufinfo.buf = temp;
    }

    bool sync = (n_args > 3) ? mp_obj_get_int(args[3]) : true;
    if (sync) {
        // If the last call was sync==False there may be outstanding responses
        // still to be received (possible many if we just had a burst of
        // unsync send()s). We need to wait for all pending responses if this
        // call has sync=True.
        // Flush out any pending responses.
        _wait_for_pending_responses(self);
    }
    int saved_failures = self->tx_failures;

    check_esp_err(esp_now_send(
        _get_bytes_len(args[1], ESP_NOW_ETH_ALEN),
        bufinfo.buf, bufinfo.len));
    self->tx_packets++;
    if (sync) {
        // Wait for message to be received by peer
        _wait_for_pending_responses(self);
    }
    return (sync && self->tx_failures != saved_failures)
            ? mp_const_false
            : mp_const_true;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espnow_send_obj, 3, 4, espnow_send);

// ### Peer Management Functions
//

// Set the ESP-NOW Primary Master Key (pmk) (for encrypted communications).
// Raise OSError if ESP-NOW functions are not initialised.
// Raise ValueError if key is not a bytes-like object exactly 16 bytes long.
STATIC mp_obj_t espnow_set_pmk(mp_obj_t _, mp_obj_t key) {
    check_esp_err(esp_now_set_kok(
        _get_bytes_len(key, ESP_NOW_KEY_LEN), ESP_NOW_KEY_LEN));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(espnow_set_pmk_obj, espnow_set_pmk);

// ESPNow.add_peer(peer_mac, [lmk, [channel, [ifidx, [encrypt]]]]) or
// ESPNow.add_peer(peer_mac, [lmk=b'0123456789abcdef'|b''|None|False],
//          [channel=1..11|0], [ifidx=0|1], [encrypt=True|False])
// Positional args set to None will be left at defaults.
// Raise OSError if ESPNow.init() has not been called.
// Raise ValueError if mac or LMK are not bytes-like objects or wrong length.
// Raise TypeError if invalid keyword args or too many positional args.
// Return None.
STATIC mp_obj_t espnow_add_peer(
    size_t n_args, const mp_obj_t *args) {

    check_esp_err(
        esp_now_add_peer(
            _get_bytes_len(args[1], ESP_NOW_ETH_ALEN),
            ESP_NOW_ROLE_COMBO,
            (n_args > 3) ? mp_obj_get_int(args[3]) : 0,
            (n_args > 2) ? _get_bytes_len(args[2], ESP_NOW_KEY_LEN) : NULL,
            ESP_NOW_KEY_LEN));

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espnow_add_peer_obj, 2, 4, espnow_add_peer);

// ESPNow.del_peer(peer_mac): Unregister peer_mac.
// Raise OSError if ESPNow.init() has not been called.
// Raise ValueError if peer is not a bytes-like objects or wrong length.
// Return None.
STATIC mp_obj_t espnow_del_peer(mp_obj_t _, mp_obj_t peer) {
    check_esp_err(esp_now_del_peer(_get_bytes_len(peer, ESP_NOW_ETH_ALEN)));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(espnow_del_peer_obj, espnow_del_peer);

STATIC const mp_rom_map_elem_t esp_espnow_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&espnow_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit), MP_ROM_PTR(&espnow_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR_irecv), MP_ROM_PTR(&espnow_irecv_obj) },
    { MP_ROM_QSTR(MP_QSTR_send), MP_ROM_PTR(&espnow_send_obj) },

    // Peer management functions
    { MP_ROM_QSTR(MP_QSTR_set_pmk), MP_ROM_PTR(&espnow_set_pmk_obj) },
    { MP_ROM_QSTR(MP_QSTR_add_peer), MP_ROM_PTR(&espnow_add_peer_obj) },
    { MP_ROM_QSTR(MP_QSTR_del_peer), MP_ROM_PTR(&espnow_del_peer_obj) },
};
STATIC MP_DEFINE_CONST_DICT(esp_espnow_locals_dict, esp_espnow_locals_dict_table);

STATIC const mp_rom_map_elem_t espnow_globals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_espnow) },
    { MP_ROM_QSTR(MP_QSTR_ESPNow), MP_ROM_PTR(&esp_espnow_type) },
};
STATIC MP_DEFINE_CONST_DICT(espnow_globals_dict, espnow_globals_dict_table);

const mp_obj_type_t esp_espnow_type = {
    { &mp_type_type },
    .name = MP_QSTR_ESPNow,
    .make_new = espnow_make_new,
    .locals_dict = (mp_obj_t)&esp_espnow_locals_dict,
};

const mp_obj_module_t mp_module_esp_espnow = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&espnow_globals_dict,
};
