# The link between the pedal and its radio

How the RP2354 and the nRF54L10 on the `minimal` board talk to each other
over a UART: the wire, the packet format, streams and windows, loss, and
restarts. The code is `nRF54/app/src/link.h`, which both sides compile,
`Firmware/nrf54/uart.h` on the pedal and `nRF54/app/src/main.c` on the radio.

## What it is built for

This is not a general-purpose serial protocol, and the design leans on that:

- **Two chips on one board.** The two data lines are copper traces under an
  inch long, with no connector. Bit errors are not expected; the CRC is
  there so that damage is seen rather than decoded, not because the line is
  noisy.
- **Both ends are ours and are updated together.** The pedal programs the
  radio over SWD (Serial Wire Debug), so there is no version negotiation.
- **Both ends poll from a main loop**, expected to come round within about a
  millisecond. The radio sleeps 1 ms when it has nothing to do. The pedal's
  loop also runs USB, and a few things hold it up for longer: a debug-probe
  command can take milliseconds, and saving a scene erases a flash sector
  with interrupts off.
- **The pedal's start is the radio's start.** The pedal holds the radio in
  reset while it brings the link up, so the radio never carries link state
  from before the pedal's.
- **Loss has two expected causes:** one side restarting in the middle of a
  packet, and a receiver falling a whole receive ring behind, which takes a
  stall of seconds. Neither is a reason to resend, and nothing is resent.

What it carries: MIDI to and from up to four Bluetooth connections, commands
from the pedal to the radio and the radio's answers, the radio's debug text,
and a load test.

## The wire

UART1 on the RP2354 (GPIO4 transmit, GPIO5 receive) to UARTE30 on the nRF
(P0.01 receive, P0.00 transmit). 1 Mbit/s, 8 data bits, no parity, one stop
bit, so 100 kB/s each way. The board also wires RTS and CTS, and nothing
uses them: there is no hardware flow control. Both receive pins have a
pull-up, so an undriven line reads as idle rather than as a break.

Both ends move bytes with DMA (direct memory access) and service the
link from the main loop, with no interrupt:

- **Pedal receive:** a 4 kB ring, aligned to its size, that one DMA transfer
  of 0xffffffff bytes writes for ever, wrapping on its address bits. The
  loop reads the channel's write address to see how far it has got.
- **Pedal transmit:** a 1 kB ring, sent one contiguous span per DMA
  transfer.
- **Radio receive:** a 4 kB ring that the DMA refills from the top for ever,
  through the shortcut from the receive END event to the START task. The
  DMA's match filter is set to the zero byte that ends every packet, which
  makes the `AMOUNT` register say how far the DMA has got at each packet
  end; END events count the wraps. The radio only knows the position at a
  packet end, which is all the decoder needs.
- **Radio transmit:** a 1 kB ring, sent one contiguous span per DMA
  transfer.

Each hand-over between the CPU and a DMA channel has a data memory barrier.

## Packets

A packet is a four-byte header, a payload of 0 to 128 bytes, and a CRC-8
over both (polynomial 0x07, initial value 0). That is encoded with COBS
(Consistent Overhead Byte Stuffing), which removes every zero byte, and
ended with a zero. A zero on the wire is therefore always the end of a
packet, and a receiver can start listening at any byte. A packet is at most
135 bytes on the wire: one byte of COBS overhead, since a frame is shorter
than COBS's 254-byte block, and the zero.

The receiver believes nothing until it has seen its first zero. A frame
that does not decode, is longer than a packet can be, or is too short to
hold a header and a CRC is dropped and counted as malformed; one whose CRC
is wrong is dropped and counted separately. Zeros with nothing between them
are ignored.

The header:

| Byte | Field | Meaning |
|---|---|---|
| 0 | kind | what the packet is (below) |
| 1 | peer | which Bluetooth connection it is from or for; 0 is all of them |
| 2 | seq | the packet's number in its stream |
| 3 | flags | `LINK_FIRST` 0x01, `LINK_LAST` 0x02, `LINK_TRUSTED` 0x04 |

