//! The one HTTP(S) client of the crate: `ureq` over rustls (aws-lc-rs, already in the tree through str0m) verifying
//! certificates with the operating system's store (`rustls-platform-verifier`), so a company proxy or antivirus root
//! that the user already trusts works, and the Mozilla list does not have to be updated with each release.
//!
//! Calls block the calling thread and are meant for the crate's worker threads only. What the C++ `juce::WebInputStream`
//! calls did not do and this does:
//! * a hard deadline for the whole call and a cap on the size of an answer;
//! * redirects are followed here, by hand, for `GET` only and a few hops: never for a request with a body (a secret in
//!   it), never to a host a secret may not go to (`secure_enough`), and the `Authorization` header is dropped when the
//!   origin changes;
//! * no secret over plain HTTP to a host that is not this computer (`secure_enough`);
//! * the proxy of the environment (`HTTPS_PROXY`, `NO_PROXY`, on Windows the system proxy setting) is used, except for this
//!   computer's own addresses. Known gap: no PAC / WPAD script, no per-protocol `http=..;https=..` proxy list and no
//!   `ProxyOverride` of the Windows setting.
use std::sync::Arc;
use std::time::Duration;
use ureq::http;
use ureq::tls::{RootCerts, TlsConfig, TlsProvider};

/// What came back. `status` 0 = no answer at all (no network, refused, timed out, bad TLS).
#[derive(Default, Clone)]
pub(crate) struct Reply {
    pub status: u16,
    pub body: Vec<u8>,
}

/// The body can hold tokens: Debug shows its size only.
impl std::fmt::Debug for Reply {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("Reply")
            .field("status", &self.status)
            .field("body_len", &self.body.len())
            .finish()
    }
}

impl Reply {
    pub fn ok(&self) -> bool {
        (200..300).contains(&self.status)
    }

    /// The body as JSON, `Null` when it is not.
    pub fn json(&self) -> serde_json::Value {
        serde_json::from_slice(&self.body).unwrap_or(serde_json::Value::Null)
    }
}

pub(crate) struct Client {
    proxied: ureq::Agent,
    direct: ureq::Agent,
    max_body: u64,
}

fn agent(connect: Duration, total: Duration, use_proxy: bool) -> ureq::Agent {
    let provider = Arc::new(rustls::crypto::aws_lc_rs::default_provider());
    let tls = TlsConfig::builder()
        .provider(TlsProvider::Rustls)
        .root_certs(RootCerts::PlatformVerifier)
        .unversioned_rustls_crypto_provider(provider)
        .build();
    let b = ureq::Agent::config_builder()
        .tls_config(tls)
        .http_status_as_error(false)
        .max_redirects(0) // followed by `Client::send`, with the rules above
        .max_redirects_will_error(false)
        .timeout_connect(Some(connect))
        .timeout_global(Some(total))
        .user_agent("HEARASIDE");
    let b = if use_proxy { b } else { b.proxy(None) };
    b.build().new_agent()
}

impl Client {
    /// `total`: the deadline for one call, connect included.
    pub fn new(connect: Duration, total: Duration, max_body: u64) -> Self {
        Self {
            proxied: agent(connect, total, true),
            direct: agent(connect, total, false),
            max_body,
        }
    }

    /// One request. `bearer`: sent as `Authorization: Bearer`. `follow`: redirects to follow (`GET` only).
    pub fn json(
        &self,
        method: &str,
        url: &str,
        body: Option<&serde_json::Value>,
        bearer: &str,
        follow: u32,
    ) -> Reply {
        let mut headers = Vec::new();
        if !bearer.is_empty() {
            headers.push(("Authorization", format!("Bearer {bearer}")));
        }
        match body {
            Some(b) => {
                headers.push(("Content-Type", "application/json".to_owned()));
                self.send(method, url, headers, b.to_string().into_bytes(), follow)
            }
            None => self.send(method, url, headers, Vec::new(), follow),
        }
    }

