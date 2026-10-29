fn main() {
    println!("{} {}", env!("CARGO_PKG_NAME"), env!("CARGO_PKG_VERSION"));
}

#[cfg(test)]
mod tests {
    use std::mem::{align_of, size_of};
    use std::sync::atomic::{AtomicI32, AtomicI64, AtomicU32, AtomicU64};

    #[test]
    fn shared_memory_primitive_layout() {
        const { assert!(cfg!(target_endian = "little")) };
        assert_eq!(size_of::<usize>(), 8);
        assert_eq!(size_of::<f32>(), 4);
        assert_eq!(size_of::<AtomicU32>(), 4);
        assert_eq!(align_of::<AtomicU32>(), 4);
        assert_eq!(size_of::<AtomicI32>(), 4);
        assert_eq!(align_of::<AtomicI32>(), 4);
        assert_eq!(size_of::<AtomicU64>(), 8);
        assert_eq!(align_of::<AtomicU64>(), 8);
        assert_eq!(size_of::<AtomicI64>(), 8);
        assert_eq!(align_of::<AtomicI64>(), 8);
    }
}
