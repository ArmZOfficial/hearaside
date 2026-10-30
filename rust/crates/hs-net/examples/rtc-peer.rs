//! JSON-lines adapter used by `tests/web-friend/rtc.test.mjs`; no HTTP/WebSocket server here.
use hs_net::rtc::{HUB_UDP_FIRST, HUB_UDP_LAST, RtcEvent, RtcPeer, RtcPeerConfig};
use serde_json::{Value, json};
use std::io::{self, BufRead, Write};
use std::sync::mpsc;
use std::time::Duration;

fn emit(value: Value) {
    println!("{value}");
    let _ = io::stdout().flush();
}

fn main() -> io::Result<()> {
    let mut peer = RtcPeer::new(&RtcPeerConfig {
        port_range_begin: HUB_UDP_FIRST,
        port_range_end: HUB_UDP_LAST,
        ..RtcPeerConfig::default()
    })?;
    emit(json!({"type":"ready", "port":peer.local_addresses().next().map(|a|a.port())}));
    let (tx, rx) = mpsc::sync_channel(64);
    std::thread::spawn(move || {
        for line in io::stdin().lock().lines() {
            let Ok(line) = line else {
                break;
            };
            if tx.send(line).is_err() {
                break;
            }
        }
    });
    loop {
        match rx.try_recv() {
            Ok(line) => {
                let result = (|| -> Result<bool, Box<dyn std::error::Error>> {
                    let value: Value = serde_json::from_str(&line)?;
                    match value["type"].as_str().unwrap_or("") {
                        "offer" => {
                            peer.initiate_offer(value["label"].as_str().unwrap_or("audio"))?
                        }
                        "rtc_answer" | "rtc_offer" => peer.set_remote_description(
                            if value["type"] == "rtc_answer" {
                                "answer"
                            } else {
                                "offer"
                            },
                            value["sdp"].as_str().ok_or("missing SDP")?,
                        )?,
                        "rtc_candidate" => peer.add_remote_candidate(
                            value["candidate"].as_str().ok_or("missing candidate")?,
                            value["mid"].as_str().unwrap_or(""),
                        )?,
                        "send" => {
                            let data: Vec<u8> = serde_json::from_value(value["data"].clone())?;
                            if !peer.send(&data) {
                                return Err("send refused".into());
                            }
                        }
                        "close" => {
                            peer.close();
                            emit(json!({"type":"closed"}));
                            return Ok(true);
                        }
                        _ => return Err("unknown command".into()),
                    }
                    Ok(false)
                })();
                match result {
                    Ok(true) => return Ok(()),
                    Ok(false) => {}
                    Err(error) => emit(json!({"type":"error", "message":error.to_string()})),
                }
            }
            Err(mpsc::TryRecvError::Disconnected) => return Ok(()),
            Err(mpsc::TryRecvError::Empty) => {}
        }
        if let Some(event) = peer.poll(Duration::from_millis(2))? {
            match event {
                RtcEvent::LocalDescription { kind, sdp } => {
                    emit(json!({"type":format!("rtc_{kind}"), "sdp":sdp}))
                }
                RtcEvent::LocalCandidate { candidate, mid } => {
                    emit(json!({"type":"rtc_candidate", "candidate":candidate,"mid":mid}))
                }
                RtcEvent::DataChannelOpen => emit(json!({"type":"open"})),
                RtcEvent::DataChannelClosed => emit(json!({"type":"closed"})),
                RtcEvent::Message(data) => emit(json!({"type":"message","data":data})),
                RtcEvent::StateChange(state) => {
                    emit(json!({"type":"state", "state":format!("{state:?}")}))
                }
            }
        }
    }
}