    pub fn send(
        &self,
        method: &str,
        url: &str,
        mut headers: Vec<(&str, String)>,
        body: Vec<u8>,
        follow: u32,
    ) -> Reply {
        let mut url = url.to_owned();
        let mut hops = 0;
        loop {
            let agent = if is_loopback(&url) {
                &self.direct
            } else {
                &self.proxied
            };
            let mut req = http::Request::builder().method(method).uri(url.as_str());
            for (k, v) in &headers {
                req = req.header(*k, v);
            }
            let Ok(req) = req.body(body.clone()) else {
                return Reply::default();
            };
            let Ok(resp) = agent.run(req) else {
                return Reply::default();
            };
            let status = resp.status().as_u16();
            if matches!(status, 301 | 302 | 303 | 307 | 308) && method == "GET" && hops < follow {
                let next = resp
                    .headers()
                    .get("location")
                    .and_then(|l| l.to_str().ok())
                    .and_then(|l| resolve_location(&url, l));
                match next {
                    // a hop to somewhere a secret may not go: stop here, the caller sees the redirect status
                    Some(next) if redirect_ok(&url, &next) => {
                        if origin_of(&next) != origin_of(&url) {
                            headers.retain(|(k, _)| !k.eq_ignore_ascii_case("authorization"));
                        }
                        url = next;
                        hops += 1;
                        continue;
                    }
                    _ => {
                        return Reply {
                            status,
                            body: Vec::new(),
                        };
                    }
                }
            }
            let body = resp
                .into_body()
                .with_config()
                .limit(self.max_body)
                .read_to_vec()
                .unwrap_or_default();
            return Reply { status, body };
        }
    }
}

/// A plain `GET` of a JSON object (no secret, redirects followed), for HAS Karaoke's update check. Blocks: worker
/// threads only. `None` on no answer, a non-2xx status or a body that is not a JSON object.
pub fn get_json(url: &str, connect: Duration, total: Duration) -> Option<serde_json::Value> {
    let r = Client::new(connect, total, 1 << 20).json("GET", url, None, "", 3);
    let v = r.json();
    (r.ok() && v.is_object()).then_some(v)
}

/// A redirect may be followed when the target may carry what the request carries, and an https request is never sent on
/// to plain http (not even to this computer: that is a downgrade).
fn redirect_ok(from: &str, to: &str) -> bool {
    secure_enough(to)
        && !(from.to_ascii_lowercase().starts_with("https://")
            && !to.to_ascii_lowercase().starts_with("https://"))
}

/// The URL a `Location` header points at, from the URL that was asked.
fn resolve_location(from: &str, location: &str) -> Option<String> {
    let location = location.trim();
    if location.is_empty() {
        return None;
    }
    if location.contains("://") {
        return Some(location.to_owned());
    }
    let (scheme, rest) = from.split_once("://")?;
    let host = rest.split(['/', '?', '#']).next()?;
    if let Some(path) = location.strip_prefix('/') {
        return Some(format!("{scheme}://{host}/{path}"));
    }
    let path = rest[host.len()..].split(['?', '#']).next().unwrap_or("");
    let dir = path.rfind('/').map_or("/", |i| &path[..=i]);
    Some(format!("{scheme}://{host}{dir}{location}"))
}

/// The host part of a URL, lowercase, without brackets; `None` when it has none or has user info.
fn host_of(url: &str) -> Option<String> {
    let rest = url.split_once("://")?.1;
    let host_port = rest.split(['/', '?', '#']).next().unwrap_or("");
    if host_port.contains('@') {
        return None;
    }
    let host = if let Some(v6) = host_port.strip_prefix('[') {
        v6.split(']').next().unwrap_or("")
    } else {
        host_port.split(':').next().unwrap_or("")
    };
    (!host.is_empty()).then(|| host.to_ascii_lowercase())
}

fn is_loopback(url: &str) -> bool {
    host_of(url).is_some_and(|h| {
        h == "localhost" || h == "127.0.0.1" || h == "::1" || h.ends_with(".localhost")
    })
}

