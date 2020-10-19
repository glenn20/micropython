/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017-2020 Nick Moore
 * Copyright (c) 2018 shawwwn <shawwwn1@gmail.com>
 * Copyright (c) 2020 Glenn Moloney @glenn20
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

#if MICROPY_ESP8266_ESPNOW

#include "c_types.h"
#include "espnow.h"

#include "py/mphal.h"
#include "py/mperrno.h"
#include "py/qstr.h"
#include "py/objstr.h"
#include "py/objarray.h"
#include "py/stream.h"

#include "mpconfigport.h"

// Reduce code size by declaring all ring-buffer functions static
#define RING_BUFFER_INCLUDE_AS_STATIC
#include "ring_buffer.h"
// Include the "static" ring_buffer function defs in this file.
// This reduces code size on the ESP8266 by 88 bytes.
#include "ring_buffer.c"

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
// Make our own for the esp8266
typedef struct esp_now_peer_info {
    uint8_t peer_addr[ESP_NOW_ETH_ALEN];
    uint8_t lmk[ESP_NOW_KEY_LEN];
    uint8_t channel;
    uint8_t ifidx;
    bool encrypt;
} esp_now_peer_info_t;

static const uint8_t ESPNOW_MAGIC = 0x99;

// Size and offset of packets in the recv_buffer.
// static const size_t ESPNOW_MAGIC_OFFSET = 0;
// static const size_t ESPNOW_MSGLEN_OFFSET = 1;
static const size_t ESPNOW_PEER_OFFSET = 2;
#define ESPNOW_MSG_OFFSET (ESPNOW_PEER_OFFSET + ESP_NOW_ETH_ALEN)
#define ESPNOW_HDR_LEN ESPNOW_MSG_OFFSET
#define ESPNOW_MAX_PKT_LEN (ESPNOW_HDR_LEN + ESP_NOW_MAX_DATA_LEN)

#define DEFAULT_RECV_BUFFER_SIZE \
    (2 * (ESPNOW_HDR_LEN + ESP_NOW_MAX_DATA_LEN))
// Enough for 2 full-size packets: 2 * (6 + 2 + 250) = 516 bytes
// Will allocate an additional 9 bytes for buffer overhead

static const size_t DEFAULT_RECV_TIMEOUT = (5 * 60 * 1000);
// 5 mins - in milliseconds

// Number of milliseconds to wait (mp_hal_wait_ms()) in each loop
// while waiting for send or receive packet.
// Needs to be >15ms to permit yield to other tasks.
static const size_t BUSY_WAIT_MS = 25;  // milliseconds

typedef struct _esp_espnow_obj_t {
    mp_obj_base_t base;

    int initialised;
    buffer_t recv_buffer;       // A buffer for received packets
    size_t sent_packets;
    volatile size_t recv_packets;
    size_t dropped_rx_pkts;
    size_t recv_timeout;        // Timeout for recv_wait()/poll()
    volatile size_t sent_responses;
    volatile size_t sent_successes;
    mp_obj_tuple_t *irecv_tuple; // A saved tuple for efficient return of data
    mp_obj_array_t *irecv_peer;
    mp_obj_array_t *irecv_msg;
} esp_espnow_obj_t;

// Initialised below.
const mp_obj_type_t esp_espnow_type;

// A static pointer to the espnow singleton
STATIC esp_espnow_obj_t *espnow_singleton = NULL;

// ### Consolidated buffer Handling Support functions
//
// Forward declare buffer and packet handling functions defined at end of file
// in lieu of splitting out to another esp_espnow_buf.[ch].
#define BUF_EMPTY (-1)      // The ring buffer is empty - flag EAGAIN
#define BUF_ERROR (-2)      // An error in the packet format
#define BUF_ESIZE (-3)      // Packet is too big for readout bufsize

