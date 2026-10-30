//! HRA wire formats (port of `libs/ssnet/include/ssnet/wire.h`). The C++ copies its packed struct with `memcpy`,
//! i.e. little-endian on every platform we ship; here the bytes are written explicitly little-endian.

/// "HRA1": 16-byte header, PCM16
pub const MAGIC1: u32 = 0x3141_5248;
/// "HRA2": 24-byte header, PCM16 + timeline position
pub const MAGIC2: u32 = 0x3241_5248;
/// "HRA3": 22-byte header, Opus / PCM + seq + position
pub const MAGIC3: u32 = 0x3341_5248;

pub const CODEC_PCM16: u8 = 0;
pub const CODEC_OPUS: u8 = 1;

pub const FLAG_NONE: u16 = 0;
/// the packet carries in-band FEC
pub const FLAG_FEC: u16 = 1 << 0;
/// discontinuous transmission (silence)
pub const FLAG_DTX: u16 = 1 << 1;

pub const HRA3_HEADER_BYTES: usize = 22;

/// magic (4) · codec (1) · chans (1) · flags (2) · seq (4) · pos (8) · frames (2)
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Hra3Header {
    pub magic: u32,
    pub codec: u8,
    /// 1 = mono, 2 = stereo
    pub chans: u8,
    pub flags: u16,
    /// incrementing sequence number
    pub seq: u32,
    /// Stream Mix timeline frame position / echo timestamp
    pub pos: u64,
    /// frames per channel in this packet (e.g. 480 for 10 ms at 48 kHz)
    pub frames: u16,
}

impl Hra3Header {
    pub fn to_bytes(&self) -> [u8; HRA3_HEADER_BYTES] {
        let mut b = [0u8; HRA3_HEADER_BYTES];
        b[0..4].copy_from_slice(&self.magic.to_le_bytes());
        b[4] = self.codec;
        b[5] = self.chans;
        b[6..8].copy_from_slice(&self.flags.to_le_bytes());
        b[8..12].copy_from_slice(&self.seq.to_le_bytes());
        b[12..20].copy_from_slice(&self.pos.to_le_bytes());
        b[20..22].copy_from_slice(&self.frames.to_le_bytes());
        b
    }

    pub fn from_bytes(b: &[u8; HRA3_HEADER_BYTES]) -> Self {
        Hra3Header {
            magic: u32::from_le_bytes([b[0], b[1], b[2], b[3]]),
            codec: b[4],
            chans: b[5],
            flags: u16::from_le_bytes([b[6], b[7]]),
            seq: u32::from_le_bytes([b[8], b[9], b[10], b[11]]),
            pos: u64::from_le_bytes([b[12], b[13], b[14], b[15], b[16], b[17], b[18], b[19]]),
            frames: u16::from_le_bytes([b[20], b[21]]),
        }
    }
}

/// The header and payload of an HRA3 packet, or `None` when it is too short, has another magic or a channel count
/// other than 1 / 2 (the same checks as the C++).
pub fn parse_hra3(data: &[u8]) -> Option<(Hra3Header, &[u8])> {
    let (head, payload) = data.split_first_chunk::<HRA3_HEADER_BYTES>()?;
    let h = Hra3Header::from_bytes(head);
    (h.magic == MAGIC3 && (h.chans == 1 || h.chans == 2)).then_some((h, payload))
}

/// Writes header + payload to `dest`; returns the packet length, or `None` when `dest` is too small (the C++ wrote
/// past the end of the buffer).
pub fn write_hra3(dest: &mut [u8], hdr: &Hra3Header, payload: &[u8]) -> Option<usize> {
    let n = HRA3_HEADER_BYTES + payload.len();
    let d = dest.get_mut(..n)?;
    d[..HRA3_HEADER_BYTES].copy_from_slice(&hdr.to_bytes());
    d[HRA3_HEADER_BYTES..].copy_from_slice(payload);
    Some(n)
}

/// Packets missing between the last played seq and this one (survives the u32 wrap). Below 0 = late or repeated:
/// over the unordered DataChannel such a packet's place has already been played, so it is dropped. A packet exactly
/// 2^31 away counts as late (the C++ overflowed `INT32_MIN - 1` into "2^31 - 1 lost").
pub fn seq_gap(seq: u32, last_seq: u32) -> i32 {
    (seq.wrapping_sub(last_seq) as i32).saturating_sub(1)
}
