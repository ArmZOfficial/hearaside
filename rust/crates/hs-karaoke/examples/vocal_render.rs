//! A/B listening (R5.3): the Rust twin of `hs-vocal-golden --render`. Runs a recording's first channel through
//! `VocalChain` with the app's default settings and writes the headphone mix as 32-bit float stereo WAV.
//!
//! `cargo run --release -p hs-karaoke --example vocal_render -- <in.wav|flac|mp3> <out.wav>`
use hs_karaoke::vocal::VocalChain;
use std::fs::File;
use symphonia::core::audio::SampleBuffer;
use symphonia::core::codecs::DecoderOptions;
use symphonia::core::io::MediaSourceStream;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("Usage: vocal_render <in> <out.wav>");
        std::process::exit(2);
    }
    let (mic, rate) = read_first_channel(&args[1]);

    const BLOCK: usize = 256; // the C++ tool's block size
    let mut chain = VocalChain::new();
    chain.prepare(f64::from(rate), BLOCK);
    let (mut l, mut r) = (vec![0.0f32; mic.len()], vec![0.0f32; mic.len()]);
    let mut net = [0.0f32; BLOCK];
    for ((m, a), b) in mic
        .chunks(BLOCK)
        .zip(l.chunks_mut(BLOCK))
        .zip(r.chunks_mut(BLOCK))
    {
        let n = m.len();
        chain.process(Some(m), Some(a), Some(b), Some(&mut net[..n]), n);
    }

    let mut wav = Vec::with_capacity(44 + mic.len() * 8);
    let data = (mic.len() * 8) as u32;
    wav.extend(b"RIFF");
    wav.extend((36 + data).to_le_bytes());
    wav.extend(b"WAVEfmt ");
    wav.extend(16u32.to_le_bytes());
    wav.extend(3u16.to_le_bytes()); // IEEE float
    wav.extend(2u16.to_le_bytes());
    wav.extend(rate.to_le_bytes());
    wav.extend((rate * 8).to_le_bytes());
    wav.extend(8u16.to_le_bytes());
    wav.extend(32u16.to_le_bytes());
    wav.extend(b"data");
    wav.extend(data.to_le_bytes());
    for (a, b) in l.iter().zip(&r) {
        wav.extend(a.to_le_bytes());
        wav.extend(b.to_le_bytes());
    }
    std::fs::write(&args[2], wav).expect("write the output");
    println!("Rendered {} frames at {rate} Hz", mic.len());
}

fn read_first_channel(path: &str) -> (Vec<f32>, u32) {
    let mss = MediaSourceStream::new(
        Box::new(File::open(path).expect("open the input")),
        Default::default(),
    );
    let mut format = symphonia::default::get_probe()
        .format(
            &Default::default(),
            mss,
            &Default::default(),
            &Default::default(),
        )
        .expect("a readable audio file")
        .format;
    let track = format.default_track().expect("an audio track");
    let (id, rate) = (
        track.id,
        track.codec_params.sample_rate.expect("a sample rate"),
    );
    let mut decoder = symphonia::default::get_codecs()
        .make(&track.codec_params, &DecoderOptions::default())
        .expect("a supported codec");
    let mut mic = Vec::new();
    while let Ok(packet) = format.next_packet() {
        if packet.track_id() != id {
            continue;
        }
        let Ok(decoded) = decoder.decode(&packet) else {
            continue;
        };
        let spec = *decoded.spec();
        let mut buf = SampleBuffer::<f32>::new(decoded.capacity() as u64, spec);
        buf.copy_interleaved_ref(decoded);
        mic.extend(buf.samples().iter().step_by(spec.channels.count().max(1)));
    }
    (mic, rate)
}
