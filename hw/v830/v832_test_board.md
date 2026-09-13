# V832 Test Board

Machine name: `v832-test-board`

The board reuses the V832 SoC and adds a deterministic fixture window at
`0xc0001000`:

| Offset | Access | Meaning |
| --- | --- | --- |
| `0x00..0x07` | write/read | Drive/read `INTP00..INTP07` levels |
| `0x10..0x17` | write/read | Drive/read `PORTB0..PORTB7` inputs |
| `0x20..0x27` | write/read | Drive/read `PORTA0..PORTA7` inputs |
| `0x30..0x33` | write/read | Drive/read external `DMARQ0..DMARQ3` |
| `0x38` | write/read | Drive/read SoC NMI level |
| `0x40` | read | Observe `PORT0..PORT4` outputs |
| `0x44` | read | Observe `PORTA0..PORTA7` outputs |
| `0x48` | read | Observe `PORTB0..PORTB7` outputs |
| `0x4c` | read | Observe `DMAAK0..DMAAK3` |
| `0x50` | read | Observe `TC/STOPAK` |
| `0x51` | read/write | Read or clear `TC/STOPAK` rising-edge count |
| `0x54` | read | Last value exchanged with the CSI SSI loopback |
| `0x58` | read/write | Read or clear DMAAK arbitration log count |
| `0x59` | write | Arm NMI on the next DMAAK assertion |
| `0x5a` | write | Arm NMI when STOPAK is asserted |
| `0x5c..0x5f` | read | DMAAK arbitration channel sequence |
| `0x60` | read/write | Read or clear PORTA DMAAK edge flags |

The board attaches an SSI loopback peripheral to CSI0. It returns each
received word unchanged and records the last transferred value.

This machine is intended for firmware conformance tests covering GPIO muxing,
external interrupts, DMA handshakes, NMI abort/resume, ICU behavior, and CSI.
