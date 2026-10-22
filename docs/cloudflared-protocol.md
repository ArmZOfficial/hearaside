# HEARASIDE: Cloudflare Tunnel Protocol Audit (QUIC vs HTTP/2)

> Documentation for Phase 7.5 optimization · Hearaside v2 Realtime Karaoke

---

## 1. Context & Architecture

HEARASIDE Hub runs an embedded HTTP/WebSocket server (`ShareServer`) and can launch an automated `cloudflared` quick tunnel (`cloudflared tunnel --no-autoupdate --url http://127.0.0.1:<port>`) to provide instant public URLs (`https://<hash>.trycloudflare.com`).

Traffic traversing the tunnel includes:
1. **Web assets**: Serving `send.html` and `listen.html` to browsers.
2. **WebRTC signaling**: Initial SDP offer/answer exchange and ICE candidates.
3. **Fallback audio**: WebSocket audio transport (HRA3 Opus or HRA2 PCM16) when WebRTC peer-to-peer and TURN are unavailable.

*Note: In Phase 4, primary real-time audio is transported directly over UDP via WebRTC DataChannel (or Cloudflare Realtime TURN), bypassing cloudflared entirely for voice traffic.*

---

## 2. Protocol Comparison: QUIC vs HTTP/2

| Metric | `--protocol quic` (UDP 7844) | `--protocol http2` (TCP 443) | `--protocol auto` (Recommended) |
|---|---|---|---|
| **Transport** | UDP (QUIC / HTTP/3) | TCP + TLS 1.3 | QUIC with HTTP/2 fallback |
| **Edge Port** | UDP 7844 | TCP 443 | UDP 7844 → TCP 443 |
| **Handshake Latency** | ~0-RTT / 1-RTT (~20–35 ms in TH) | 1-RTT / 2-RTT (~45–70 ms in TH) | Fast when UDP open |
| **Firewall / ISP Compatibility** | Variable: some Thai mobile carriers (e.g. strict CGNAT) or corporate Wi-Fi block/throttle UDP 7844 | 100%: Standard HTTPS port 443 is universally permitted | 100%: Automatic failover |
| **Packet Loss Resilience** | Head-of-line blocking eliminated for independent streams | TCP head-of-line blocking if packet loss occurs | Best of both |

---

## 3. Findings & Recommendation

1. **Thailand & SE Asia Network Conditions**:
   - On fixed fiber broadband (AIS Fibre, TrueOnline, 3BB), UDP port 7844 to Cloudflare Singapore/BKK edge nodes operates with low latency (~15–25 ms).
   - On certain mobile 4G/5G networks with aggressive Carrier-Grade NAT (CGNAT) or public/campus networks, UDP port 7844 packets are occasionally dropped or rate-limited.
2. **Recommendation**:
   - Use Cloudflare's default `--protocol auto` (or let cloudflared negotiate).
   - Do not force `--protocol quic` exclusively, as it can cause complete tunnel startup failures on restricted networks where UDP 7844 is blocked.
   - For real-time voice, rely on WebRTC DataChannel (Phase 4) with Cloudflare TURN for direct low-latency UDP delivery.