The radio assigns peer numbers to its connections. The pedal only ever
echoes one back, so it keeps no table of them.

| Kind | Value | Direction | Carries |
|---|---|---|---|
| `LINK_MIDI` | 0 | both | MIDI bytes, to or from one peer |
| `LINK_CONTROL` | 1 | both | a command for the radio, or its answer |
| `LINK_DEBUG` | 2 | radio to pedal | the radio's `printk` text |
| `LINK_ACK` | 3 | both | how far a stream has got (below) |
| `LINK_HELLO` | 4 | radio to pedal | the radio has started |
| `LINK_ASK` | 5 | both | please acknowledge this stream |
| `LINK_TEST` | 6 | both | load for testing the link |

`LINK_FIRST` marks a payload that starts a message, which is where a
receiver may pick up again after a lost packet. On a MIDI stream that is
any payload starting with a status byte other than F7 (the end of a
SysEx), real-time bytes (F8 to FF) included; on the others, the first
packet of a message. `LINK_LAST` marks
the end of a command or answer. `LINK_TRUSTED` is set by the radio on MIDI
from a peer that bonded with Secure Connections through the pedal's pairing
window, and the pedal refuses SysEx and the reboot-to-bootloader CC from a
peer without it.

## Streams

A stream is a kind and a peer: MIDI for each peer, the control stream, the
debug stream, and the test streams. Acknowledgements, asks and the hello
are not streams. Each side keeps up to 12 streams, made on first use; when
all 12 are in use, the one idle longest - nothing in flight, no
acknowledgement due - is reused, and with none of those a packet on a new
stream is dropped and counted.

A new stream starts at sequence 0 on both sides, and takes nothing until a
packet with `LINK_FIRST`: its first packet may be the rest of a message
from a stream the far side had before, numbered 0 and so in order.

The radio numbers its Bluetooth connections from 1 to 255 and then from 1
again, a new number for each connection and never one still in use, so
streams for peers that have gone are reused as new ones arrive. A radio
that restarts starts again from 1.

For each stream, each side keeps:

| Field | Meaning |
|---|---|
| `tx_next` | the sequence number the next packet out gets |
| `tx_acked` | the first packet out the far side has not consumed |
| `rx_next` | the sequence number expected next |
| `rx_done` | one past the last packet consumed |

Sequence numbers are 8 bits and wrap, and are compared modulo 256. With
two packets in flight at most, that is never ambiguous.

## Windows and acknowledgements

A sender may have at most 2 packets on a stream that the receiver has not
**consumed**: `tx_next - tx_acked < 2`. Consumed means dealt with, not
merely received, so a stream whose receiver cannot use its packets yet
stops its sender on that stream. Every stream starts with its window open
and nothing is granted, because each side has room for every stream's
window at once.

**The windows are per stream, but MIDI for different peers shares a queue
at every stage**, so a shut window on one peer's MIDI stream holds up MIDI
for all of them:

- the pedal collects outgoing MIDI into one packet buffer, and takes no MIDI
  for another peer until that packet has gone;
- the radio's MIDI from the air waits in one queue, and the record at its
  head waits for its own stream's window;
- the radio hands held MIDI to Bluetooth in arrival order, through one set
  of notification buffers shared by every connection.

Commands, answers, debug text and test traffic do not share these, and a
stalled MIDI stream does not hold them up.

An acknowledgement is a `LINK_ACK` packet with the stream's peer in the
peer byte, the stream's kind in the flags byte, `rx_done` as its sequence
number and `rx_next` as a one-byte payload: how far consumed, and how far
received. The sender takes the first as its new `tx_acked` if it lies
between the old one and `tx_next`.

