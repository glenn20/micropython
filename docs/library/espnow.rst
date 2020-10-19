:mod:`espnow` --- Support for the ESP-NOW protocol
==================================================

.. module:: espnow
   :synopsis: ESP-NOW wireless protocol support

This module provides an interface to the ESP-NOW protocol provided by Espressif
on ESP32 and ESP8266 devices. Some calls are only available on the ESP32 due to
code size restrictions on the ESP8266 and differences in the Espressif API.

Load this module from the :doc:`esp` module. A simple example would be:

**Sender:** ::

        import network
        from esp import espnow

        # A WLAN interface must be active to send()/recv()
        w0 = network.WLAN(network.STA_IF)  # Or network.AP_IF
        w0.active(True)

        e = espnow.ESPNow()
        e.init()
        peer = b'\xbb\xbb\xbb\xbb\xbb\xbb'   # MAC address of peer's wifi interface
        e.add_peer(peer)

        e.send("Starting...")       # Send to all peers
        for i in range(100):
            e.send(peer, str(i)*20, True)
        e.send(b'end')

**Receiver:** ::

        import network
        from esp import espnow

        # A WLAN interface must be active to send()/recv()
        w0 = network.WLAN(network.STA_IF)
        w0.active(True)

        e = espnow.ESPNow()
        e.init()
        peer = b'\xaa\xaa\xaa\xaa\xaa\xaa'   # MAC address of peer's wifi interface
        e.add_peer(peer)

        while True:
            msg = e.irecv()     # Available on ESP32 and ESP8266
            if msg:             # msg == None if timeout in irecv()
                print(msg)
                if msg[1] == b'end':
                    break

.. note:: This module is still under development and its classes, functions,
          methods and constants are subject to change.

class ESPNow
------------

Constructor
-----------

.. class:: ESPNow()

    Returns the singleton ESPNow object. As this is a singleton, all calls to
    `espnow.ESPNow()` return a reference to the same object.

Configuration
-------------

.. method:: ESPNow.init()
            ESPNow.init([recv_bufsize])    (ESP8266 only)

    Prepare the software and hardware for use of the ESPNow communication
    protocol, including:

    - initialise the ESPNow data structures,
    - allocate the recv data buffer,
    - invoke esp_now_init() and
    - register the send and recv callbacks.

    **ESP8266**: The recv buffer size may be set as an argument to `init()` as
    there is no `config()` method on the ESP8266 (due to code size restrictions).

.. method:: ESPNow.deinit()

    De-initialise the Espressif ESPNow software stack (esp_now_deinit()),
    disable callbacks and deallocate the recv data buffer.

    **Note**: `deinit()` will also deregister all peers which must be
    re-registered after `init()`.

.. method:: ESPNow.config('param')
            ESPNow.config(param=value, ...)

    **Note:** ESP32 only - Use `init([recv_bufsize])<ESPNow.init()>` on the
    ESP8266.

    Get or set configuration values of the ESPNow interface. To get a value the
    parameter name should be quoted as a string, and just one parameter is
    queried at a time.  To set values use the keyword syntax, and one or more
    parameters can be set at a time.

    Currently supported values are:

    - ``rxbuf``: *(default=516)* Get/set the size in bytes of the internal
      buffer used to store incoming ESPNow packet data. The default size is
      selected to fit two max-sized ESPNow packets (250 bytes) with associated
      mac_address (6 bytes) and a message byte count (1 byte) plus buffer
      overhead. Increase this if you expect to receive a lot of large packets
      or expect bursty incoming traffic.

      **Note:** The recv buffer is only allocated by `ESPNow.init()`.
      Changing these values will have no effect until the next call of
      `ESPNow.init()`.

    - ``timeout``: *(default=300,000)* Default read timeout (in milliseconds).
      The timeout can also be provided as arg to `recv()` and `irecv()`.

.. method:: ESPNow.clear(True) (ESP32 only)

    Clear out any data in the recv buffer. Use this to clean
    up after receiving a ``Buffer error`` (should not happen). All data in the
    buffers will be discarded. An arg of `True` is required to guard against
    inadvertent use.

.. method:: ESPNow.set_pmk(pmk)

    Set the Primary Master Key (PMK) which is used to encrypt the Local Master
    Keys (LMK) for encrypting ESPNow data traffic. If this is not set, a default
    PMK is used by the underlying Espressif esp_now software stack. The ``pmk``
    argument bust be a byte string of length `espnow.KEY_LEN` (16 bytes).

Sending and Receiving Data
--------------------------

A wifi interface (``network.STA_IF`` or ``network.AP_IF``) must be
`active()<network.WLAN.active>` before messages can be sent or received,
but it is not necessary to connect or configure the WLAN interface.
For example::

    import network

    w0 = network.WLAN(network.STA_IF)
    w0.active(True)

