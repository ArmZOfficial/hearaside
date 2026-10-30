//! One WebRTC peer of a listener or friend session, owned and polled by its own thread (the `RtcPeer` is single owner).
//! The session talks to it through a small bounded channel; what the peer produces comes back through [`RtcSink`].
//! The peer is created here, not on the session's thread: its constructor does DNS and socket work.
use crate::rtc::{
    HUB_UDP_FIRST, HUB_UDP_LAST, IceServerConfig, RtcEvent, RtcPeer, RtcPeerConfig, RtcState,
};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{self, SyncSender};
use std::thread::JoinHandle;
use std::time::Duration;

/// What an RTC peer hands to its session.
pub(crate) trait RtcSink: Send + Sync + 'static {
    /// A signaling message for the page (`rtc_offer` / `rtc_candidate` JSON text).
    fn signal(&self, json: String);
    /// A binary DataChannel message from the page.
    fn message(&self, data: &[u8]);
    /// The session is over: the peer stops.
    fn finished(&self) -> bool;
}

enum Cmd {
    Answer(String),
    Candidate(String, String),
    Send(Vec<u8>),
}

/// The session's end of the peer. Cheap to share: everything is behind an `Arc`.
pub(crate) struct RtcHandle {
    tx: SyncSender<Cmd>,
    open: Arc<AtomicBool>,
}

impl RtcHandle {
    pub(crate) fn is_open(&self) -> bool {
        self.open.load(Ordering::Acquire)
    }
    pub(crate) fn answer(&self, sdp: String) {
        let _ = self.tx.try_send(Cmd::Answer(sdp));
    }
    pub(crate) fn candidate(&self, candidate: String, mid: String) {
        let _ = self.tx.try_send(Cmd::Candidate(candidate, mid));
    }
    /// Unordered, unreliable like the DataChannel itself: a full command queue drops the packet.
    pub(crate) fn send(&self, data: &[u8]) -> bool {
        matches!(self.tx.try_send(Cmd::Send(data.to_vec())), Ok(()))
    }
}

pub(crate) struct RtcWorker {
    handle: Arc<RtcHandle>,
    stop: Arc<AtomicBool>,
    join: Option<JoinHandle<()>>,
}

/// Counts a running worker thread, also when it panics.
struct Live(Arc<AtomicUsize>);

impl Drop for Live {
    fn drop(&mut self) {
        self.0.fetch_sub(1, Ordering::SeqCst);
    }
}

/// STUN servers the Hub's peers use unless the caller sets others.
pub(crate) fn default_ice() -> Vec<IceServerConfig> {
    [
        "stun:stun.cloudflare.com:3478",
        "stun:stun.l.google.com:19302",
    ]
    .into_iter()
    .map(|url| IceServerConfig {
        url: url.into(),
        username: String::new(),
        password: String::new(),
    })
    .collect()
}

impl RtcWorker {
    /// Starts the thread; it creates the peer, offers a DataChannel `audio` and runs until the session ends.
    /// `None` when the thread cannot start; a peer that cannot be created leaves the WebSocket as the only path.
    /// `live` counts the running worker threads of the server. The ICE servers must be IP addresses (the peer then does
    /// no DNS, so the thread never blocks on the network): see `Inner::ice_servers`.
    pub(crate) fn start(
        sink: Arc<dyn RtcSink>,
        ice: Vec<IceServerConfig>,
        live: Arc<AtomicUsize>,
    ) -> Option<Self> {
        let (tx, rx) = mpsc::sync_channel::<Cmd>(256);
        let open = Arc::new(AtomicBool::new(false));
        let stop = Arc::new(AtomicBool::new(false));
        let handle = Arc::new(RtcHandle {
            tx,
            open: open.clone(),
        });
        let t_stop = stop.clone();
        live.fetch_add(1, Ordering::SeqCst);
        let counted = Live(live);
        let join = std::thread::Builder::new()
            .name("share-rtc".into())
            .spawn(move || {
                let _live = counted;
                run(&*sink, &rx, &open, &t_stop, ice);
                open.store(false, Ordering::Release);
            })
            .ok()?;
        Some(RtcWorker {
            handle,
            stop,
            join: Some(join),
        })
    }

    pub(crate) fn handle(&self) -> Arc<RtcHandle> {
        self.handle.clone()
    }

    /// Ends the peer and joins its thread, always: the thread holds the session (and with it the bus mapping), so it
    /// must be gone before the session returns and `stop()` returns. It ends within a few milliseconds (it never waits
    /// on the network).
    pub(crate) fn shutdown(&mut self) {
        self.stop.store(true, Ordering::Release);
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

impl Drop for RtcWorker {
    fn drop(&mut self) {
        self.shutdown();
    }
}

fn over(sink: &dyn RtcSink, stop: &AtomicBool) -> bool {
    stop.load(Ordering::Acquire) || sink.finished()
}

fn run(
    sink: &dyn RtcSink,
    rx: &mpsc::Receiver<Cmd>,
    open: &AtomicBool,
    stop: &AtomicBool,
    ice: Vec<IceServerConfig>,
) {
    let config = RtcPeerConfig {
        ice_servers: ice,
        port_range_begin: HUB_UDP_FIRST,
        port_range_end: HUB_UDP_LAST,
    };
    let Ok(mut peer) = RtcPeer::new(&config) else {
        return;
    };
    if over(sink, stop) || peer.initiate_offer("audio").is_err() {
        peer.close();
        return;
    }
    loop {
        // commands first, so an answer is applied before the next poll
        while let Ok(cmd) = rx.try_recv() {
            match cmd {
                Cmd::Answer(sdp) => {
                    let _ = peer.set_remote_description("answer", &sdp);
                }
                Cmd::Candidate(c, mid) => {
                    let _ = peer.add_remote_candidate(&c, &mid);
                }
                Cmd::Send(data) => {
                    peer.send(&data);
                }
            }
        }
        if over(sink, stop) || matches!(peer.state(), RtcState::Closed | RtcState::Failed) {
            break;
        }
        match peer.poll(Duration::from_millis(1)) {
            Ok(Some(event)) => {
                if over(sink, stop) {
                    break;
                }
                match event {
                    RtcEvent::LocalDescription { kind: "offer", sdp } => {
                        sink.signal(super::ws::rtc_offer_json(&sdp));
                    }
                    RtcEvent::LocalCandidate { candidate, mid } => {
                        sink.signal(super::ws::rtc_candidate_json(&candidate, &mid));
                    }
                    RtcEvent::Message(data) => sink.message(&data),
                    RtcEvent::DataChannelOpen => open.store(true, Ordering::Release),
                    RtcEvent::DataChannelClosed => open.store(false, Ordering::Release),
                    RtcEvent::StateChange(RtcState::Failed | RtcState::Closed) => {
                        open.store(false, Ordering::Release);
                    }
                    _ => {}
                }
            }
            Ok(None) => {}
            Err(_) => break,
        }
    }
    open.store(false, Ordering::Release);
    peer.close();
}
