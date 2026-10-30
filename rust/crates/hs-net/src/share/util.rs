//! Small pure helpers of the share server: tokens, constant-time compare, clocks, Origin rules, address picking,
//! the trycloudflare URL parser and `juce::var`-like reads of the JSON control messages.
use hs_bus::paced_send::SendQueue;
use serde_json::Value;
use std::net::{IpAddr, Ipv4Addr};
use std::sync::OnceLock;
use std::sync::atomic::{AtomicU32, Ordering};
use std::time::Instant;

/// Milliseconds on a wrapping `u32` clock, like `juce::Time::getMillisecondCounter()`. Never 0 (callers use 0 for
/// "not yet").
pub(crate) fn now_ms() -> u32 {
    static START: OnceLock<Instant> = OnceLock::new();
    let start = *START.get_or_init(Instant::now);
    (start.elapsed().as_millis() as u32).wrapping_add(1000)
}

/// An `f32` in an atomic (the C++ `std::atomic<float>`).
#[derive(Default)]
pub(crate) struct AtomicF32(AtomicU32);

impl AtomicF32 {
    pub(crate) fn new(v: f32) -> Self {
        Self(AtomicU32::new(v.to_bits()))
    }
    pub(crate) fn load(&self) -> f32 {
        f32::from_bits(self.0.load(Ordering::SeqCst))
    }
    pub(crate) fn store(&self, v: f32) {
        self.0.store(v.to_bits(), Ordering::SeqCst);
    }
}

/// Fills `dst` from the OS's secure random source.
pub(crate) fn secure_random(dst: &mut [u8]) -> bool {
    getrandom::fill(dst).is_ok()
}

/// Compares two secrets without stopping at the first different byte. Different lengths are not secret (every token
/// has one length).
pub(crate) fn ct_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    a.iter().zip(b).fold(0u8, |acc, (x, y)| acc | (x ^ y)) == 0
}

/// Keeps the pongs a connection answers its browser's pings with from piling up behind a stuck line.
///
/// C++ bug (a real race, found as a flaky test): the cap counted every control byte waiting in the queue, the
/// WebRTC offer and candidates too (an offer alone is over 1 KiB). A ping that arrived while those were still waiting
/// for their next flush got no pong at all. Only the pong bytes count now (a bound on them: whatever is still
/// waiting is never more than the control bytes in the queue).
#[derive(Default)]
pub(crate) struct PongBudget(usize);

impl PongBudget {
    /// Queues `frame` unless `cap` bytes of pongs wait already. `false` = dropped.
    pub(crate) fn push(&mut self, q: &mut SendQueue, frame: &[u8], cap: usize) -> bool {
        let waiting = self.0.min(q.control_pending());
        if waiting >= cap {
            self.0 = waiting;
            return false;
        }
        self.0 = waiting + frame.len();
        q.append(frame);
        true
    }
}

/// `path` is `prefix` + `token`, with a non-empty token (an empty token means "no such link", not "any path").
pub(crate) fn path_is(path: &str, prefix: &str, token: &str) -> bool {
    !token.is_empty()
        && path
            .strip_prefix(prefix)
            .is_some_and(|rest| ct_eq(rest.as_bytes(), token.as_bytes()))
}

pub(crate) fn is_private_v4(ip: Ipv4Addr) -> bool {
    let o = ip.octets();
    o[0] == 10 || (o[0] == 192 && o[1] == 168) || (o[0] == 172 && (16..=31).contains(&o[1]))
}

/// The address for LAN links: the first private one, else the first that is not loopback / link-local, else 127.0.0.1.
pub(crate) fn pick_lan(addrs: &[Ipv4Addr]) -> Ipv4Addr {
    let mut best = Ipv4Addr::LOCALHOST;
    for &a in addrs {
        if is_private_v4(a) {
            return a;
        }
        if !a.is_loopback() && !a.is_link_local() && best == Ipv4Addr::LOCALHOST {
            best = a;
        }
    }
    best
}