/// `true` when `base` may carry a secret: https anywhere, http only to this computer (a dev server, the tests).
pub(crate) fn secure_enough(base: &str) -> bool {
    let lower = base.to_ascii_lowercase();
    if lower.starts_with("https://") {
        return host_of(&lower).is_some();
    }
    lower.starts_with("http://") && is_loopback(&lower)
}

/// `scheme://host[:port]` of a URL, lowercase; empty when it has none (`ShareDirectory::originOf`).
pub fn origin_of(url: &str) -> String {
    let Some((scheme, rest)) = url.split_once("://") else {
        return String::new();
    };
    if scheme.is_empty() {
        return String::new();
    }
    let host = rest.split('/').next().unwrap_or("");
    if host.is_empty() {
        return String::new();
    }
    format!("{}://{}", scheme.to_lowercase(), host.to_lowercase())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn secrets_only_over_https_or_loopback() {
        assert!(secure_enough("https://hearaside.netlify.app"));
        assert!(secure_enough("http://127.0.0.1:3000"));
        assert!(secure_enough("http://localhost"));
        assert!(secure_enough("http://[::1]:8080/x"));
        assert!(!secure_enough("http://example.com"));
        assert!(!secure_enough("http://127.0.0.1@evil.example"));
        assert!(!secure_enough("ftp://127.0.0.1"));
        assert!(!secure_enough(""));
    }

    #[test]
    fn origin() {
        assert_eq!(
            origin_of("HTTPS://Share.Example.com/l/abc"),
            "https://share.example.com"
        );
        assert_eq!(
            origin_of("http://127.0.0.1:47871"),
            "http://127.0.0.1:47871"
        );
        assert_eq!(origin_of("nonsense"), "");
        assert_eq!(origin_of("https://"), "");
        assert_eq!(origin_of(""), "");
    }

    #[test]
    fn locations_resolve_like_a_browser() {
        let from = "https://a.example/api/v1/me?x=1";
        assert_eq!(
            resolve_location(from, "https://b.example/x").as_deref(),
            Some("https://b.example/x")
        );
        assert_eq!(
            resolve_location(from, "/other").as_deref(),
            Some("https://a.example/other")
        );
        assert_eq!(
            resolve_location(from, "peer").as_deref(),
            Some("https://a.example/api/v1/peer")
        );
        assert_eq!(
            resolve_location("http://127.0.0.1:9/me", "/x").as_deref(),
            Some("http://127.0.0.1:9/x")
        );
        assert_eq!(resolve_location(from, ""), None);
    }

    #[test]
    fn redirects_never_downgrade_or_leave_for_a_plain_host() {
        assert!(redirect_ok("https://a.example/x", "https://b.example/y"));
        assert!(
            !redirect_ok("https://a.example/x", "http://127.0.0.1/y"),
            "https to http is a downgrade"
        );
        assert!(!redirect_ok("https://a.example/x", "http://b.example/y"));
        assert!(redirect_ok("http://127.0.0.1:5/x", "http://127.0.0.1:6/y"));
        assert!(!redirect_ok("http://127.0.0.1:5/x", "http://b.example/y"));
    }

    #[test]
    fn this_computer_is_not_sent_through_a_proxy() {
        for u in [
            "http://127.0.0.1:80/x",
            "http://localhost/",
            "http://[::1]:3000/",
            "https://a.localhost/",
        ] {
            assert!(is_loopback(u), "{u}");
        }
        for u in [
            "https://hearaside.netlify.app",
            "http://192.168.1.5/",
            "http://127.0.0.1.evil.example/",
            "http://localhost.evil.example/",
        ] {
            assert!(!is_loopback(u), "{u}");
        }
    }

    #[test]
    fn reply_debug_never_shows_the_body() {
        let r = Reply {
            status: 200,
            body: br#"{"refresh_token":"SECRET-TOKEN"}"#.to_vec(),
        };
        let shown = format!("{r:?}");
        assert!(
            !shown.contains("SECRET") && shown.contains("200"),
            "{shown}"
        );
    }
}
