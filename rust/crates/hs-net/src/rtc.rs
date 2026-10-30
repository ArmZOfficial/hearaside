//! WebRTC DataChannel transport. Own and poll it on a network worker, never an audio thread.
//!
//! SDP and candidate strings use the existing `rtc_offer` / `rtc_answer` / `rtc_candidate`
//! signaling fields. STUN gathering uses the same UDP sockets as ICE and DTLS.
use std::collections::{HashSet, VecDeque};
use std::io;
use std::net::{IpAddr, SocketAddr, ToSocketAddrs, UdpSocket};
use std::time::{Duration, Instant};

use str0m::change::{SdpAnswer, SdpOffer, SdpPendingOffer};
use str0m::channel::{ChannelConfig, ChannelId, Reliability};
use str0m::ice::{StunMessageBuilder, TransId};
use str0m::net::{Protocol, Receive};
use str0m::{Candidate, Event, IceConnectionState, Input, Output, Rtc};

pub const HUB_UDP_FIRST: u16 = 47830;
pub const HUB_UDP_LAST: u16 = 47999;
const MAX_MESSAGE: usize = 65536;
const MAX_BUFFERED: usize = 256 * 1024;
const MAX_CANDIDATES: usize = 128;

#[derive(Clone, Debug)]
pub struct IceServerConfig {
    pub url: String,
    pub username: String,
    pub password: String,
}