/// Radmin VPN gives every machine in its networks an address in 26.0.0.0/8.
pub(crate) fn pick_radmin(addrs: &[Ipv4Addr]) -> Option<Ipv4Addr> {
    addrs.iter().copied().find(|a| a.octets()[0] == 26)
}

/// The IPv4 addresses of this computer in the order the OS lists them.
pub(crate) fn local_ipv4() -> Vec<Ipv4Addr> {
    if_addrs::get_if_addrs()
        .map(|list| {
            list.into_iter()
                .filter_map(|i| match i.ip() {
                    IpAddr::V4(v) => Some(v),
                    IpAddr::V6(_) => None,
                })
                .collect()
        })
        .unwrap_or_default()
}

/// The Origin rule. `o` is the header value; requests without an Origin (App Audio, tools) are always allowed. Allowed
/// are this Hub's own addresses (also the Radmin one and any other interface, not only the one used for the LAN
/// link), the tunnel's address and the configured share web site.
pub(crate) fn origin_allowed(
    origin: &str,
    port: u16,
    local: &[Ipv4Addr],
    public_base: &str,
    allowed_origin: &str,
) -> bool {
    let o = origin.trim().to_lowercase();
    if o.is_empty() {
        return true;
    }
    if o == format!("http://127.0.0.1:{port}")
        || o == format!("http://localhost:{port}")
        || o == format!("http://[::1]:{port}")
        || o == format!("http://{}:{port}", pick_lan(local))
        || local.iter().any(|a| o == format!("http://{a}:{port}"))
    {
        return true;
    }
    (!public_base.is_empty() && o == public_base.to_lowercase())
        || (!allowed_origin.is_empty() && o == allowed_origin)
}

/// `https://<label>.trycloudflare.com` in cloudflared's output. `api.trycloudflare.com` is cloudflared's own
/// service host, which its error lines print ("Post https://api.trycloudflare.com/tunnel: dial tcp ..."): not a tunnel.
pub(crate) fn find_trycloudflare_url(text: &[u8]) -> Option<String> {
    const SCHEME: &[u8] = b"https://";
    const SUFFIX: &[u8] = b".trycloudflare.com";
    let mut from = 0;
    while let Some(at) = text[from..]
        .windows(SCHEME.len())
        .position(|w| w == SCHEME)
        .map(|p| p + from)
    {
        let start = at + SCHEME.len();
        let label_len = text[start..]
            .iter()
            .take_while(|b| matches!(b, b'a'..=b'z' | b'0'..=b'9' | b'-'))
            .count();
        let end = start + label_len;
        if label_len > 0
            && text[end..].starts_with(SUFFIX)
            && &text[start..end] != b"api"
            && !text
                .get(end + SUFFIX.len())
                .is_some_and(|c| c.is_ascii_alphanumeric() || *c == b'-')
        {
            return Some(String::from_utf8_lossy(&text[at..end + SUFFIX.len()]).into_owned());
        }
        from = start;
    }
    None
}

// ---- reading the JSON control messages like `juce::var` did ------------------------------------------------------

/// `j.hasProperty(name)`: an object that has the member.
pub(crate) fn has(j: &Value, name: &str) -> bool {
    j.get(name).is_some()
}

/// `var::toString()` for the values the pages send.
pub(crate) fn var_string(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        Value::Number(n) => n.to_string(),
        Value::Bool(b) => (if *b { "1" } else { "0" }).to_owned(),
        _ => String::new(),
    }
}

/// Leading integer of a string like `String::getIntValue()`.
fn int_prefix(s: &str) -> i64 {
    let t = s.trim_start();
    let (neg, digits) = match t.strip_prefix('-') {
        Some(r) => (true, r),
        None => (false, t.strip_prefix('+').unwrap_or(t)),
    };
    let n = digits
        .bytes()
        .take_while(u8::is_ascii_digit)
        .take(18)
        .fold(0i64, |a, d| a * 10 + i64::from(d - b'0'));
    if neg { -n } else { n }
}

