//! Runs a `ShareServer` with a fake cloudflared until it is killed (used by `tests/share.rs` to check that a crashed
//! host takes its tunnel process with it). Usage: `share-host <bus name> <folder with cloudflared>`; prints `ready`.
use hs_bus::shm::SharedMemory;
use hs_net::share::{CloudflaredSource, ShareServer};
use std::sync::Arc;

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let bus = Arc::new(SharedMemory::open(&args[0]).expect("bus"));
    let server = ShareServer::new();
    server.set_cloudflared_source(CloudflaredSource::Dirs(vec![args[1].clone().into()]));
    assert!(server.start(bus, "listen-token-listen-token", "send-token-send-token-xx"));
    println!("ready");
    loop {
        std::thread::sleep(std::time::Duration::from_secs(1));
    }
}
