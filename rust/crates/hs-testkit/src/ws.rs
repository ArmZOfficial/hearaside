//! A tiny WebSocket and HTTP client for the checks (what a browser does): handshake, masked binary frames out, plain frames
//! in. One socket, one thread. The same as the C++ `WsClient` / `httpGet` of `ui-snapshot`.
use std::io::{ErrorKind, Read, Write};
use std::net::TcpStream;
use std::time::{Duration, Instant};

pub struct WsClient {
    sock: Option<TcpStream>,
    pending: Vec<u8>,
    pub status: i32,
}

impl Default for WsClient {
    fn default() -> Self {
        Self::new()
    }
}

impl WsClient {
    pub fn new() -> Self {
        WsClient {
            sock: None,
            pending: Vec::new(),
            status: 0,
        }
    }

    /// The handshake; `true` = 101 Switching Protocols (`status` has the HTTP status either way).
    pub fn open(&mut self, port: u16, path: &str, origin: &str) -> bool {
        let Ok(mut s) =
            TcpStream::connect_timeout(&([127, 0, 0, 1], port).into(), Duration::from_secs(2))
        else {
            return false;
        };
        let _ = s.set_nodelay(true);
        let req = format!(
            "GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\
             Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n{}\r\n",
            if origin.is_empty() {
                String::new()
            } else {
                format!("Origin: {origin}\r\n")
            }
        );
        if s.write_all(req.as_bytes()).is_err() {
            return false;
        }
        let mut head = Vec::new();
        let mut c = [0u8; 1];
        let _ = s.set_read_timeout(Some(Duration::from_secs(3)));
        while !head.ends_with(b"\r\n\r\n") && head.len() < 4096 {
            if !matches!(s.read(&mut c), Ok(1)) {
                return false;
            }
            head.push(c[0]);
        }
        self.status = status_of(&head);
        self.sock = Some(s);
        self.status == 101
    }

    /// One masked binary message.
    pub fn send(&mut self, payload: &[u8]) -> bool {
        let Some(s) = self.sock.as_mut() else {
            return false;
        };
        let mask = [1u8, 2, 3, 4];
        let mut f = vec![0x82];
        if payload.len() < 126 {
            f.push(0x80 | payload.len() as u8);
        } else if let Ok(n) = u16::try_from(payload.len()) {
            f.push(0x80 | 126);
            f.extend_from_slice(&n.to_be_bytes());
        } else {
            f.push(0x80 | 127);
            f.extend_from_slice(&(payload.len() as u64).to_be_bytes());
        }
        f.extend_from_slice(&mask);
        f.extend(payload.iter().enumerate().map(|(i, b)| b ^ mask[i & 3]));
        s.write_all(&f).is_ok()
    }

    /// One message (binary or text); `None` = nothing within `timeout_ms` or closed (`closed` says which). Bytes of a frame
    /// that has not come whole stay in the buffer for the next call.
    pub fn receive(&mut self, timeout_ms: u64, closed: &mut bool) -> Option<Vec<u8>> {
        let deadline = Instant::now() + Duration::from_millis(timeout_ms.max(1));
        loop {
            if let Some((op, body, used)) = frame(&self.pending) {
                self.pending.drain(..used);
                if op == 8 {
                    *closed = true;
                    return None;
                }
                return Some(body);
            }
            if self.pending.len() >= 2 && self.pending[1] & 0x7F == 127 && self.pending.len() >= 10
            {
                let len = u64::from_be_bytes(self.pending[2..10].try_into().ok()?);
                if len > MAX_FRAME {
                    *closed = true; // not a frame of ours
                    return None;
                }
            }
            let s = self.sock.as_mut()?;
            let left = deadline.saturating_duration_since(Instant::now());
            if left.is_zero() {
                return None;
            }
            let _ = s.set_read_timeout(Some(left));
            let mut buf = [0u8; 8192];
            match s.read(&mut buf) {
                Ok(0) => {
                    *closed = true;
                    return None;
                }
                Ok(n) => self.pending.extend_from_slice(&buf[..n]),
                Err(e) if matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) => {
                    return None;
                }
                Err(_) => {
                    *closed = true;
                    return None;
                }
            }
        }
    }
}

/// The biggest frame a check will wait for.
const MAX_FRAME: u64 = 16 << 20;

/// The first whole unmasked frame in `b`: `(opcode, payload, bytes used)`.
fn frame(b: &[u8]) -> Option<(u8, Vec<u8>, usize)> {
    if b.len() < 2 {
        return None;
    }
    let (mut len, mut off) = (u64::from(b[1] & 0x7F), 2usize);
    if len == 126 {
        if b.len() < 4 {
            return None;
        }
        len = u64::from(u16::from_be_bytes([b[2], b[3]]));
        off = 4;
    } else if len == 127 {
        if b.len() < 10 {
            return None;
        }
        len = u64::from_be_bytes(b[2..10].try_into().ok()?);
        off = 10;
    }
    let end = off.checked_add(
        usize::try_from(len)
            .ok()
            .filter(|&l| l as u64 <= MAX_FRAME)?,
    )?;
    (b.len() >= end).then(|| (b[0] & 0x0F, b[off..end].to_vec(), end))
}