/// `int(var)`: doubles truncate, strings parse their leading number, booleans are 0 / 1.
pub(crate) fn var_int(v: &Value) -> i32 {
    let n = match v {
        Value::Number(n) => n.as_f64().map_or(0, |f| f as i64),
        Value::String(s) => int_prefix(s),
        Value::Bool(b) => i64::from(*b),
        _ => 0,
    };
    n.clamp(i64::from(i32::MIN), i64::from(i32::MAX)) as i32
}

/// `bool(var)`: numbers are true when not 0, strings when they are a non-zero number or "true" / "yes".
pub(crate) fn var_bool(v: &Value) -> bool {
    match v {
        Value::Bool(b) => *b,
        Value::Number(n) => n.as_f64().is_some_and(|f| f != 0.0),
        Value::String(s) => {
            int_prefix(s) != 0
                || s.trim().eq_ignore_ascii_case("true")
                || s.trim().eq_ignore_ascii_case("yes")
        }
        _ => false,
    }
}

/// A text control message as a JSON object (anything else, also text too long to be one, is ignored).
pub(crate) fn parse_control(msg: &[u8]) -> Option<Value> {
    if msg.len() > 64 * 1024 {
        return None; // the longest real message is an SDP (limited to 64 KiB by the peer)
    }
    serde_json::from_slice::<Value>(msg)
        .ok()
        .filter(Value::is_object)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn v4(a: u8, b: u8, c: u8, d: u8) -> Ipv4Addr {
        Ipv4Addr::new(a, b, c, d)
    }

    #[test]
    fn constant_time_compare_and_paths() {
        assert!(ct_eq(b"abc", b"abc"));
        assert!(!ct_eq(b"abc", b"abd"));
        assert!(!ct_eq(b"abc", b"abcd"));
        assert!(path_is("/l/tok", "/l/", "tok"));
        assert!(!path_is("/l/tok", "/l/", "tom"));
        assert!(!path_is("/l/tok2", "/l/", "tok"));
        assert!(!path_is("/s/tok", "/l/", "tok"));
        // an empty token disables the link: "/l/" is not a valid link
        assert!(!path_is("/l/", "/l/", ""));
    }

    #[test]
    fn pongs_are_not_dropped_for_the_signaling_waiting_with_them() {
        let mut q = SendQueue::new();
        let mut b = PongBudget::default();
        q.append(&vec![7u8; 1316]); // an rtc_offer frame nobody flushed yet
        let pong = [0x8A, 1, b'x'];
        assert!(b.push(&mut q, &pong, 1024), "the offer does not count");
        // a flood of pings is still bounded
        let mut kept = 0;
        for _ in 0..1000 {
            kept += usize::from(b.push(&mut q, &pong, 1024));
        }
        assert!((300..400).contains(&(kept + 1)), "{kept}");
        // sent: room again
        assert!(q.flush(|p| p.len() as isize));
        assert!(b.push(&mut q, &pong, 1024));
    }

    #[test]
    fn lan_and_radmin_address_picking() {
        let lo = v4(127, 0, 0, 1);
        assert_eq!(pick_lan(&[]), lo);
        assert_eq!(pick_lan(&[lo, v4(169, 254, 3, 4)]), lo);
        assert_eq!(pick_lan(&[lo, v4(203, 0, 113, 9)]), v4(203, 0, 113, 9));
        assert_eq!(
            pick_lan(&[v4(203, 0, 113, 9), v4(192, 168, 1, 5), v4(10, 0, 0, 2)]),
            v4(192, 168, 1, 5)
        );
        // 172.x is only private inside 172.16/12 (the C++ took every 172.x address as LAN)
        assert_eq!(
            pick_lan(&[v4(172, 217, 0, 1), v4(10, 1, 1, 1)]),
            v4(10, 1, 1, 1)
        );
        assert_eq!(pick_lan(&[v4(172, 20, 0, 1)]), v4(172, 20, 0, 1));
        assert!(!is_private_v4(v4(172, 15, 0, 1)));
        assert!(!is_private_v4(v4(172, 32, 0, 1)));
        assert_eq!(
            pick_radmin(&[lo, v4(26, 4, 5, 6), v4(26, 9, 9, 9)]),
            Some(v4(26, 4, 5, 6))
        );
        assert_eq!(pick_radmin(&[lo, v4(192, 168, 1, 5)]), None);
    }

    #[test]
    fn origin_rules() {
        let local = [v4(192, 168, 1, 5), v4(26, 1, 2, 3)];
        let ok = |o: &str| {
            origin_allowed(
                o,
                47810,
                &local,
                "https://x-y.trycloudflare.com",
                "https://share.example",
            )
        };
        assert!(ok(""));
        assert!(ok("  "));
        assert!(ok("http://127.0.0.1:47810"));
        assert!(ok("HTTP://LOCALHOST:47810"));
        assert!(ok("http://192.168.1.5:47810"));
        assert!(ok("http://26.1.2.3:47810"), "Radmin address of this Hub");
        assert!(ok("http://[::1]:47810"));
        assert!(ok("https://x-y.trycloudflare.com"));
        assert!(ok("https://share.example"));
        assert!(!ok("http://127.0.0.1:47811"));
        assert!(!ok("http://evil.example"));
        assert!(!ok("null"));
        assert!(!ok("http://127.0.0.1:47810.evil.example"));
        assert!(!ok("https://other.trycloudflare.com"));
        assert!(!origin_allowed(
            "https://share.example",
            47810,
            &local,
            "",
            ""
        ));
    }

    #[test]
    fn trycloudflare_url_parser() {
        let t = b"2026 INF |  https://quiet-river-12.trycloudflare.com  |\n";
        assert_eq!(
            find_trycloudflare_url(t).as_deref(),
            Some("https://quiet-river-12.trycloudflare.com")
        );
        // the service host in an error line is not a tunnel
        let e = b"ERR failed to request quick Tunnel: Post \"https://api.trycloudflare.com/tunnel\": dial tcp: no such host";
        assert_eq!(find_trycloudflare_url(e), None);
        // ... but a tunnel later in the same output still is
        let both = [&e[..], b"\nhttps://abc.trycloudflare.com\n"].concat();
        assert_eq!(
            find_trycloudflare_url(&both).as_deref(),
            Some("https://abc.trycloudflare.com")
        );
        assert_eq!(
            find_trycloudflare_url(b"https://abc.trycloudflare.co"),
            None
        );
        assert_eq!(find_trycloudflare_url(b"https://.trycloudflare.com"), None);
        assert_eq!(
            find_trycloudflare_url(b"http://abc.trycloudflare.com"),
            None
        );
        assert_eq!(
            find_trycloudflare_url(b"https://abc.trycloudflare.comx"),
            None
        );
        assert_eq!(
            find_trycloudflare_url(b"https://A.trycloudflare.com"),
            None,
            "labels are lower case"
        );
        assert_eq!(find_trycloudflare_url(b""), None);
    }

    #[test]
    fn json_var_conversions() {
        let j = parse_control(
            br#"{"codec":"opus","bitrate":"64000x","a":true,"b":"yes","c":0,"d":1.9}"#,
        )
        .unwrap();
        assert!(has(&j, "codec") && !has(&j, "zz"));
        assert_eq!(var_string(&j["codec"]), "opus");
        assert_eq!(var_int(&j["bitrate"]), 64000);
        assert!(var_bool(&j["a"]) && var_bool(&j["b"]) && !var_bool(&j["c"]));
        assert_eq!(var_int(&j["d"]), 1);
        assert!(parse_control(b"[1,2]").is_none());
        assert!(parse_control(b"nonsense").is_none());
        assert!(parse_control(&vec![b' '; 70_000]).is_none());
    }

    #[test]
    fn clock_never_zero_and_atomic_float() {
        assert_ne!(now_ms(), 0);
        let f = AtomicF32::new(-1.0);
        assert_eq!(f.load(), -1.0);
        f.store(2.5);
        assert_eq!(f.load(), 2.5);
    }
}