Acknowledgements are sent the next time a side sends anything after
something is consumed, ahead of any other packet, and outside any window.
Each side reads its receive ring before it sends, so that an
acknowledgement says where it really is: after a stall, one sent first
would report the position from before it. Every 100 ms each side repeats
one for every stream it has received anything on, since an acknowledgement
only restates a position and a lost one would otherwise leave a full window
shut for good. The radio
does this only on a pass that found nothing to do, so a radio that is busy
without a break does not repeat them; the pedal does it on every pass.

What consuming means at each end:

- **The pedal consumes everything on arrival:** MIDI goes through a parser
  per peer, answers through a parser of their own, debug text to its debug
  port.
- **The radio holds MIDI** in one of 32 slots until it has been handed to
  Bluetooth, so a Bluetooth client that cannot keep up shuts the MIDI
  windows from the pedal.
- **The radio holds commands** in one of 2 slots until its queue of answers
  to the pedal is empty, so that whatever a command says back has room.

## Loss

A receiver that gets a sequence number other than `rx_next` counts a gap,
takes the new number as its position, and drops packets on that stream,
consuming each, until one with `LINK_FIRST`, and marks that one as
following a gap. The parser behind the stream then starts afresh rather
than take it as the rest of the message that was cut: the pedal drops a
SysEx cut short, and the radio closes it with F7 and sends it on. The
radio also closes an open SysEx when MIDI goes on to another peer, since
the pedal only switches peer between messages.

**Write-off.** A sender that gets an acknowledgement showing the receiver
has received less than was sent, and has sent nothing on the stream for
200 ms (`LINK_LOST_MS`), takes the rest as lost: it moves `tx_acked` so
that only packets received and not yet consumed hold the window. Worked out
from the acknowledgement itself, so the same one again changes nothing.
The receiver sees the next packet as a gap. A receiver that is merely slow
has received everything sent, so its window stays shut until it catches
up.

A side whose own loop was held up does not count that time: a jump of more
than 100 ms in its clock is taken as a stall, and moves the time each
stream last sent forward by as much. What it had queued mostly could not
leave meanwhile, and the acknowledgements waiting in its ring are from
before it did.

**Ask.** A receiver that has heard nothing on a stream has nothing to
acknowledge, so a write-off would never come. So a sender with a packet
unacknowledged, and nothing sent on the stream for 200 ms, sends a
`LINK_ASK`, at most once per 200 ms per stream: the stream's peer, its
kind in the flags byte, sequence 0, and the sender's `tx_acked` as the
payload. A receiver that knows the stream acknowledges it. One that does
not makes it, starting at the asker's `tx_acked`, and acknowledges that:
the packets it never saw are then written off like any others.

## Starting and restarting

**The pedal starts first.** It finds the radio over SWD, holds it in reset,
sets up its UART and DMA, initialises its link state and releases the
reset. Until it has a hello, it sends nothing but acknowledgements.

**The radio starts listening, then says hello.** It starts its receive DMA,
picks a random 32-bit boot id, and sends a zero followed by a `LINK_HELLO`:
peer 0, sequence 0, `LINK_FIRST | LINK_LAST`, with the boot id (four bytes,
low first) and the image's build date and time as payload. It repeats the
hello every 50 ms, on passes that found nothing to do, until it has decoded
any packet from the pedal.

**The pedal answers a new hello by starting over.** A hello with the boot id
it already has is a repeat and is ignored. Any other forgets every stream,
throws away half-parsed MIDI and the MIDI collecting for the next packet,
starts the command it was part way through sending again from its first
packet, sends a zero so that the radio's decoder is in step, and queues
what the radio has to know: the pedal's name and whether the pairing window
is open. Those go behind any commands already queued.

It also stops sending MIDI until the new radio says a Bluetooth client is
subscribed. It keeps its place in its queue of outgoing MIDI, so the rest
of a message it was part way through goes out, without `LINK_FIRST`, and
the new radio drops it. Packets already in its transmit ring go out too,
numbered for the old streams, and are taken as gaps.

**A radio that restarts on its own** comes through the same path with a new
boot id. **A pedal that restarts** resets the radio, so both start again.

## Buffers

