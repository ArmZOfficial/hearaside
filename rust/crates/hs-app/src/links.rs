//! HEARASIDE share links: `https://<any host>/l/<token>` (listen) and `/s/<token>` (send in): `plugins/common/Links.h`. The
//! host can be the permanent site, a trycloudflare tunnel or an address on the Wi-Fi. Pure functions.

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Kind {
    #[default]
    None,
    Listen,
    Send,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Parsed {
    pub kind: Kind,
    /// `https://host[:port]` as typed
    pub base: String,
    /// the part after `/l/` or `/s/`
    pub token: String,
}

fn starts_with_ignore_case(s: &str, prefix: &str) -> bool {
    s.len() >= prefix.len()
        && s.is_char_boundary(prefix.len())
        && s[..prefix.len()].eq_ignore_ascii_case(prefix)
}

/// What a pasted link means. A site may sit under a sub-path: the last two parts of the path count.
pub fn parse(typed: &str) -> Parsed {
    let none = Parsed::default();
    let t = typed.trim();
    if !starts_with_ignore_case(t, "https://") && !starts_with_ignore_case(t, "http://") {
        return none;
    }
    let t = t.split('#').next().unwrap_or("");
    let t = t.split('?').next().unwrap_or("").trim_end_matches('/');
    let Some(scheme_end) = t.find("://").map(|i| i + 3) else {
        return none;
    };
    let Some(path_start) = t[scheme_end..].find('/').map(|i| i + scheme_end) else {
        return none;
    };
    let host = &t[scheme_end..path_start];
    if host.is_empty() || host.contains([' ', '\t', '\\', '@']) {
        return none;
    }
    let kept: Vec<&str> = t[path_start..]
        .split('/')
        .filter(|p| !p.is_empty())
        .collect();
    if kept.len() < 2 {
        return none;
    }
    let (kind, token) = (kept[kept.len() - 2].to_lowercase(), kept[kept.len() - 1]);
    if token.len() < 8
        || !token
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
    {
        return none;
    }
    let kind = match kind.as_str() {
        "l" => Kind::Listen,
        "s" => Kind::Send,
        _ => return none,
    };
    Parsed {
        kind,
        base: t[..path_start].to_owned(),
        token: token.to_owned(),
    }
}

pub fn is_listen(t: &str) -> bool {
    parse(t).kind == Kind::Listen
}

pub fn is_send(t: &str) -> bool {
    parse(t).kind == Kind::Send
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn what_a_pasted_link_means() {
        assert_eq!(
            parse("https://hearaside.vercel.app/l/7Kq2mW9fAbCd").kind,
            Kind::Listen
        );
        assert_eq!(
            parse("https://abc-def.trycloudflare.com/s/7Kq2mW9fAbCd/").kind,
            Kind::Send
        );
        assert_eq!(
            parse("http://192.168.1.5:47810/s/7Kq2mW9fAbCd").kind,
            Kind::Send
        );
        let p = parse("https://x.example/l/7Kq2mW9fAbCd?x=1");
        assert_eq!(
            (p.token.as_str(), p.base.as_str()),
            ("7Kq2mW9fAbCd", "https://x.example")
        );
        assert!(
            parse("hello").kind == Kind::None
                && parse("https://example.com/").kind == Kind::None
                && parse("https://example.com/x/7Kq2mW9fAbCd").kind == Kind::None
                && parse("ftp://h/l/7Kq2mW9fAbCd").kind == Kind::None
        );
        assert_eq!(
            parse("https://h/l/abc").kind,
            Kind::None,
            "a short token is refused"
        );
    }

    #[test]
    fn sub_paths_hosts_and_odd_input() {
        assert!(is_listen("HTTPS://Site.example/app/l/7Kq2mW9fAbCd"));
        assert!(is_send("  https://h.example/S/7Kq2mW9fAbCd#frag "));
        assert!(
            !is_send("https://user@h.example/s/7Kq2mW9fAbCd"),
            "no credentials in a host"
        );
        assert!(!is_send("https:///s/7Kq2mW9fAbCd"));
        assert!(!is_send("https://h.example/s/7Kq2mW9f!bCd"));
        assert!(
            !is_listen("\u{0e01}\u{0e02}\u{0e03}\u{0e04}\u{0e05}"),
            "no panic on a cut inside a character"
        );
        assert!(!is_listen(""));
    }
}