#[derive(Clone, Debug, Default)]
pub struct RtcPeerConfig {
    /// Empty means the two default public STUN servers, matching the C++ peer.
    pub ice_servers: Vec<IceServerConfig>,
    /// `(0, 0)` requests ephemeral ports; Hub callers use `HUB_UDP_FIRST..=HUB_UDP_LAST`.
    pub port_range_begin: u16,
    pub port_range_end: u16,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RtcState {
    New,
    Connecting,
    Connected,
    Disconnected,
    Failed,
    Closed,
}

#[derive(Debug)]
pub enum RtcEvent {
    LocalDescription { kind: &'static str, sdp: String },
    LocalCandidate { candidate: String, mid: String },
    StateChange(RtcState),
    DataChannelOpen,
    DataChannelClosed,
    Message(Vec<u8>),
}

struct BoundSocket {
    socket: UdpSocket,
    local: SocketAddr,
}
struct StunBinding {
    socket: usize,
    server: SocketAddr,
    request: Vec<u8>,
    next: Instant,
    attempts: u32,
    completed: bool,
}

pub struct RtcPeer {
    rtc: Rtc,
    sockets: Vec<BoundSocket>,
    bindings: Vec<StunBinding>,
    pending: Option<SdpPendingOffer>,
    channel: Option<ChannelId>,
    open: bool,
    state: RtcState,
    events: VecDeque<RtcEvent>,
    candidates: Vec<String>,
    remote_candidates: HashSet<String>,
    receive_sources: HashSet<SocketAddr>,
    mid: Option<String>,
    started: Option<Instant>,
    established: bool,
    buffer: Vec<u8>,
}

fn invalid(message: impl ToString) -> io::Error {
    io::Error::new(io::ErrorKind::InvalidInput, message.to_string())
}

// str0m's STUN parser requires ICE MESSAGE-INTEGRITY, absent in public STUN replies.
// Only parse RFC 8489 Binding Success / XOR-MAPPED-ADDRESS for an outstanding transaction.
fn stun_mapped(packet: &[u8], request: &[u8]) -> Option<SocketAddr> {
    if packet.len() < 20
        || packet[..2] != [1, 1]
        || packet[4..8] != [0x21, 0x12, 0xa4, 0x42]
        || packet.get(8..20) != request.get(8..20)
        || u16::from_be_bytes([packet[2], packet[3]]) as usize + 20 != packet.len()
    {
        return None;
    }
    let mut pos = 20;
    while pos + 4 <= packet.len() {
        let kind = u16::from_be_bytes([packet[pos], packet[pos + 1]]);
        let len = u16::from_be_bytes([packet[pos + 2], packet[pos + 3]]) as usize;
        let data = packet.get(pos + 4..pos + 4 + len)?;
        if kind == 0x20 && data.len() >= 4 && data[0] == 0 {
            let port = u16::from_be_bytes([data[2], data[3]]) ^ 0x2112;
            let ip = match (data[1], data.len()) {
                (1, 8) => IpAddr::V4(std::net::Ipv4Addr::new(
                    data[4] ^ 0x21,
                    data[5] ^ 0x12,
                    data[6] ^ 0xa4,
                    data[7] ^ 0x42,
                )),
                (2, 20) => {
                    let mut bytes = [0u8; 16];
                    for i in 0..16 {
                        bytes[i] = data[4 + i] ^ packet[4 + i];
                    }
                    IpAddr::V6(bytes.into())
                }
                _ => return None,
            };
            return Some(SocketAddr::new(ip, port));
        }
        pos += 4 + len.div_ceil(4) * 4;
    }
    None
}

impl RtcPeer {
    /// Prepares sockets, interfaces, DNS and crypto on the calling network worker.
    /// TURN URLs return `Unsupported`; browser-side TURN remains usable with this peer.
    pub fn new(config: &RtcPeerConfig) -> io::Result<Self> {
        if (config.port_range_begin == 0) != (config.port_range_end == 0)
            || config.port_range_begin > config.port_range_end
        {
            return Err(invalid("invalid UDP port range"));
        }
        let default_servers = [
            "stun:stun.cloudflare.com:3478",
            "stun:stun.l.google.com:19302",
        ];
        let urls: Vec<&str> = if config.ice_servers.is_empty() {
            default_servers.to_vec()
        } else {
            config.ice_servers.iter().map(|s| s.url.as_str()).collect()
        };
        let mut servers = Vec::new();
        for url in urls {
            if url.starts_with("turn:") || url.starts_with("turns:") || url.starts_with("stuns:") {
                return Err(io::Error::new(
                    io::ErrorKind::Unsupported,
                    "only UDP STUN gathering is supported",
                ));
            }
            let host = url
                .strip_prefix("stun:")
                .ok_or_else(|| invalid("invalid ICE server URL"))?;
            let host = host.strip_suffix("?transport=udp").unwrap_or(host);
            if host.is_empty() || host.contains(['/', '?', '@']) {
                return Err(invalid("invalid STUN server URL"));
            }
            let authority = if host.starts_with('[') {
                if host.ends_with(']') {
                    format!("{host}:3478")
                } else {
                    host.to_owned()
                }
            } else if host.contains(':') {
                host.to_owned()
            } else {
                format!("{host}:3478")
            };
            // An unavailable public resolver must not prevent a direct LAN/VPN connection.
            if let Ok(addresses) = authority.to_socket_addrs() {
                servers.extend(addresses);
            }
        }
        servers.sort_unstable();
        servers.dedup();
        let mut ips: Vec<IpAddr> = if_addrs::get_if_addrs()?
            .into_iter()
            .map(|i| i.ip())
            .filter(|ip| !ip.is_unspecified() && !ip.is_multicast())
            // Scoped IPv6 needs scope ids, which are not carried by SDP host candidates.
            .filter(|ip| !matches!(ip, IpAddr::V6(v) if v.is_unicast_link_local()))
            .collect();
        ips.sort_unstable();
        ips.dedup();
        let mut sockets = Vec::new();
        for ip in ips {
            let ports = config.port_range_begin..=config.port_range_end;
            let mut last_error = None;
            for port in ports {
                match UdpSocket::bind(SocketAddr::new(ip, port)) {
                    Ok(socket) => {
                        socket.set_nonblocking(true)?;
                        let local = socket.local_addr()?;
                        sockets.push(BoundSocket { socket, local });
                        break;
                    }
                    Err(error) => last_error = Some(error),
                }
            }
            if sockets.is_empty()
                && let Some(error) = last_error
            {
                // Keep trying the other interfaces (e.g. one just went down).
                if error.kind() != io::ErrorKind::AddrInUse
                    && error.kind() != io::ErrorKind::AddrNotAvailable
                {
                    return Err(error);
                }
            }
        }
        if sockets.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::AddrInUse,
                "no UDP port available in requested range",
            ));
        }
        let now = Instant::now();
        let mut rtc = Rtc::builder().build(now);
        let mut candidates = Vec::new();
        for bound in &sockets {
            if let Ok(candidate) = Candidate::host(bound.local, "udp") {
                candidates.push(candidate.to_sdp_string());
                rtc.add_local_candidate(candidate);
            }
        }
        let mut bindings = Vec::new();
        for (socket, bound) in sockets.iter().enumerate() {
            for &server in &servers {
                if server.is_ipv4() != bound.local.is_ipv4()
                    || bound.local.ip().is_loopback() != server.ip().is_loopback()
                {
                    continue;
                }
                let transaction = TransId::new();
                let mut request = vec![0u8; 128];
                let len = StunMessageBuilder::new()
                    .binding()
                    .request()
                    .build(transaction)
                    .to_bytes(None, &mut request, |_, _| [0; 20])
                    .map_err(invalid)?;
                request.truncate(len);
                bindings.push(StunBinding {
                    socket,
                    server,
                    request,
                    next: now,
                    attempts: 0,
                    completed: false,
                });
            }
        }
        Ok(Self {
            rtc,
            sockets,
            bindings,
            pending: None,
            channel: None,
            open: false,
            state: RtcState::New,
            events: VecDeque::new(),
            candidates,
            remote_candidates: HashSet::new(),
            receive_sources: HashSet::new(),
            mid: None,
            started: None,
            established: false,
            buffer: vec![0; 65536],
        })
    }

    pub fn local_addresses(&self) -> impl Iterator<Item = SocketAddr> + '_ {
        self.sockets.iter().map(|s| s.local)
    }
    pub fn is_open(&self) -> bool {
        self.open
    }
    pub fn state(&self) -> RtcState {
        self.state
    }

    pub fn initiate_offer(&mut self, label: &str) -> io::Result<()> {
        if self.state != RtcState::New || self.pending.is_some() {
            return Err(invalid("peer has already started"));
        }
        let mut changes = self.rtc.sdp_api();
        self.channel = Some(changes.add_channel_with_config(ChannelConfig {
            label: label.to_owned(),
            ordered: false,
            reliability: Reliability::MaxRetransmits { retransmits: 0 },
            ..ChannelConfig::default()
        }));
        let (offer, pending) = changes.apply().ok_or_else(|| invalid("no SDP offer"))?;
        let sdp = offer.to_sdp_string();
        self.pending = Some(pending);
        self.description("offer", sdp)?;
        Ok(())
    }

    pub fn set_remote_description(&mut self, kind: &str, sdp: &str) -> io::Result<()> {
        if matches!(self.state, RtcState::Closed | RtcState::Failed) {
            return Err(invalid("peer is closed"));
        }
        if sdp.len() > 65536 {
            return Err(invalid("SDP too large"));
        }
        let embedded: HashSet<String> = sdp
            .lines()
            .filter_map(|line| line.trim().strip_prefix("a=candidate:"))
            .map(|line| format!("candidate:{line}"))
            .collect();
        if sdp
            .lines()
            .filter(|line| line.trim().starts_with("a=candidate:"))
            .count()
            > MAX_CANDIDATES
            || embedded.iter().any(|c| c.len() > 2048)
            || self.remote_candidates.union(&embedded).count() > MAX_CANDIDATES
        {
            return Err(invalid("too many remote ICE candidates"));
        }
        match kind {
            "offer" if self.state == RtcState::New && self.pending.is_none() => {
                let offer = SdpOffer::from_sdp_string(sdp).map_err(invalid)?;
                let answer = self.rtc.sdp_api().accept_offer(offer).map_err(invalid)?;
                self.description("answer", answer.to_sdp_string())?;
            }
            "answer" => {
                let answer = SdpAnswer::from_sdp_string(sdp).map_err(invalid)?;
                let pending = self
                    .pending
                    .take()
                    .ok_or_else(|| invalid("no pending offer"))?;
                if let Err(error) = self.rtc.sdp_api().accept_answer(pending, answer) {
                    self.set_state(RtcState::Failed);
                    return Err(invalid(error));
                }
            }
            _ => return Err(invalid("unexpected SDP type")),
        }
        self.remote_candidates.extend(embedded);
        Ok(())
    }

    pub fn add_remote_candidate(&mut self, candidate: &str, mid: &str) -> io::Result<()> {
        if matches!(self.state, RtcState::Closed | RtcState::Failed) {
            return Err(invalid("peer is closed"));
        }
        if candidate.len() > 2048
            || (!mid.is_empty() && self.mid.as_deref().is_some_and(|m| m != mid))
        {
            return Err(invalid("invalid candidate mid or length"));
        }
        let mut parts: Vec<&str> = candidate.split_whitespace().collect();
        let mdns = parts.get(4).is_some_and(|host| host.ends_with(".local"));
        if mdns {
            parts[4] = "127.0.0.1";
        }
        let candidate = Candidate::from_sdp_string(&parts.join(" ")).map_err(invalid)?;
        // Browsers can hide host IPs behind mDNS or offer TCP too; UDP ICE still
        // learns peer-reflexive addresses from authenticated connectivity checks.
        if mdns || candidate.proto() != Protocol::Udp {
            return Ok(());
        }
        let text = candidate.to_sdp_string();
        if self.remote_candidates.contains(&text) {
            return Ok(());
        }
        if self.remote_candidates.len() >= MAX_CANDIDATES {
            return Err(invalid("too many remote ICE candidates"));
        }
        self.remote_candidates.insert(text);
        self.rtc.add_remote_candidate(candidate);
        Ok(())
    }

    pub fn send(&mut self, data: &[u8]) -> bool {
        if !self.open || data.is_empty() || data.len() > MAX_MESSAGE {
            return false;
        }
        let Some(id) = self.channel else {
            return false;
        };
        let Some(mut channel) = self.rtc.channel(id) else {
            return false;
        };
        if channel.buffered_amount().saturating_add(data.len()) > MAX_BUFFERED {
            return false;
        }
        channel.write(true, data).unwrap_or(false)
    }

    /// Drives socket IO, ICE, DTLS, SCTP and STUN until an event or the caller's deadline.
    pub fn poll(&mut self, timeout: Duration) -> io::Result<Option<RtcEvent>> {
        let deadline = Instant::now() + timeout;
        loop {
            if let Some(event) = self.events.pop_front() {
                return Ok(Some(event));
            }
            if matches!(self.state, RtcState::Closed | RtcState::Failed) {
                return Ok(None);
            }
            let now = Instant::now();
            if !self.established
                && self
                    .started
                    .is_some_and(|t| now.duration_since(t) > Duration::from_secs(30))
            {
                self.close();
                self.set_state(RtcState::Failed);
                continue;
            }
            for binding in &mut self.bindings {
                if !binding.completed && binding.attempts < 5 && now >= binding.next {
                    let _ = self.sockets[binding.socket]
                        .socket
                        .send_to(&binding.request, binding.server);
                    binding.next = now + Duration::from_millis(500 << binding.attempts);
                    binding.attempts += 1;
                }
            }
            self.rtc
                .handle_input(Input::Timeout(now))
                .map_err(invalid)?;
            let next = loop {
                match self.rtc.poll_output().map_err(invalid)? {
                    Output::Timeout(next) => break next,
                    Output::Transmit(output) => {
                        if let Some(socket) = self.sockets.iter().find(|s| s.local == output.source)
                        {
                            // Losing a UDP packet is allowed; a failed NIC must not kill the WebSocket fallback.
                            let _ = socket.socket.send_to(&output.contents, output.destination);
                        }
                    }
                    Output::Event(event) => self.handle_event(event),
                }
            };
            for index in 0..self.sockets.len() {
                // Bound each cycle so a flooding sender cannot starve signaling or other interfaces.
                for _ in 0..64 {
                    let (len, source) = match self.sockets[index].socket.recv_from(&mut self.buffer)
                    {
                        Ok(value) => value,
                        Err(error)
                            if matches!(
                                error.kind(),
                                io::ErrorKind::WouldBlock | io::ErrorKind::ConnectionReset
                            ) =>
                        {
                            break;
                        }
                        Err(error) => return Err(error),
                    };
                    let local = self.sockets[index].local;
                    if let Some(binding) = self.bindings.iter_mut().find(|b| {
                        !b.completed
                            && b.socket == index
                            && b.server == source
                            && self.buffer[..len].get(8..20) == b.request.get(8..20)
                    }) {
                        if let Some(mapped) = stun_mapped(&self.buffer[..len], &binding.request)
                            && let Ok(candidate) = Candidate::server_reflexive(mapped, local, "udp")
                        {
                            binding.attempts = 5;
                            binding.completed = true;
                            let text = candidate.to_sdp_string();
                            if !self.candidates.contains(&text) {
                                self.rtc.add_local_candidate(candidate);
                                self.candidates.push(text.clone());
                                if let Some(mid) = &self.mid {
                                    self.events.push_back(RtcEvent::LocalCandidate {
                                        candidate: text,
                                        mid: mid.clone(),
                                    });
                                }
                            }
                        }
                        continue;
                    }
                    if let Ok(receive) =
                        Receive::new(Protocol::Udp, source, local, &self.buffer[..len])
                    {
                        // Invalid unauthenticated network traffic is ignored by the transport.
                        let input = Input::Receive(Instant::now(), receive);
                        if self.rtc.accepts(&input)
                            && (self.receive_sources.contains(&source)
                                || self.receive_sources.len() < MAX_CANDIDATES)
                        {
                            // Authenticated STUN can discover new peer-reflexive candidates;
                            // bound those sources as well as the signaled candidate list.
                            self.receive_sources.insert(source);
                            let _ = self.rtc.handle_input(input);
                        }
                    }
                }
            }
            if !self.events.is_empty() {
                continue;
            }
            let now = Instant::now();
            if now >= deadline {
                return Ok(None);
            }
            std::thread::sleep(
                deadline
                    .min(next)
                    .saturating_duration_since(now)
                    .min(Duration::from_millis(2)),
            );
        }
    }

    pub fn close(&mut self) {
        if self.state == RtcState::Closed && self.sockets.is_empty() {
            return;
        }
        let _ = self.rtc.close();
        if self.open {
            self.events.push_back(RtcEvent::DataChannelClosed);
        }
        self.open = false;
        self.pending = None;
        self.set_state(RtcState::Closed);
        // Flush close_notify before releasing the sockets.
        while let Ok(output) = self.rtc.poll_output() {
            if let Output::Transmit(output) = output {
                if let Some(socket) = self.sockets.iter().find(|s| s.local == output.source) {
                    let _ = socket.socket.send_to(&output.contents, output.destination);
                }
            } else if matches!(output, Output::Timeout(_)) {
                break;
            }
        }
        self.sockets.clear();
        self.bindings.clear();
        self.rtc.disconnect();
    }

    fn description(&mut self, kind: &'static str, sdp: String) -> io::Result<()> {
        let mid = sdp
            .lines()
            .find_map(|line| line.trim().strip_prefix("a=mid:"))
            .ok_or_else(|| invalid("missing datachannel mid"))?
            .to_owned();
        self.mid = Some(mid.clone());
        self.started = Some(Instant::now());
        self.set_state(RtcState::Connecting);
        self.events
            .push_back(RtcEvent::LocalDescription { kind, sdp });
        for candidate in &self.candidates {
            self.events.push_back(RtcEvent::LocalCandidate {
                candidate: candidate.clone(),
                mid: mid.clone(),
            });
        }
        Ok(())
    }

    fn set_state(&mut self, state: RtcState) {
        if state != self.state {
            self.state = state;
            self.events.push_back(RtcEvent::StateChange(state));
        }
    }

    fn handle_event(&mut self, event: Event) {
        if matches!(self.state, RtcState::Closed | RtcState::Failed) {
            return;
        }
        match event {
            Event::Connected => {
                self.established = true;
                self.set_state(RtcState::Connected);
            }
            Event::IceConnectionStateChange(
                IceConnectionState::Connected | IceConnectionState::Completed,
            ) if self.state == RtcState::Disconnected && self.established => {
                self.set_state(RtcState::Connected)
            }
            Event::IceConnectionStateChange(IceConnectionState::Disconnected) => {
                self.set_state(RtcState::Disconnected)
            }
            Event::ChannelOpen(id, _) => {
                self.established = true;
                self.channel = Some(id);
                self.open = true;
                self.set_state(RtcState::Connected);
                self.events.push_back(RtcEvent::DataChannelOpen);
            }
            Event::ChannelClose(id) if self.channel == Some(id) => {
                self.open = false;
                self.events.push_back(RtcEvent::DataChannelClosed);
            }
            Event::ChannelData(data)
                if self.channel == Some(data.id) && data.binary && !data.data.is_empty() =>
            {
                self.events.push_back(RtcEvent::Message(data.data))
            }
            Event::Closed => self.close(),
            _ => {}
        }
    }
}

