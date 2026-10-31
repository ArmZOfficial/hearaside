use std::path::{Path, PathBuf};

/// A 16-bit stereo WAV of `frames` frames of `f(i)` on both channels.
pub fn write_wav(path: &Path, rate: u32, frames: usize, f: impl Fn(usize) -> f32) {
    let data_len = (frames * 4) as u32;
    let mut b = Vec::with_capacity(44 + data_len as usize);
    b.extend(b"RIFF");
    b.extend((36 + data_len).to_le_bytes());
    b.extend(b"WAVEfmt ");
    b.extend(16u32.to_le_bytes());
    b.extend(1u16.to_le_bytes()); // PCM
    b.extend(2u16.to_le_bytes());
    b.extend(rate.to_le_bytes());
    b.extend((rate * 4).to_le_bytes());
    b.extend(4u16.to_le_bytes());
    b.extend(16u16.to_le_bytes());
    b.extend(b"data");
    b.extend(data_len.to_le_bytes());
    for i in 0..frames {
        let s = (f(i).clamp(-1.0, 1.0) * 32767.0) as i16;
        b.extend(s.to_le_bytes());
        b.extend(s.to_le_bytes());
    }
    std::fs::write(path, b).unwrap();
}

/// A file in the temp folder, deleted on drop.
pub struct Temp(pub PathBuf);

impl Temp {
    pub fn new(name: &str) -> Self {
        Self(std::env::temp_dir().join(format!("hs-karaoke-{}-{name}", std::process::id())))
    }
}

impl Drop for Temp {
    fn drop(&mut self) {
        let _ = std::fs::remove_file(&self.0);
    }
}