.. method:: ESPNow.send(mac, msg, [sync=True])
            ESPNow.send(msg)    (ESP32 only)

    Send the data contained in ``msg`` to the peer with given network ``mac``
    address. In the second form, ``mac=None`` and ``sync=True``.

    Arguments:

    - ``mac``: byte string exactly 6 bytes long or ``None``
    - ``msg``: string or byte-string such that
      ``0<len(msg)<=espnow.MAX_DATA_LEN`` (250) bytes
    - ``sync``:

      - ``True``: (default) send ``msg`` to the peer and wait for a response
        (or not). Returns ``False`` if any peers fail to respond.

      - ``False`` handover ``msg`` to the esp_now software stack for
        transmission and return immediately.
        Responses from the peers will be discarded.
        Always returns ``True``.

    If ``mac`` is ``None`` the message will be sent to all
    registered peers as an ESP-Now broadcast.

    **Note**: A peer will respond with success if it's wifi interface is
    active() and set to the same channel as the sender, regardless of whether
    it has initialised it's ESP-Now system or is actively listening for ESP-Now
    traffic (see the Espressif ESP-Now docs).

.. method:: ESPNow.recv([timeout]) (ESP32 only)

    **Note:** ESP32 only. Use `irecv()` on the esp8266.

    Wait for an incoming message and return:

    - ``None`` if ``timeout`` is reached before a message is received, or
    - a newly allocated tuple of `bytes`: ``(mac, message)``, where:

      - ``mac`` is the mac address of the sending device (peer) and

      - ``msg`` is the message/data sent from the peer.

    ``timeout`` optionally sets a timeout (in milliseconds) for the read. The
    default timeout (5 minutes) can be set on the ESP32 using `ESPNow.config()`.

    **Note**: repeatedly calling `recv()<ESPNow.recv()>` will exercise the
    micropython memory allocation as new storage is allocated for each new
    message and tuple. Use `irecv()<ESPNow.irecv()>`
    for a more memory-efficient option.

.. method:: ESPNow.irecv([timeout])

    Wait for an incoming message and return:

    - ``None`` if ``timeout`` is reached before a message is received, or
    - a "callee-owned" tuple of `bytearray`: ``(mac, message)``, where:

      - ``mac`` is the mac address of the sending device (peer) and

      - ``msg`` is the message/data sent from the peer.

    ``timeout`` optionally sets a timeout (in milliseconds) for the read. The
    default timeout (5 minutes) can be set on the ESP32 using `ESPNow.config()`.

    **Note**: Equivalent to `recv()<ESPNow.recv()>`, except that
    `irecv()<ESPNow.irecv()>` will
    return a "callee-owned" tuple of bytearrays.
    That is, memory will be allocated once for the tuple and bytearrays on
    invocation of `espnow.ESPNow()<ESPNow()>` and reused for subsequent calls to
    `irecv()<ESPNow.irecv()>`. You must make copies if you
    wish to keep the values across subsequent calls to `irecv()<ESPNow.irecv()>`.
    `irecv()<ESPNow.irecv()>` is more efficient on memory constrained
    microcontrollers like the ESP32 and ESP8266.

    On timeout, `irecv()` will return `None` and set the length of the
    callee-owned ``message`` bytearray to zero.

.. method:: ESPNow.stats() (ESP32 only)

    Return a 5-tuple containing the number of packets sent/received/lost::

    (sent_packets, sent_responses, sent_failures, recv_packets, dropped_recv_packets)

    Incoming packets are *dropped* when the recv buffers are full. To reduce
    packet loss, increase the ``rxbuf`` config parameters and ensure you are
    in a tight loop calling `irecv()<ESPNow.irecv()>` as quickly as possible.

    **Note**: Dropped packets will still be acknowledged to the sender as
    received.

Iteration over ESPNow
---------------------

**Note**: ESP32 only

It is also possible to read messages by iterating over the ESPNow singleton
object. This will yield ``(mac, message)`` tuples using the alloc-free
`irecv()` method, eg::

        for msg in e:
            print(f"Recv: mac={msg[0]}, message={msg[1]}")

**Note**: Iteration will yield ``None`` if the default timeout expires waiting
for a message.

Peer Management
---------------

The Esspresif ESP-Now software requires that other devices (peers) must be
*registered* before we can `send()<ESPNow.send()>` them messages.

