//
// MIDI over Bluetooth Low Energy, in the browser.
//
// The pedal's radio presents the standard BLE MIDI service, and this is
// the other end of it.  It exists because Web MIDI cannot reach that
// service on Android at all: a Bluetooth MIDI device only becomes a
// MidiDevice once something calls MidiManager.openBluetoothDevice(), and
// Chrome does not, so requestMIDIAccess() never lists the pedal however
// well it advertises.  Web Bluetooth talks to the GATT characteristic
// directly, which is the one route a page has.
//
// Chrome and Edge on Android and the desktop.  Safari has neither API.
//
// The encoding is MMA/AMEI RP-052.  Three rules matter here:
//
//   - every packet starts with a header byte, 1 0 t t t t t t
//   - every MIDI status byte is preceded by a timestamp byte, 1 t t t t t t t
//   - SysEx is the only message that may span packets, and a
//     continuation packet is a header byte followed straight by data
//     with no timestamp.  That absence is the only thing marking it
//
// The timestamp is 13 bits of milliseconds and wraps every 8192 ms.
// Nothing here uses it: the pedal's replies are not time-critical and
// the app renders them when they arrive.  It is still generated, because
// the specification requires it and a receiver may be stricter than we
// are.
//

const BLE_MIDI_SERVICE = '03b80e5a-ede8-4b33-a751-6ce34ec4c700';
const BLE_MIDI_CHARACTERISTIC = '7772e5db-3868-4112-a1a9-f2669d106bf3';

//
// Twenty bytes, which is what the default ATT MTU of 23 leaves.
//
// Web Bluetooth does not expose the negotiated MTU, so there is no way
// to find out that more would fit.  It costs nothing worth having: what
// this end sends is requests and parameter changes, and the replies -
// the schema, tens of kilobytes - arrive as notifications sized by the
// pedal, which does know its MTU and uses all of it.
//
const BLE_MIDI_PACKET = 20;

//
// The largest message worth assembling.  The pedal's schema is the
// biggest thing it sends, at about 40 kB.
//
const MIDI_MAX_MESSAGE = 262144;

function bleMidiTimestamp() {
    return Math.floor(performance.now()) & 0x1fff;
}

//
// How long a MIDI message is, from its status byte.  Zero means SysEx,
// which ends at its own F7 rather than after a count.
//
function midiMessageLength(status) {
    if (status >= 0xf8) return 1;
    switch (status & 0xf0) {
    case 0x80: case 0x90: case 0xa0: case 0xb0: case 0xe0: return 3;
    case 0xc0: case 0xd0: return 2;
    }
    switch (status) {
    case 0xf0: return 0;
    case 0xf1: case 0xf3: return 2;
    case 0xf2: return 3;
    }
    return 1;
}