// Put received data into the buffer (called from recv_cb()).
static void _buf_put_recv_data(buffer_t buf, const uint8_t *mac,
    const uint8_t *data, size_t msg_len);
// Get the peer mac address and message from a packet in the buffer.
static bool _buf_get_recv_data(buffer_t buf, uint8_t *mac, uint8_t *msg, int msg_len);
// Peek at the next recv packet in the ring buffer to get the message length.
static int _buf_peek_message_length(buffer_t buf);
// Get a pointer to a str, bytes or bytearray object's data
static uint8_t *_get_bytes_len(mp_obj_t obj, size_t size);

// ### Initialisation and Config functions
//

STATIC mp_obj_t espnow_make_new(const mp_obj_type_t *type, size_t n_args,
    size_t n_kw, const mp_obj_t *all_args) {

    if (espnow_singleton != NULL) {
        return espnow_singleton;
    }
    esp_espnow_obj_t *self = m_malloc0(sizeof(esp_espnow_obj_t));
    self->base.type = &esp_espnow_type;
    self->recv_timeout = DEFAULT_RECV_TIMEOUT;

    // Allocate and initialise the "callee-owned" tuple for irecv().
    uint8_t msg_tmp[ESP_NOW_MAX_DATA_LEN], peer_tmp[ESP_NOW_ETH_ALEN];
    self->irecv_peer = MP_OBJ_TO_PTR(mp_obj_new_bytearray(ESP_NOW_ETH_ALEN, peer_tmp));
    self->irecv_msg = MP_OBJ_TO_PTR(mp_obj_new_bytearray(ESP_NOW_MAX_DATA_LEN, msg_tmp));
    self->irecv_tuple = mp_obj_new_tuple(2, NULL);
    self->irecv_tuple->items[0] = MP_OBJ_FROM_PTR(self->irecv_peer);
    self->irecv_tuple->items[1] = MP_OBJ_FROM_PTR(self->irecv_msg);

    // Set the global singleton pointer for the espnow protocol.
    espnow_singleton = self;

    return self;
}

static void check_esp_err(int e) {
    if (e != 0) {
        nlr_raise(mp_obj_new_exception_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("ESP-Now Unknown Error 0x%04x"), e));
    }
}

STATIC void
send_cb(uint8_t *mac_addr, uint8_t status);

STATIC void
recv_cb(uint8_t *mac_addr, uint8_t *data, uint8_t len);

// ESPNow.init()
// Initialise the Espressif ESPNOW software stack, register callbacks and
// allocate the recv data buffers.
STATIC mp_obj_t espnow_init(size_t n_args, const mp_obj_t *args) {
    esp_espnow_obj_t *self = espnow_singleton;
    if (self->initialised) {
        return mp_const_none;
    }
    self->recv_buffer = buffer_init(
        (n_args > 1) ? mp_obj_get_int(args[1]) : DEFAULT_RECV_BUFFER_SIZE
        );

    self->initialised = 1;
    check_esp_err(esp_now_init());
    esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
    esp_now_register_recv_cb(recv_cb);
    esp_now_register_send_cb(send_cb);

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espnow_init_obj, 1, 2, espnow_init);

