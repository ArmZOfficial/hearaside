//! WebSocket framing + handshake of the share server (RFC 6455, server side), JSON text as `juce::JSON` wrote it.
//! SHA-1 and base64 are only for the `Sec-WebSocket-Accept` value, so they are small own code like the C++.
use super::sock;
use std::net::TcpStream;

pub(crate) const MAGIC1: u32 = 0x3141_5248; // "HRA1": 16-byte header (magic, rate, channels, 0, frames)
pub(crate) const MAGIC2: u32 = 0x3241_5248; // "HRA2": 24-byte header, plus a u64 position on the Stream Mix timeline

/// A whole message may not be larger than this (summed over its fragments): endless fragments must not fill the
/// DAW's memory.
pub(crate) const MAX_MESSAGE: usize = 1 << 22;

pub(crate) fn sha1(msg: &[u8]) -> [u8; 20] {
    let mut h: [u32; 5] = [
        0x6745_2301,
        0xEFCD_AB89,
        0x98BA_DCFE,
        0x1032_5476,
        0xC3D2_E1F0,
    ];
    let mut m = msg.to_vec();
    let bits = (msg.len() as u64).wrapping_mul(8);
    m.push(0x80);
    while m.len() % 64 != 56 {
        m.push(0);
    }
    m.extend_from_slice(&bits.to_be_bytes());
    for block in m.as_chunks::<64>().0 {
        let mut w = [0u32; 80];
        for (i, c) in block.as_chunks::<4>().0.iter().enumerate() {
            w[i] = u32::from_be_bytes([c[0], c[1], c[2], c[3]]);
        }
        for i in 16..80 {
            w[i] = (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16]).rotate_left(1);
        }
        let [mut a, mut b, mut c, mut d, mut e] = h;
        for (i, wi) in w.iter().enumerate() {
            let (f, k) = match i {
                0..=19 => ((b & c) | (!b & d), 0x5A82_7999u32),
                20..=39 => (b ^ c ^ d, 0x6ED9_EBA1),
                40..=59 => ((b & c) | (b & d) | (c & d), 0x8F1B_BCDC),
                _ => (b ^ c ^ d, 0xCA62_C1D6),
            };
            let t = a
                .rotate_left(5)
                .wrapping_add(f)
                .wrapping_add(e)
                .wrapping_add(k)
                .wrapping_add(*wi);
            e = d;
            d = c;
            c = b.rotate_left(30);
            b = a;
            a = t;
        }
        h[0] = h[0].wrapping_add(a);
        h[1] = h[1].wrapping_add(b);
        h[2] = h[2].wrapping_add(c);
        h[3] = h[3].wrapping_add(d);
        h[4] = h[4].wrapping_add(e);
    }
    let mut out = [0u8; 20];
    for (i, v) in h.iter().enumerate() {
        out[i * 4..i * 4 + 4].copy_from_slice(&v.to_be_bytes());
    }
    out
}

pub(crate) fn base64(data: &[u8]) -> String {
    const ABC: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut s = String::with_capacity(data.len().div_ceil(3) * 4);
    for c in data.chunks(3) {
        let n = (u32::from(c[0]) << 16)
            | (u32::from(*c.get(1).unwrap_or(&0)) << 8)
            | u32::from(*c.get(2).unwrap_or(&0));
        s.push(ABC[(n >> 18) as usize & 63] as char);
        s.push(ABC[(n >> 12) as usize & 63] as char);
        s.push(if c.len() > 1 {
            ABC[(n >> 6) as usize & 63] as char
        } else {
            '='
        });
        s.push(if c.len() > 2 {
            ABC[n as usize & 63] as char
        } else {
            '='
        });
    }
    s
}