impl Drop for RtcPeer {
    fn drop(&mut self) {
        self.close();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn peer() -> RtcPeer {
        RtcPeer::new(&RtcPeerConfig {
            ice_servers: vec![IceServerConfig {
                url: "stun:127.0.0.1:9".into(),
                username: String::new(),
                password: String::new(),
            }],
            ..RtcPeerConfig::default()
        })
        .unwrap()
    }

    #[test]
    fn ice_disconnection_recovers_after_the_one_time_dtls_connected_event() {
        let mut peer = peer();
        peer.handle_event(Event::Connected);
        peer.handle_event(Event::IceConnectionStateChange(
            IceConnectionState::Disconnected,
        ));
        assert_eq!(peer.state(), RtcState::Disconnected);
        peer.handle_event(Event::IceConnectionStateChange(
            IceConnectionState::Completed,
        ));
        assert_eq!(peer.state(), RtcState::Connected);
        peer.close();
        peer.handle_event(Event::Connected);
        assert_eq!(peer.state(), RtcState::Closed);
    }

    #[test]
    fn setup_timeout_is_terminal_even_when_ice_has_already_disconnected() {
        let mut peer = peer();
        peer.initiate_offer("audio").unwrap();
        peer.events.clear();
        peer.started = Some(Instant::now() - Duration::from_secs(31));
        peer.state = RtcState::Disconnected;
        peer.poll(Duration::ZERO).unwrap();
        assert_eq!(peer.state(), RtcState::Failed);
        assert_eq!(peer.local_addresses().count(), 0);
        peer.handle_event(Event::Connected);
        assert_eq!(peer.state(), RtcState::Failed);
        assert!(!peer.send(&[1]));
    }

    #[test]
    fn public_stun_xor_mapped_v6_and_malformed_replies() {
        let mut request = [0u8; 20];
        request[..8].copy_from_slice(&[0, 1, 0, 0, 0x21, 0x12, 0xa4, 0x42]);
        request[8..].copy_from_slice(&[1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12]);
        let mut packet = [0u8; 44];
        packet[..20].copy_from_slice(&request);
        packet[..4].copy_from_slice(&[1, 1, 0, 24]);
        packet[20..24].copy_from_slice(&[0, 0x20, 0, 20]);
        packet[24..28].copy_from_slice(&[0, 2, 0x21, 0x12 ^ 7]);
        let ip = "2001:db8::1234".parse::<std::net::Ipv6Addr>().unwrap();
        for (n, byte) in ip.octets().into_iter().enumerate() {
            packet[28 + n] = byte ^ request[4 + n];
        }
        assert_eq!(
            stun_mapped(&packet, &request),
            Some(SocketAddr::new(ip.into(), 7))
        );
        for n in 0..packet.len() {
            assert_eq!(stun_mapped(&packet[..n], &request), None);
        }
        packet[8] ^= 1;
        assert_eq!(stun_mapped(&packet, &request), None);
        packet[8] ^= 1;
        packet[22] = 255;
        assert_eq!(stun_mapped(&packet, &request), None);
    }

    #[test]
    fn authenticated_stun_cannot_grow_peer_reflexive_sources_without_bound() {
        use str0m::ice::StunMessage;
        let mut peer = peer();
        peer.initiate_offer("audio").unwrap();
        let sdp = peer
            .events
            .iter()
            .find_map(|e| {
                if let RtcEvent::LocalDescription { sdp, .. } = e {
                    Some(sdp.clone())
                } else {
                    None
                }
            })
            .unwrap();
        let credential = |prefix| {
            sdp.lines()
                .find_map(|l| l.trim().strip_prefix(prefix))
                .unwrap()
        };
        let username = format!("{}:remote", credential("a=ice-ufrag:"));
        let password = credential("a=ice-pwd:");
        let target = peer
            .local_addresses()
            .find(|a| a.is_ipv4() && a.ip().is_loopback())
            .unwrap();
        let provider = str0m::crypto::from_feature_flags();
        let sockets: Vec<_> = (0..130)
            .map(|_| UdpSocket::bind("127.0.0.1:0").unwrap())
            .collect();
        let mut packet = [0u8; 256];
        let len =
            StunMessage::binding_request(&username, TransId::new(), true, 1, 2122260223, false)
                .to_bytes(Some(password.as_bytes()), &mut packet, |key, data| {
                    provider.sha1_hmac_provider.sha1_hmac(key, data)
                })
                .unwrap();
        for socket in &sockets {
            socket.send_to(&packet[..len], target).unwrap();
            peer.events.clear();
            peer.poll(Duration::from_millis(1)).unwrap();
        }
        assert_eq!(peer.receive_sources.len(), MAX_CANDIDATES);
        for _ in 0..10 {
            sockets[0].send_to(&packet[..len], target).unwrap();
            peer.events.clear();
            peer.poll(Duration::from_millis(1)).unwrap();
        }
        assert_eq!(peer.receive_sources.len(), MAX_CANDIDATES);
    }
}