fn status_of(head: &[u8]) -> i32 {
    String::from_utf8_lossy(head)
        .split(' ')
        .nth(1)
        .and_then(|s| s.trim().parse().ok())
        .unwrap_or(0)
}

/// `GET path` on 127.0.0.1:`port`: `(status, body)`; status 0 = no connection.
pub fn http_get(port: u16, path: &str) -> (i32, String) {
    let Ok(mut s) =
        TcpStream::connect_timeout(&([127, 0, 0, 1], port).into(), Duration::from_secs(2))
    else {
        return (0, String::new());
    };
    let req = format!("GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n");
    if s.write_all(req.as_bytes()).is_err() {
        return (0, String::new());
    }
    let _ = s.set_read_timeout(Some(Duration::from_secs(3)));
    let mut all = Vec::new();
    let mut buf = [0u8; 4096];
    while let Ok(n) = s.read(&mut buf) {
        if n == 0 {
            break;
        }
        all.extend_from_slice(&buf[..n]);
    }
    let text = String::from_utf8_lossy(&all).into_owned();
    let body = text
        .split_once("\r\n\r\n")
        .map(|(_, b)| b.to_owned())
        .unwrap_or_default();
    (status_of(text.as_bytes()), body)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;

    #[test]
    fn a_frame_that_comes_in_pieces_is_kept_not_dropped() {
        // C++ bug: a 5 ms wait ran out in the middle of a frame, the half was thrown away and the next read began inside it
        let mut whole = vec![0x82, 6];
        whole.extend_from_slice(b"abcdef");
        whole.extend_from_slice(&[0x82, 2, b'x', b'y']);
        assert!(frame(&whole[..1]).is_none() && frame(&whole[..5]).is_none());
        let (op, body, used) = frame(&whole).unwrap();
        assert_eq!((op, body.as_slice(), used), (2, &b"abcdef"[..], 8));
        assert_eq!(frame(&whole[used..]).unwrap().1, b"xy");
        let mut long = vec![0x82, 126, 1, 0];
        long.extend(std::iter::repeat_n(7u8, 256));
        assert!(frame(&long[..200]).is_none());
        assert_eq!(frame(&long).unwrap().1.len(), 256);
        // a length no check will wait for is not a frame
        let mut huge = vec![0x82, 127];
        huge.extend_from_slice(&u64::MAX.to_be_bytes());
        assert!(frame(&huge).is_none());
    }

    #[test]
    fn receive_waits_for_the_rest_of_a_slow_frame() {
        let l = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = l.local_addr().unwrap().port();
        let server = std::thread::spawn(move || {
            let (mut s, _) = l.accept().unwrap();
            let mut buf = [0u8; 1024];
            let _ = s.read(&mut buf).unwrap();
            s.write_all(b"HTTP/1.1 101 Switching Protocols\r\n\r\n")
                .unwrap();
            s.write_all(&[0x82, 4, 1, 2]).unwrap(); // half a frame
            s.flush().unwrap();
            std::thread::sleep(Duration::from_millis(150));
            s.write_all(&[3, 4]).unwrap();
            s.write_all(&[0x81, 2, b'h', b'i']).unwrap();
            std::thread::sleep(Duration::from_millis(300));
        });
        let mut c = WsClient::new();
        assert!(c.open(port, "/x", ""));
        let mut closed = false;
        assert!(c.receive(20, &mut closed).is_none(), "nothing whole yet");
        assert_eq!(
            c.receive(1000, &mut closed).unwrap(),
            [1, 2, 3, 4],
            "the half was kept"
        );
        assert_eq!(c.receive(1000, &mut closed).unwrap(), b"hi");
        assert!(!closed);
        server.join().unwrap();
    }

    #[test]
    fn a_payload_over_64_kib_is_sent_with_a_64_bit_length() {
        let l = TcpListener::bind(("127.0.0.1", 0)).unwrap();
        let port = l.local_addr().unwrap().port();
        let server = std::thread::spawn(move || {
            let (mut s, _) = l.accept().unwrap();
            let mut buf = [0u8; 2048];
            let _ = s.read(&mut buf).unwrap();
            s.write_all(b"HTTP/1.1 101 Switching Protocols\r\n\r\n")
                .unwrap();
            let mut head = [0u8; 14]; // 2 + 8 + 4
            s.read_exact(&mut head).unwrap();
            let len = u64::from_be_bytes(head[2..10].try_into().unwrap()) as usize;
            let mask: [u8; 4] = head[10..14].try_into().unwrap();
            let mut body = vec![0u8; len];
            s.read_exact(&mut body).unwrap();
            for (i, b) in body.iter_mut().enumerate() {
                *b ^= mask[i & 3];
            }
            (head[0], head[1], body)
        });
        let mut c = WsClient::new();
        assert!(c.open(port, "/x", ""));
        let payload: Vec<u8> = (0..70_000u32).map(|i| (i % 251) as u8).collect();
        assert!(c.send(&payload));
        let (op, len_byte, body) = server.join().unwrap();
        assert_eq!((op, len_byte), (0x82, 0x80 | 127));
        assert_eq!(body, payload);
    }
}