.. method:: ESPNow.add_peer(mac, [lmk], [channel], [ifidx], [encrypt])
            ESPNow.add_peer(mac, param=value, ...)   (ESP32 only)

    Add/register the provided ``mac`` address (a 6-byte byte-string) as a peer
    under the ESPNow protocol. The following "peer info" parameters may also be
    specified as positional or keyword arguments:

    - ``lmk``: The Local Master Key (LMK) key used to encrypt data transfers
      with this peer (if the *encrypt* parameter is set to *True*). Must be:

      - a byte-string of length ``espnow.KEY_LEN`` (16 bytes), or
      - any non-`True` python value (default= ``b''``), signifying an *empty* key
        which will disable encryption.

    - ``channel``: The wifi channel (2.4GHz) to communicate with this peer. Must
      be an integer from 0 to 14. If channel is set to 0 the current channel
      of the wifi device will be used. (default=0)

    - ``ifidx``: *(ESP32 only)* Index of the wifi interface which will be used
      to send data to this peer. Must be an integer set to
      ``network.STA_IF`` (=0) or ``network.AP_IF`` (=1).
      (default=0/``network.STA_IF``).

    - ``encrypt``: *(ESP32 only)* If set to ``True`` data exchanged with this
      peer will be encrypted with the PMK and LMK. (default=``False``)

    **ESP8266**: Keyword args may not be used on the ESP8266.

    **Note**: Managing peers can become complex on the ESP32/8266 if you are
    using more than just the STA_IF interface. The ESP32/8266 effectively has two
    independent wifi interfaces (STA_IF and AP_IF) and each has their own MAC
    address. You must:

    - choose the correct MAC address of the remote peer (STA_IF or AP_IF) to
      register,

    - register it with the correct local interface (``ifidx`` = STA_IF or AP_IF),
      and

    - ensure the correct interfaces are ``active(True)`` on the local and remote
      peer.

    `ESPNow.send()<ESPNow.send()>` will raise an
    ``OSError('ESP_ERR_ESPNOW_IF')``
    exception when trying to send a message to a peer which is registered to a
    local interface which is not ``active(True)``. Note also that both
    interfaces may be active simultaneously, leading to a lot of flexibility
    in configuring ESPNow and Wifi networks.

.. method:: ESPNow.get_peer(mac) (ESP32 only)

    Return a 5-tuple of the "peer info" associated with the ``mac`` address::

        (mac, lmk, channel, ifidx, encrypt)

.. method:: ESPNow.peer_count() (ESP32 only)

    Return the number of peers which have been registered.

.. method:: ESPNow.get_peers() (ESP32 only)

    Return the "peer info" parameters for all the registered peers (as a tuple
    of tuples).

.. method:: ESPNow.mod_peer(mac, lmk, [channel], [ifidx], [encrypt]) (ESP32 only)
            ESPNow.mod_peer(mac, 'param'=value, ...) (ESP32 only)

    Modify the parameters of the peer associated with the provided ``mac``
    address. Parameters may be provided as positional or keyword arguments.

.. method:: ESPNow.del_peer(mac)

    Deregister the peer associated with the provided ``mac`` address.

Stream IO interface
-------------------

**Note**: ESP32 only

**Note**: The ESPNow buffer packet format is not yet fully documented. It
will be supported by a python support module for reading and sending ESPNow
message packets through the ``stream`` interface.

EspNow also supports the micropython ``stream`` io interface. This is intended
to help support high throughput low-copy transfers and also to support
``uasyncio`` through the StreamReader interface. ESPNow includes
support for the following python
`stream interface <https://docs.python.org/3/library/io.html>`_ methods:

.. method:: ESPNow.read([size=-1])

    Return up to ``size`` bytes read from the espnow recv buffers as a byte
    string. Is nonblocking and returns None if no data available. The returned
    data is a stream of ESPNow buffer packet data.

.. method:: ESPNow.read1([size=-1])

    As for `read()` but will return after at most one packet is read.

.. method:: ESPNow.readinto(b)

    Read bytes into a pre-allocated, writable bytes-like object (eg. bytearray)
    and return the number of bytes read. Is nonblocking and returns None if no
    data available.

.. method:: ESPNow.readinto1(b)

    As for `readinto()` but will return after at most one packet is read.

.. method:: ESPNow.write(b)

    Write the given bytes-like object to the ESPNow interface. ``b`` must
    contain a sequence of ESPNow buffer packet data.

`ESPNow` also supports the ``poll.poll`` and ``poll.ipoll`` calls, so users
may wait on received events.

Supporting ``uasyncio``
-----------------------

**Note**: ESP32 only

`ESPNow` uses the ``stream`` io interface to support the micropython
``uasyncio`` module for asynchronous IO. A ``StreamReader`` class may be
constructed from an ESPNow object and used to support async IO. Eg::

        s = StreamReader(e)

        async def areadespnow(s):
            while e.send(b'ping'):
                msg = await(s.read1())
                if msg[8:] != b'pong'
                    break

Constants
---------

**Note**: ESP32 only

.. data:: espnow.MAX_DATA_LEN         (=250)
          espnow.KEY_LEN              (=16)
          espnow.MAX_TOTAL_PEER_NUM   (=20)
          espnow.MAX_ENCRYPT_PEER_NUM (=6)