| Side | Buffer | Size | Holds |
|---|---|---|---|
| pedal | receive ring | 4 kB | bytes from the radio, written by DMA |
| pedal | transmit ring | 1 kB | packets for the radio |
| pedal | command queue | 512 B | whole commands waiting for the control window |
| pedal | MIDI buffer | 128 B | MIDI collecting into the next packet |
| radio | receive ring | 4 kB | bytes from the pedal, written by DMA |
| radio | transmit ring | 1 kB | packets for the pedal |
| radio | Bluetooth in | 1 kB | packets from the Bluetooth thread, waiting for the loop |
| radio | MIDI from the air | 2 kB | MIDI decoded from Bluetooth, waiting for its window |
| radio | answer queue | 1 kB | answers waiting for the control window |
| radio | debug queue | 1 kB | lines of `printk` waiting for the debug window |
| radio | held MIDI | 32 slots | MIDI from the pedal waiting for Bluetooth |
| radio | held commands | 2 slots | commands waiting for the answer queue to empty |

**The windows keep each receive ring from overrunning.** Twelve streams of
two packets of at most 135 bytes is 3240 bytes, under the 4 kB ring, so
data alone cannot lap it however late the loop is. What still arrives while
a receiver's loop is stopped is acknowledgements and asks, at most a few
hundred bytes a second. Measured with the load test, the radio's loop had to
stop for several seconds before its ring overran.

The radio takes its ring as overrun when more than 3826 bytes are waiting:
the ring less two packets, one for a packet the DMA may be part way through
and one for the time it takes to read the rest. It then counts everything
waiting as lost, skips it, and drops the packet after it as well, since the
decoder was in the middle of one. Since END is a flag rather than a count,
an overrun that crosses two wraps between polls is not counted, and shows
up as gaps and a malformed packet instead. The pedal cannot tell an overrun
at all: it sees the result as gaps, CRC failures and malformed packets.

**When a queue is full, what does not fit is dropped and counted**, not
held back: commands at the pedal, answers and debug lines at the radio,
and MIDI from Bluetooth, which has no way to push back on a sender over the
air. When the radio's MIDI-from-the-air queue lacks room for another
packet's worth, the loop stops taking from Bluetooth in, which then fills
and drops.

## Commands and answers

A command is a whole SysEx message, `F0 7D` and a command byte through F7,
of up to 255 bytes. The pedal queues it whole and sends it on the control
stream in packets of up to 128 bytes, `LINK_FIRST` on the first and
`LINK_LAST` on the last. The radio puts it back together in a 256-byte
buffer and deals with it when it takes the last packet. Answers come back
on the control stream as SysEx messages, which the pedal parses as MIDI from
a trusted source.

Each pass, the radio sends a packet of the control stream before one of
MIDI, so that a notice that a Bluetooth client has subscribed reaches the
pedal ahead of the request that client sends next - as long as the control
window is open. The pedal throws MIDI for the radio away until it has such
a notice, since sending to nobody would cost the link the whole of a big
reply.

## Timing

| Constant | Value | What it is |
|---|---|---|
| `LINK_LOST_MS` | 200 ms | how long a packet may still be on its way |
| acknowledgement repeat | 100 ms | both sides |
| hello repeat | 50 ms | until the radio hears from the pedal |
| radio idle sleep | 1 ms | when a pass found nothing to do |

## Tests

- `Validation/test-link.c` checks the encoder and decoder against known
  packets and every single-byte change and omission - a changed data byte
  is always caught, and a changed COBS code byte or a missing byte gets
  through about 1 time in 3000 - and two ends joined in memory for windows,
  acknowledgements, loss, asks, the hello, stream reuse and sequence wrap.
- `Validation/test-uartrx.c` checks the radio's receive position logic
  against a model of its DMA.
- `Validation/link-stress.py` runs the load test on a pedal: test streams
  both ways, a stream held stuck, debug text, commands timed through the
  radio, a schema over Bluetooth, and optionally the radio's loop stopped
  for a while.
