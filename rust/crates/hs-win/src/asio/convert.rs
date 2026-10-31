//! ASIO sample types this host converts (little-endian, the Windows ones) and the conversions to and from float.
//! Port of `toFloat` / `fromFloat` in apps/karaoke/HasAsio.cpp: same scale factors, same order of the float
//! arithmetic, so the output matches the C++ bit for bit (`tests/convert.rs` compares against C++ output).
//!
//! Both directions are for the audio thread: no allocation, no panic (every index is a `chunks_exact`, and a
//! buffer that is too short makes the call return `false` before anything is written).

pub const INT16_LSB: i32 = 16;
pub const INT24_LSB: i32 = 17;
pub const INT32_LSB: i32 = 18;
pub const FLOAT32_LSB: i32 = 19;
pub const FLOAT64_LSB: i32 = 20;
pub const INT32_LSB16: i32 = 24;
pub const INT32_LSB18: i32 = 25;
pub const INT32_LSB20: i32 = 26;
pub const INT32_LSB24: i32 = 27;

/// Bytes per sample, 0 = a type this host can't handle (big-endian and DSD types included).
pub fn bytes_per_sample(ty: i32) -> usize {
    match ty {
        INT16_LSB => 2,
        INT24_LSB => 3,
        INT32_LSB | FLOAT32_LSB | INT32_LSB16 | INT32_LSB18 | INT32_LSB20 | INT32_LSB24 => 4,
        FLOAT64_LSB => 8,
        _ => 0,
    }
}

pub fn bit_depth(ty: i32) -> i32 {
    match ty {
        INT16_LSB | INT32_LSB16 => 16,
        INT32_LSB18 => 18,
        INT32_LSB20 => 20,
        INT24_LSB | INT32_LSB24 => 24,
        INT32_LSB | FLOAT32_LSB => 32,
        FLOAT64_LSB => 64,
        _ => 0,
    }
}

/// Full scale of the integer types (the data sits in the low bits of the 32-bit container for 16/18/20/24).
fn full_scale(ty: i32) -> f64 {
    match ty {
        INT16_LSB | INT32_LSB16 => 32768.0,
        INT32_LSB18 => 131072.0,
        INT32_LSB20 => 524288.0,
        INT24_LSB | INT32_LSB24 => 8388608.0,
        INT32_LSB => 2147483648.0,
        _ => 1.0,
    }
}

/// Driver buffer to float: `dst.len()` samples from `src`. `false` = unsupported type or `src` too short (nothing written).
pub fn to_float(ty: i32, src: &[u8], dst: &mut [f32]) -> bool {
    let bytes = bytes_per_sample(ty);
    if bytes == 0 || src.len() < bytes * dst.len() {
        return false;
    }
    let fs = full_scale(ty);
    let k = (1.0 / fs) as f32;
    match ty {
        INT16_LSB => {
            for (d, s) in dst.iter_mut().zip(src.as_chunks::<2>().0) {
                *d = f32::from(i16::from_le_bytes(*s)) * k;
            }
        }
        INT24_LSB => {
            for (d, p) in dst.iter_mut().zip(src.as_chunks::<3>().0) {
                let sign = if p[2] & 0x80 != 0 { 0xff00_0000 } else { 0 };
                let v = (u32::from(p[0]) | (u32::from(p[1]) << 8) | (u32::from(p[2]) << 16) | sign)
                    as i32;
                *d = v as f32 * k;
            }
        }
        INT32_LSB | INT32_LSB16 | INT32_LSB18 | INT32_LSB20 | INT32_LSB24 => {
            for (d, s) in dst.iter_mut().zip(src.as_chunks::<4>().0) {
                *d = (f64::from(i32::from_le_bytes(*s)) / fs) as f32;
            }
        }
        FLOAT32_LSB => {
            for (d, s) in dst.iter_mut().zip(src.as_chunks::<4>().0) {
                *d = f32::from_le_bytes(*s);
            }
        }
        _ => {
            for (d, s) in dst.iter_mut().zip(src.as_chunks::<8>().0) {
                *d = f64::from_le_bytes(*s) as f32;
            }
        }
    }
    true
}

/// Clamp to [-1, 1]. C++ bug fixed: `fmin(fmax(NaN, -1), 1)` is -1, so a NaN from a broken plug-in went to the
/// interface as full-scale negative; here it is silence.
fn clamp_unit(x: f64) -> f64 {
    if x.is_nan() { 0.0 } else { x.clamp(-1.0, 1.0) }
}

/// Clamp, then round (half away from zero, like `llround`) to the integer range of the type.
fn to_int(x: f32, fs: f64) -> i64 {
    (clamp_unit(f64::from(x)) * (fs - 1.0)).round() as i64
}

/// Float to driver buffer: `src.len()` samples into `dst`. Clipped like the integer types: an over never reaches the
/// interface above full scale. `false` = unsupported type or `dst` too short (nothing written).
pub fn from_float(ty: i32, src: &[f32], dst: &mut [u8]) -> bool {
    let bytes = bytes_per_sample(ty);
    if bytes == 0 || dst.len() < bytes * src.len() {
        return false;
    }
    let fs = full_scale(ty);
    match ty {
        INT16_LSB => {
            for (s, d) in src.iter().zip(dst.as_chunks_mut::<2>().0) {
                d.copy_from_slice(&(to_int(*s, fs) as i16).to_le_bytes());
            }
        }
        INT24_LSB => {
            for (s, p) in src.iter().zip(dst.as_chunks_mut::<3>().0) {
                let v = to_int(*s, fs) as i32 as u32;
                p[0] = v as u8;
                p[1] = (v >> 8) as u8;
                p[2] = (v >> 16) as u8;
            }
        }
        INT32_LSB | INT32_LSB16 | INT32_LSB18 | INT32_LSB20 | INT32_LSB24 => {
            for (s, d) in src.iter().zip(dst.as_chunks_mut::<4>().0) {
                d.copy_from_slice(&(to_int(*s, fs) as i32).to_le_bytes());
            }
        }
        FLOAT32_LSB => {
            for (s, d) in src.iter().zip(dst.as_chunks_mut::<4>().0) {
                let v = if s.is_nan() { 0.0 } else { s.clamp(-1.0, 1.0) };
                d.copy_from_slice(&v.to_le_bytes());
            }
        }
        _ => {
            for (s, d) in src.iter().zip(dst.as_chunks_mut::<8>().0) {
                d.copy_from_slice(&clamp_unit(f64::from(*s)).to_le_bytes());
            }
        }
    }
    true
}