// ESPNow.deinit()
// De-initialise the Espressif ESPNOW software stack, disable callbacks and
// deallocate the recv data buffers.
STATIC mp_obj_t espnow_deinit(mp_obj_t _) {
    esp_espnow_obj_t *self = espnow_singleton;
    if (!self->initialised) {
        return mp_const_none;
    }
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    self->initialised = 0;
    buffer_release(self->recv_buffer);
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(espnow_deinit_obj, espnow_deinit);

// set_pmk(primary_key)
STATIC mp_obj_t espnow_set_pmk(mp_obj_t _, mp_obj_t key) {
    check_esp_err(esp_now_set_kok(
        _get_bytes_len(key, ESP_NOW_KEY_LEN), ESP_NOW_KEY_LEN));
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_2(espnow_set_pmk_obj, espnow_set_pmk);

// ### The ESP_Now send and recv callback routines
//

// Triggered when receipt of a sent packet is acknowledged (or not)
// If required, save the response in the ring buffer
STATIC void send_cb(uint8_t *mac_addr, uint8_t status) {
    espnow_singleton->sent_responses++;
    if (status == ESP_NOW_SEND_SUCCESS) {
        espnow_singleton->sent_successes++;
    }
}

// Triggered when an ESP-Now packet is received.
STATIC void recv_cb(
    uint8_t *mac_addr, uint8_t *msg, uint8_t msg_len) {

    esp_espnow_obj_t *self = espnow_singleton;
    if (ESPNOW_HDR_LEN + msg_len >= buffer_free(self->recv_buffer)) {
        self->dropped_rx_pkts++;
        return;
    }
    _buf_put_recv_data(self->recv_buffer, mac_addr, msg, msg_len);
    self->recv_packets++;
}

static int _wait_for_recv_packet(size_t n_args, const mp_obj_t *args) {
    esp_espnow_obj_t *self = espnow_singleton;
    size_t timeout = (
        (n_args > 1) ? mp_obj_get_int(args[1]) : self->recv_timeout);
    int msg_len;
    int64_t start = mp_hal_ticks_ms();
    while ((msg_len = _buf_peek_message_length(self->recv_buffer)) == BUF_EMPTY &&
           (mp_hal_ticks_ms() - start) <= timeout) {
        // Won't yield unless delay > portTICK_PERIOD_MS (10ms)
        mp_hal_delay_ms(BUSY_WAIT_MS);
    }
    return msg_len;
}

// ### Send and Receive ESP_Now data
//
// ESPNow.recv([timeout]):
// Returns a tuple of byte strings: (peer_addr, message) where peer_addr is
// the MAC address of the sending peer.
// Takes an optional "timeout" argument in milliseconds.
// Default timeout is set with ESPNow.config(timeout=milliseconds).
// Returns None on timeout.
STATIC mp_obj_t espnow_irecv(size_t n_args, const mp_obj_t *args) {
    esp_espnow_obj_t *self = espnow_singleton;

    int msg_len = _wait_for_recv_packet(n_args, args);
    if (msg_len < 0) {
        self->irecv_msg->len = 0;
        return mp_const_none;   // Timed out - just return None
    }
    if (!_buf_get_recv_data(
        self->recv_buffer,
        self->irecv_peer->items,
        self->irecv_msg->items,
        msg_len)) {
        mp_raise_ValueError(MP_ERROR_TEXT("Buffer error"));
    }
    self->irecv_msg->len = msg_len;
    return MP_OBJ_FROM_PTR(self->irecv_tuple);
}
MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espnow_irecv_obj, 1, 2, espnow_irecv);

// ESPNow.send(peer, message, [sync=True]) can use "synchronised" or
// "fire and forget" mode.
// Synchronised mode (sync=True):
//   Send the message and wait for all recipients to respond (or not),
//   including broadcasts which expect a response from every peer.
//   (Before sending we need to check and clear any pending responses)
//   This is a safer, but slower way to communicate with peers.
//   Caveat: This does NOT guard against packet loss on the recipient
//   due to overflowed recv buffers.
// Un-synchronised mode (sync=False):
//   Just send the message and don't wait for ack from recipients.
//   This is faster, but may be less reliable for communicating with peers.

// ESPNow.send([peer_addr (=None)], message, [sync (=true)])
// Send a message to the peer's mac address. Optionally wait for a response.
// If peer_addr == None, send to all registered peers (broadcast).
// If sync==True, wait for response after sending.
// Returns:
//   True  if sync==False and message sent successfully.
//   True  if sync==True and message is received successfully by all recipients
//   False if sync==True and message is not received by any recipients
// Raises: EAGAIN if the internal espnow buffers are full.

static void wait_for_response(esp_espnow_obj_t *self) {
    int64_t start = mp_hal_ticks_ms();
    while (self->sent_responses < self->sent_packets &&
           (mp_hal_ticks_ms() - start) <= 5000) {
        // Won't yield unless delay > portTICK_PERIOD_MS (10ms)
        mp_hal_delay_ms(BUSY_WAIT_MS);
    }
}

STATIC mp_obj_t espnow_send(size_t n_args, const mp_obj_t *args) {
    esp_espnow_obj_t *self = espnow_singleton;

    bool sync = (n_args > 3) ? mp_obj_get_int(args[3]) : true;
    if (sync) {
        // Wait for any pending send responses
        wait_for_response(self);
    }
    int n = self->sent_successes;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[2], &bufinfo, MP_BUFFER_READ);
    check_esp_err(esp_now_send(
        _get_bytes_len(args[1], ESP_NOW_ETH_ALEN),
        bufinfo.buf, bufinfo.len));
    self->sent_packets++;
    if (sync) {
        // Wait for message to be received by peer
        wait_for_response(self);
    }

    return (!sync || self->sent_successes > n) ? mp_const_true : mp_const_false;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espnow_send_obj, 3, 4, espnow_send);