/// The `Sec-WebSocket-Accept` value for a client key.
pub(crate) fn accept_key(key: &str) -> String {
    let mut m = key.as_bytes().to_vec();
    m.extend_from_slice(b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
    base64(&sha1(&m))
}

/// The 101 response of the handshake.
pub(crate) fn upgrade_response(key: &str) -> String {
    format!(
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: {}\r\n\r\n",
        accept_key(key)
    )
}

/// A WebSocket frame (server to browser: not masked) in one piece, appended after clearing `out`.
pub(crate) fn frame_into(out: &mut Vec<u8>, opcode: u8, data: &[u8]) {
    out.clear();
    out.push(0x80 | opcode);
    let n = data.len();
    if n < 126 {
        out.push(n as u8);
    } else if n < 65536 {
        out.push(126);
        out.extend_from_slice(&(n as u16).to_be_bytes());
    } else {
        out.push(127);
        out.extend_from_slice(&(n as u64).to_be_bytes());
    }
    out.extend_from_slice(data);
}

pub(crate) fn frame(opcode: u8, data: &[u8]) -> Vec<u8> {
    let mut v = Vec::new();
    frame_into(&mut v, opcode, data);
    v
}

/// Header and payload leave in one write (two tiny segments per packet otherwise).
pub(crate) fn send_frame(s: &TcpStream, opcode: u8, data: &[u8]) -> bool {
    sock::write_all(s, &frame(opcode, data))
}

/// Grows `out` by `len` bytes read from the socket in chunks, so a header that promises 4 MiB does not cost 4 MiB
/// before the bytes arrive.
fn read_payload(
    s: &TcpStream,
    out: &mut Vec<u8>,
    len: usize,
    timeout_ms: i32,
    alive: &dyn Fn() -> bool,
) -> bool {
    let mut left = len;
    let mut chunk = [0u8; 16384];
    while left > 0 {
        let n = left.min(chunk.len());
        if !sock::read_exact(s, &mut chunk[..n], timeout_ms, alive) {
            return false;
        }
        out.extend_from_slice(&chunk[..n]);
        left -= n;
    }
    true
}

/// Gets the payload of a ping.
pub(crate) type PingHandler<'a> = &'a mut dyn FnMut(&[u8]);

/// Reads one whole message (joins fragments). Returns its opcode: 1 text, 2 binary, 8 close, 9 / 10 a ping / pong that
/// arrived alone (answered, nothing to process); 0 = timeout / error / protocol violation. `on_ping` gets the payload of a ping (this socket has a writer thread that sends the pong); without
/// it the pong goes out directly.
pub(crate) fn read_message(
    s: &TcpStream,
    out: &mut Vec<u8>,
    timeout_ms: i32,
    alive: &dyn Fn() -> bool,
    mut on_ping: Option<PingHandler>,
) -> u8 {
    out.clear();
    let mut first = 0u8;
    loop {
        let mut h = [0u8; 2];
        if !sock::read_exact(s, &mut h, timeout_ms, alive) {
            return 0;
        }
        let op = h[0] & 0x0F;
        // No extension is negotiated, so a reserved bit is a protocol error; reserved opcodes too.
        if h[0] & 0x70 != 0 || (3..=7).contains(&op) || op > 0xA {
            return 0;
        }
        let mut len = u64::from(h[1] & 0x7F);
        if len == 126 {
            let mut e = [0u8; 2];
            if !sock::read_exact(s, &mut e, timeout_ms, alive) {
                return 0;
            }
            len = u64::from(u16::from_be_bytes(e));
        } else if len == 127 {
            let mut e = [0u8; 8];
            if !sock::read_exact(s, &mut e, timeout_ms, alive) {
                return 0;
            }
            len = u64::from_be_bytes(e);
        }
        // RFC 6455 5.5: a control frame is 125 bytes at most and never fragmented (a 4 MB ping must not be answered,
        // again and again).
        if op >= 8 && (len > 125 || h[0] & 0x80 == 0) {
            return 0;
        }
        if len > MAX_MESSAGE as u64 || out.len() as u64 + len > MAX_MESSAGE as u64 {
            return 0;
        }
        let len = len as usize;
        let mut mask = [0u8; 4];
        let masked = h[1] & 0x80 != 0;
        if masked && !sock::read_exact(s, &mut mask, timeout_ms, alive) {
            return 0;
        }
        let at = out.len();
        if !read_payload(s, out, len, timeout_ms, alive) {
            return 0;
        }
        if masked {
            for (i, b) in out[at..].iter_mut().enumerate() {
                *b ^= mask[i & 3];
            }
        }
        if op >= 8 {
            // control frames may arrive between fragments
            if op == 9 {
                match on_ping.as_deref_mut() {
                    Some(f) => f(&out[at..]),
                    None => {
                        let payload = out[at..].to_vec();
                        send_frame(s, 0xA, &payload);
                    }
                }
            }
            if op == 8 {
                return 8;
            }
            out.truncate(at);
            if first == 0 && at == 0 {
                // C++ bug: it went on waiting for a data frame, so a ping (or an unsolicited pong) from the browser ended
                // the connection after the read timeout. Not a message: the caller ignores this opcode.
                return op;
            }
            continue;
        }
        if op != 0 {
            first = op;
        }
        if h[0] & 0x80 != 0 {
            return first;
        }
    }
}

// ---- JSON text, written the way `juce::JSON::toString(var, true)` did (", " and ": " between members) ----------

/// A JSON string literal as JUCE 8.0.15 (`modules/juce_core/json/juce_JSON.cpp`, `JSONFormatter::writeString`, default
/// `Encoding::utf8`) wrote it: `"` `\` `\b` `\f` `\t` `\r` `\n` escaped, every other code point below 32 as `\u00xx`,
/// everything else (DEL and all non-ASCII too) as raw UTF-8.
pub(crate) fn json_str(s: &str) -> String {
    let mut o = String::with_capacity(s.len() + 2);
    o.push('"');
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\u{8}' => o.push_str("\\b"),
            '\u{c}' => o.push_str("\\f"),
            '\t' => o.push_str("\\t"),
            '\r' => o.push_str("\\r"),
            '\n' => o.push_str("\\n"),
            c if (c as u32) < 32 => {
                o.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

pub(crate) fn rtc_offer_json(sdp: &str) -> String {
    format!("{{\"type\": \"rtc_offer\", \"sdp\": {}}}", json_str(sdp))
}

pub(crate) fn rtc_candidate_json(candidate: &str, mid: &str) -> String {
    format!(
        "{{\"type\": \"rtc_candidate\", \"candidate\": {}, \"mid\": {}}}",
        json_str(candidate),
        json_str(mid)
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    fn hex(b: &[u8]) -> String {
        b.iter().map(|x| format!("{x:02x}")).collect()
    }

    #[test]
    fn sha1_known_vectors() {
        assert_eq!(hex(&sha1(b"")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
        assert_eq!(
            hex(&sha1(b"abc")),
            "a9993e364706816aba3e25717850c26c9cd0d89d"
        );
        let long = vec![b'a'; 1_000_000];
        assert_eq!(
            hex(&sha1(&long)),
            "34aa973cd4c4daa4f61eeb2bdbad27316534016f"
        );
        // 56 bytes: the length no longer fits the block, so the padding needs a second one
        assert_eq!(
            hex(&sha1(
                b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
            )),
            "84983e441c3bd26ebaae4aa1f95129e5e54670f1"
        );
        assert_eq!(
            hex(&sha1(b"The quick brown fox jumps over the lazy dog")),
            "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12"
        );
    }

    #[test]
    fn base64_padding() {
        assert_eq!(base64(b""), "");
        assert_eq!(base64(b"f"), "Zg==");
        assert_eq!(base64(b"fo"), "Zm8=");
        assert_eq!(base64(b"foo"), "Zm9v");
        assert_eq!(base64(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn accept_key_is_the_rfc_6455_example() {
        assert_eq!(
            accept_key("dGhlIHNhbXBsZSBub25jZQ=="),
            "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="
        );
    }

    #[test]
    fn frame_lengths() {
        assert_eq!(frame(1, b"hi"), [0x81, 2, b'h', b'i']);
        let f = frame(2, &vec![7u8; 300]);
        assert_eq!(&f[..4], &[0x82, 126, 1, 44]);
        assert_eq!(f.len(), 304);
        let f = frame(2, &vec![7u8; 70000]);
        assert_eq!(&f[..2], &[0x82, 127]);
        assert_eq!(&f[2..10], &70000u64.to_be_bytes());
    }

    #[test]
    fn json_text_matches_juce_formatting() {
        assert_eq!(
            rtc_offer_json("v=0\r\n"),
            "{\"type\": \"rtc_offer\", \"sdp\": \"v=0\\r\\n\"}"
        );
        assert_eq!(
            rtc_candidate_json("candidate:1", "0"),
            "{\"type\": \"rtc_candidate\", \"candidate\": \"candidate:1\", \"mid\": \"0\"}"
        );
        assert_eq!(json_str("a\"b\\c\u{1}ก"), "\"a\\\"b\\\\c\\u0001ก\"");
        // JUCE's UTF-8 mode does not escape DEL, "/" or code points outside the BMP
        assert_eq!(json_str("\u{7f}/\u{1F600}"), "\"\u{7f}/\u{1F600}\"");
    }
}