//
// One MIDI byte stream in, the packets to write out.
//
// Running status is never generated: it saves one byte and costs the
// next reader an ambiguity.
//
function bleMidiEncode(bytes, maxPacket = BLE_MIDI_PACKET) {
    const packets = [];
    let pkt = null;

    const ts = bleMidiTimestamp();
    const header = 0x80 | (ts >> 7);
    const stamp = 0x80 | (ts & 0x7f);

    //
    // Room for 'need' more bytes, starting a fresh packet if there is
    // not.  It emits the header byte and nothing else, so a packet
    // begun in the middle of SysEx data is a continuation - the caller
    // is the one that adds a timestamp, and for continuation data it
    // does not.
    //
    const room = (need) => {
        if (pkt && pkt.length + need <= maxPacket)
            return;
        if (pkt)
            packets.push(Uint8Array.from(pkt));
        pkt = [header];
    };

    let i = 0;
    let status = 0;

    while (i < bytes.length) {
        const b = bytes[i];

        if (b === 0xf0) {
            //
            // The whole SysEx at once: its start is timestamped, its
            // data is not, and its F7 is timestamped again.
            //
            let end = i + 1;
            while (end < bytes.length && bytes[end] !== 0xf7)
                end++;

            if (end >= bytes.length) {
                //
                // No F7 in the buffer, so this is half a message.
                // Sending it leaves the far end assembling for ever and
                // appending whatever comes next to the wreckage, which
                // is worse than losing one reply.
                //
                break;
            }

            room(2);
            pkt.push(stamp, 0xf0);
            for (let j = i + 1; j < end; j++) {
                //
                // Real-time is allowed to interrupt a SysEx and needs
                // its own timestamp byte.  Written as data it would
                // have bit 7 set where the receiver expects none, and
                // be read as a timestamp - losing the byte and the one
                // behind it.
                //
                if (bytes[j] >= 0xf8) {
                    room(2);
                    pkt.push(stamp, bytes[j]);
                    continue;
                }
                room(1);
                pkt.push(bytes[j]);
            }
            room(2);
            pkt.push(stamp, 0xf7);
            i = end + 1;
            continue;
        }

        if (b < 0x80) {
            //
            // Running status: the sender left the status byte out
            // because it has not changed.  Put it back, since a packet
            // may not begin with one that was established in an earlier
            // packet.  With nothing established there is no way to say
            // what the byte belongs to, so it goes.
            //
            if (!status) {
                i++;
                continue;
            }

            const want = midiMessageLength(status) - 1;
            const have = Math.min(want, bytes.length - i);

            room(1 + 1 + have);
            pkt.push(stamp, status);
            for (let j = 0; j < have; j++)
                pkt.push(bytes[i + j]);
            i += have;
            continue;
        }

        //
        // Channel messages set the running status; system common
        // clears it, and real-time leaves it alone.
        //
        if (b < 0xf0)
            status = b;
        else if (b < 0xf8)
            status = 0;

        const len = midiMessageLength(b) || 1;

        //
        // Stop at the next status byte as well as at the end of the
        // buffer.  A message that is short is malformed, and encoding
        // it anyway would send an incomplete one and swallow the
        // message behind it.
        //
        let end2 = i + 1;
        while (end2 < bytes.length && end2 < i + len && !(bytes[end2] & 0x80))
            end2++;
        if (end2 - i !== len) {
            i = end2;
            continue;
        }

        room(1 + len);
        pkt.push(stamp);
        for (let j = 0; j < len; j++)
            pkt.push(bytes[i + j]);
        i += len;
    }

    if (pkt && pkt.length > 1)
        packets.push(Uint8Array.from(pkt));

    return packets;
}

//
// Packets in, whole MIDI messages out.
//
// Whole messages, because that is what Web MIDI delivers and what the
// rest of the app is written against: a SysEx arrives as one event
// however many packets carried it.
//
class BleMidiDecoder {
    constructor(onMessage) {
        this.onMessage = onMessage;
        this.msg = [];          // the message being assembled
        this.want = 0;          // how many bytes it will be, 0 for SysEx
        this.status = 0;        // running status
    }

    //
    // One MIDI byte from the stream the packets carried.
    //
    byte(b) {
        if (b >= 0xf8) {
            this.onMessage(Uint8Array.of(b));   // real-time, stands alone
            return;
        }

        if (b >= 0x80) {
            if (b === 0xf7) {
                // The end of a SysEx, which is the only message that
                // ends on a status byte rather than on a count.
                if (this.want === 0 && this.msg.length) {
                    this.msg.push(b);
                    this.onMessage(Uint8Array.from(this.msg));
                }
                this.msg = [];
                this.want = 0;
                return;
            }
            this.msg = [b];
            this.want = midiMessageLength(b);
            this.status = b < 0xf0 ? b : 0;
            if (this.want === 1) {
                this.onMessage(Uint8Array.from(this.msg));
                this.msg = [];
                this.want = 0;
            }
            return;
        }

        if (this.msg.length >= MIDI_MAX_MESSAGE) {
            //
            // A SysEx whose F7 was lost would otherwise collect every
            // later reply on top of itself for the life of the
            // connection.  Give up on it and resync at the next status
            // byte.
            //
            this.msg = [];
            this.want = 0;
            this.status = 0;
            return;
        }

        if (this.want === 0 && this.msg.length === 0) {
            if (!this.status)
                return;                 // orphan data byte
            this.msg = [this.status];   // running status
            this.want = midiMessageLength(this.status);
        }

        this.msg.push(b);
        if (this.want && this.msg.length >= this.want) {
            this.onMessage(Uint8Array.from(this.msg));
            this.msg = [];
            this.want = 0;
        }
    }