// ### Peer Management Functions
//

// add_peer(peer_mac, [lmk, [channel]])
STATIC mp_obj_t espnow_add_peer(
    size_t n_args, const mp_obj_t *args) {

    check_esp_err(esp_now_add_peer(
        _get_bytes_len(args[1], ESP_NOW_ETH_ALEN),
        ESP_NOW_ROLE_COMBO,
        (n_args > 3) ? mp_obj_get_int(args[3]) : 0,
        (n_args > 2) ? _get_bytes_len(args[2], ESP_NOW_KEY_LEN) : NULL,
        ESP_NOW_KEY_LEN));

    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(espnow_add_peer_obj, 2, 4, espnow_add_peer);

// del_peer(peer_mac)
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

// Put received message into the buffer (called from recv_cb()).
static void
_buf_put_recv_data(buffer_t buf, const uint8_t *mac,
    const uint8_t *msg, size_t msg_len
    ) {
    uint8_t header[2] = {ESPNOW_MAGIC, msg_len};
    buffer_put(buf, header, sizeof(header));
    buffer_put(buf, mac, ESP_NOW_ETH_ALEN);
    buffer_put(buf, msg, msg_len);
}

// Get the peer mac address and message from a packet in the buffer.
// Assumes msg_len is correct and does not check packet format.
// Get the length and check the packet first with _buf_peek_message_length().
static bool
_buf_get_recv_data(buffer_t buf, uint8_t *mac, uint8_t *msg, int msg_len) {
    uint8_t header[2];              // Copy out the header and ignore it
    return buffer_get(buf, header, sizeof(header)) &&
           buffer_get(buf, mac, ESP_NOW_ETH_ALEN) &&
           buffer_get(buf, msg, msg_len);
}

// Peek at the next recv packet in the ring buffer to get the message length.
// Also validates the packet header.
static int
_buf_peek_message_length(buffer_t buf) {
    // Check for the magic byte followed by the message length
    uint8_t header[2];
    return buffer_peek(buf, header, sizeof(header))
            ? (header[0] == ESPNOW_MAGIC && header[1] <= ESP_NOW_MAX_DATA_LEN)
              ? header[1]       // Success: return length of message
              : BUF_ERROR       // Packet header is wrong
            : BUF_EMPTY;        // No data to read
}

static uint8_t *_get_bytes_len(mp_obj_t obj, size_t len) {
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(obj, &bufinfo, MP_BUFFER_READ);
    if (bufinfo.len != len) {
        mp_raise_ValueError(MP_ERROR_TEXT("wrong length"));
    }
    return (uint8_t *)bufinfo.buf;
}

#endif // MICROPY_ESP8266_ESPNOW