    //
    // One notification.
    //
    // A byte with bit 7 set after the header is a timestamp, and the
    // byte after it is either a status byte or, under running status,
    // the first data byte.  A byte with bit 7 clear where a timestamp
    // would be is SysEx continuation data.
    //
    packet(data) {
        if (data.length < 2 || (data[0] & 0xc0) !== 0x80)
            return;

        let i = 1;
        while (i < data.length) {
            if (!(data[i] & 0x80)) {
                this.byte(data[i++]);
                continue;
            }

            i++;                        // the timestamp itself
            if (i >= data.length)
                return;

            if (data[i] & 0x80)
                this.byte(data[i++]);
            else if (this.status)
                this.byte(this.status);

            while (i < data.length && !(data[i] & 0x80))
                this.byte(data[i++]);
        }
    }
}

//
// The pedal as a port pair: something with onmidimessage on one side
// and send() on the other, which is what the rest of the app reads.
//
// Connecting needs a user gesture, because requestDevice() opens the
// browser's own device chooser.  That chooser is the whole of the
// permission model for now - the characteristic is not encrypted and
// nothing is bonded, so there is no pairing to do and no key to keep.
//
const BLE_PEDAL_ID = 'ble-pedal';

const blePedal = {
    id: BLE_PEDAL_ID,
    name: 'Bluetooth pedal (pick one)',
    onmidimessage: null,

    device: null,
    characteristic: null,
    decoder: null,

    //
    // Writes go out one at a time.  A second writeValueWithoutResponse()
    // while the first is outstanding fails with "GATT operation already
    // in progress", and send() is called from drawing code that cannot
    // wait, so they are chained rather than awaited.
    //
    writes: Promise.resolve(),

    get supported() {
        return typeof navigator !== 'undefined' && !!navigator.bluetooth;
    },

    get connected() {
        return !!(this.device && this.device.gatt && this.device.gatt.connected);
    },

    //
    // Bounded, because on Linux a GATT connect can wait for ever rather
    // than fail - and an app that sits there says less than one that
    // gives up and reports why.
    //
    async step(what, promise, seconds = 15) {
        let timer;
        const limit = new Promise((_, reject) => {
            timer = setTimeout(
                () => reject(new Error(what + ' timed out after '
                                       + seconds + 's')),
                seconds * 1000);
        });

        try {
            return await Promise.race([promise, limit]);
        } finally {
            clearTimeout(timer);
        }
    },

    async connect() {
        const device = await navigator.bluetooth.requestDevice({
            filters: [{ services: [BLE_MIDI_SERVICE] }],
        });

        const server = await this.step('connect', device.gatt.connect());
        const service = await this.step(
            'find the MIDI service', server.getPrimaryService(BLE_MIDI_SERVICE));
        const ch = await this.step(
            'find the MIDI characteristic',
            service.getCharacteristic(BLE_MIDI_CHARACTERISTIC));

        this.decoder = new BleMidiDecoder((msg) => {
            if (this.onmidimessage)
                this.onmidimessage({ data: msg });
        });

        //
        // Removed before it is added, because reconnecting to a device
        // the browser has already handed out gives back the same
        // objects - so a second connect would leave two listeners on
        // one characteristic and decode every notification twice.
        //
        if (this.onValue)
            ch.removeEventListener('characteristicvaluechanged',
                                   this.onValue);
        this.onValue = (ev) => {
            const v = ev.target.value;
            this.decoder.packet(
                new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
        };
        ch.addEventListener('characteristicvaluechanged', this.onValue);

        await this.step('subscribe', ch.startNotifications());

        // once: the next connect adds it again
        device.addEventListener('gattserverdisconnected', () => {
            this.characteristic = null;
            this.decoder = null;
            if (this.ondisconnect)
                this.ondisconnect();
        }, { once: true });

        this.device = device;
        this.characteristic = ch;
        this.name = 'Bluetooth: ' + (device.name || 'pedal');
        return device.name;
    },

    send(bytes) {
        if (!this.characteristic)
            return;

        for (const packet of bleMidiEncode(Array.from(bytes))) {
            this.writes = this.writes
                .then(() => this.characteristic.writeValueWithoutResponse(packet))
                .catch((err) => {
                    console.error('[BLE MIDI] write failed', err);
                });
        }
    },
};

//
// The codec on its own, for Validation/test-webmidi.js.
//
// Encoding and decoding are arithmetic and testable without a browser or
// a pedal; the transport above them is neither.
//
const bleCodec = {
    encode: bleMidiEncode,
    Decoder: BleMidiDecoder,
};
