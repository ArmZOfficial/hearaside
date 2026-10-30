//! Shared-memory layout, protocol v14. Mirrors `libs/ssbus/include/ssbus/protocol.h` field for
//! field (same names, same order); the layout test compares every offset, size and alignment
//! with `compat/fixtures/shm-v14-layout.json`. Changing anything here means bumping
//! `kProtocolVersion` on both sides.
#![allow(non_snake_case, non_upper_case_globals, non_camel_case_types)]

use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::mem::{align_of, offset_of, size_of};
use core::sync::atomic::{AtomicI32, AtomicI64, AtomicU32, AtomicU64};

pub const kMagic: u32 = 0x5353_5031; // 'SSP1'
pub const kProtocolVersion: u32 = 14;
pub const kMaxSlots: usize = 64;
pub const kMaxStems: usize = 8;
pub const kNumStreamOuts: usize = 1 + kMaxStems;
pub const kMaxChannels: usize = 2;
pub const kRingFrames: u32 = 65536;
pub const kRingMask: u32 = kRingFrames - 1;
pub const kGuardFrames: u32 = 4096;
pub const kMailboxSize: usize = 32;
pub const kTagRingSize: usize = 512;
pub const kNameBytes: usize = 64;
pub const kUuidBytes: usize = 40;
pub const kRemoteQueueSize: usize = 64;
pub const kMaxSources: usize = 16;
pub const kMaxFriends: usize = 8;
pub const kMaxFeeders: usize = 16;
pub const kRequestQueueSize: usize = 16;
pub const kTapFrames: usize = 8192;
pub const kTapDecimation: usize = 4;
pub const kPairAutomatic: u32 = 0xFFFF_FFFF;
pub const kReplyNotInRoom: u32 = 0xFFFF_0001;
pub const kReplyRoomFull: u32 = 0xFFFF_0002;
pub const kReplyFailed: u32 = 0xFFFF_0003;
pub const kReplyNoMatch: u32 = 0xFFFF_0004;
pub const kBeaconMagic: u32 = 0x4842_5143; // 'HBQC'
pub const kLayoutSize: usize = size_of::<BusLayout>();

/// `(name, value)` for every constant, in the order of the fixture's `constants`.
pub const CONSTANTS: &[(&str, u64)] = &[
    ("kMagic", kMagic as u64),
    ("kProtocolVersion", kProtocolVersion as u64),
    ("kMaxSlots", kMaxSlots as u64),
    ("kMaxStems", kMaxStems as u64),
    ("kNumStreamOuts", kNumStreamOuts as u64),
    ("kMaxChannels", kMaxChannels as u64),
    ("kRingFrames", kRingFrames as u64),
    ("kRingMask", kRingMask as u64),
    ("kGuardFrames", kGuardFrames as u64),
    ("kMailboxSize", kMailboxSize as u64),
    ("kTagRingSize", kTagRingSize as u64),
    ("kNameBytes", kNameBytes as u64),
    ("kUuidBytes", kUuidBytes as u64),
    ("kRemoteQueueSize", kRemoteQueueSize as u64),
    ("kMaxSources", kMaxSources as u64),
    ("kMaxFriends", kMaxFriends as u64),
    ("kMaxFeeders", kMaxFeeders as u64),
    ("kRequestQueueSize", kRequestQueueSize as u64),
    ("kTapFrames", kTapFrames as u64),
    ("kTapDecimation", kTapDecimation as u64),
    ("kPairAutomatic", kPairAutomatic as u64),
    ("kReplyNotInRoom", kReplyNotInRoom as u64),
    ("kReplyRoomFull", kReplyRoomFull as u64),
    ("kReplyFailed", kReplyFailed as u64),
    ("kReplyNoMatch", kReplyNoMatch as u64),
    ("kBeaconMagic", kBeaconMagic as u64),
    ("kLayoutSize", kLayoutSize as u64),
];

/// Defines a module of `u32` values plus `ALL` (name, value) for the layout test.
macro_rules! abi_enum {
    ($name:ident { $($v:ident = $x:expr),* $(,)? }) => {
        pub mod $name {
            $(pub const $v: u32 = $x;)*
            pub const ALL: &[(&str, u32)] = &[$((stringify!($v), $x)),*];
        }
    };
}

abi_enum!(HubFlags { kHubBypassed = 1, kHubPreview = 2, kHubPanic = 4, kHubOffline = 8, kHubLimiter = 16 });
// Remote control target = -1 addresses the Hub itself (paramId is a RemoteParam), 0..63 a Track slot.
abi_enum!(RemoteParam { Panic = 1, Preview = 2, AutoSync = 3, Share = 4 });
abi_enum!(SlotState { kSlotFree = 0, kSlotClaiming = 1, kSlotActive = 2 });
abi_enum!(SlotFlags {
    kFlagMon = 1,
    kFlagStr = 2,
    kFlagSolo = 4,
    kFlagBypassed = 8,
    kFlagOffline = 16,
    kFlagMono = 32,
    kFlagApp = 64,
    kFlagViewersViaHub = 128,
});
abi_enum!(ParamId { Mon = 1, Str = 2, StrGainDb = 3, StrPan = 4, StrDelayMs = 5, MonTrimDb = 6, StrSolo = 7, StemIndex = 8 });
abi_enum!(SlotHubStatus { kHubStatusNone = 0, kHubStatusOk = 1, kHubStatusRateMismatch = 2, kHubStatusAhead = 4, kHubStatusUnderrun = 8 });
abi_enum!(SourceFlags { kSrcOn = 1, kSrcRecording = 2, kSrcFollowRec = 4, kSrcSystem = 8, kSrcDropped = 16 });
abi_enum!(SourceParam { On = 1, LevelDb = 2, Record = 3, FollowRecord = 4, DelayMs = 5 });
abi_enum!(FriendState { kFriendFree = 0, kFriendWaiting = 1, kFriendLive = 2, kFriendOffline = 3 });
abi_enum!(FriendRoute { kRouteDirect = 0, kRouteDaw = 1 });
abi_enum!(FeederStatus { kFeederFlowing = 0, kFeederBypassed = 1, kFeederOffline = 2, kFeederNoFriend = 3 });
abi_enum!(RequestKind {
    kReqResolveToken = 1,
    kReqCreateFriend = 2,
    kReqRelease = 3,
    kReqCopyLink = 4,
    kReqPairWith = 5,
    kReqProbe = 6,
});

/// A field that C++ declares `alignas(64)`: same offset, but Rust can only align a wrapper.
#[repr(C, align(64))]
pub struct A64<T>(pub T);

/// A field other processes write without atomics (names, command slots, audio). Reaching it through a
/// plain `&` would be UB, so it sits in an `UnsafeCell`: `get` gives the raw pointer to read or write
/// with volatile / seqlock protocols. Same size and alignment as `T`.
#[repr(transparent)]
pub struct Shared<T>(UnsafeCell<T>);

// SAFETY: access is only through raw pointers under the bus protocols (single writer + seqlock or
// ring cursors); the type itself hands out no references.
unsafe impl<T: Send> Sync for Shared<T> {}

impl<T> Shared<T> {
    pub const fn get(&self) -> *mut T {
        self.0.get()
    }
}

/// Size of the value inside an `A64<T>`, or of the type itself (autoref specialization, used by the layout test).
pub struct Probe<T>(pub PhantomData<T>);
pub trait InnerOfA64 {
    fn inner(&self) -> usize;
}
impl<T> InnerOfA64 for Probe<A64<T>> {
    fn inner(&self) -> usize {
        size_of::<T>()
    }
}
pub trait InnerOfAny {
    fn inner(&self) -> usize;
}
impl<T> InnerOfAny for &Probe<T> {
    fn inner(&self) -> usize {
        size_of::<T>()
    }
}

/// Defines a `repr(C)` struct and `fields()`: `(name, offset, size, align, inner size)` per field,
/// where `inner size` looks through `A64`.
macro_rules! abi_struct {
    ($(#[$m:meta])* $name:ident { $($f:ident : $t:ty),* $(,)? }) => {
        $(#[$m])*
        pub struct $name { $(pub $f: $t),* }
        impl $name {
            pub fn fields() -> Vec<(&'static str, usize, usize, usize, usize)> {
                vec![$((
                    stringify!($f),
                    offset_of!($name, $f),
                    size_of::<$t>(),
                    align_of::<$t>(),
                    (&Probe::<$t>(PhantomData)).inner(),
                )),*]
            }
        }
    };
}

type Chars<const N: usize> = Shared<[u8; N]>;

abi_struct!(
    /// Multi-writer entry: claim an index with fetch_add on `remoteReserve`, fill, store `seq = index + 1`.
    #[repr(C)]
    RemoteCommand { seq: AtomicU32, target: AtomicI32, paramId: AtomicU32, valueBits: AtomicU32 }
);

abi_struct!(
    #[repr(C, align(64))]
    BusHeader {
        magic: AtomicU32, // written last during init (release)
        version: u32,
        layoutSize: u32,
        reserved0: u32,
        hubOwnerToken: AtomicU64,
        hubOwnerPid: AtomicU32,
        hubSampleRate: AtomicU32,
        hubHeartbeatNs: AtomicU64,
        hubFlags: AtomicU32,
        hubBlockSize: AtomicU32,
        hubLatencyFrames: AtomicU32,
        slotSequence: AtomicU32,
        consumerHeartbeatNs: AtomicU64,
        consumerCount: AtomicU32,
        consumerBufferMs: AtomicU32,
        remoteReserve: AtomicU32,
        reserved1: u32,
        remote: [RemoteCommand; kRemoteQueueSize],
        pad: Chars<{ 4096 - 72 - 8 - kRemoteQueueSize * 16 }>,
    }
);

abi_struct!(
    #[derive(Clone, Copy)]
    #[repr(C)]
    Command {
        paramId: u32,
        value: f32
    }
);

abi_struct!(
    /// Seqlock entry: `seq` is block counter + 1 when complete, 0 while writing.
    #[repr(C)]
    TimelineTag {
        seq: AtomicU64,
        fifoPos: AtomicU64,
        timeSamples: AtomicI64,
        numFrames: AtomicU32,
        playing: AtomicU32,
    }
);

abi_struct!(
    #[repr(C, align(64))]
    SlotHeader {
        state: AtomicU32,
        epoch: AtomicU32,
        ownerPid: AtomicU32,
        sequence: AtomicU32,
        nameSeq: AtomicU32,
        colorARGB: Shared<u32>,
        uuid: Chars<kUuidBytes>,
        name: Chars<kNameBytes>,
        sampleRate: AtomicU32,
        numChannels: AtomicU32,
        heartbeatNs: AtomicU64,
        claimNs: AtomicU64,
        flags: AtomicU32,
        strGainBits: AtomicU32,
        strPanBits: AtomicU32,
        strDelayBits: AtomicU32,
        monTrimBits: AtomicU32,
        stemIndex: AtomicI32,
        cmdWrite: AtomicU32,
        cmds: [Shared<Command>; kMailboxSize],
        renameSeq: AtomicU32,
        renameTo: Chars<kNameBytes>,
        peakInBits: [AtomicU32; kMaxChannels],
        peakStreamBits: [AtomicU32; kMaxChannels],
        hubStatus: AtomicU32,
        hubLeadFrames: AtomicU32,
        hubMute: AtomicU32,
        chainLatencyBits: AtomicU32,
        fedBy: AtomicU32,
        fxLatencyBits: AtomicU32,
        tagWrite: AtomicU64,
        // C++ has `char pad[48]` here; the 64-byte wrapper already covers it (see `PAD_IN_WRAPPER`).
        writePos: A64<AtomicU64>,
    }
);

abi_struct!(
    #[repr(C)]
    SlotTags {
        tags: [TimelineTag; kTagRingSize]
    }
);

abi_struct!(
    #[repr(C, align(64))]
    StreamOutHeader {
        nameSeq: AtomicU32,
        sampleRate: AtomicU32,
        active: AtomicU32,
        reserved: u32,
        names: [Chars<kNameBytes>; kNumStreamOuts],
        peakBits: [[AtomicU32; kMaxChannels]; kNumStreamOuts],
        loudnessMBits: AtomicU32,
        loudnessSBits: AtomicU32,
        writePos: A64<[AtomicU64; kNumStreamOuts]>,
    }
);

abi_struct!(
    #[repr(C, align(64))]
    SourceHeader {
        state: AtomicU32,
        ownerPid: AtomicU32,
        sequence: AtomicU32,
        nameSeq: AtomicU32,
        name: Chars<kNameBytes>,
        app: Chars<kNameBytes>,
        colorARGB: Shared<u32>,
        flags: AtomicU32,
        heartbeatNs: AtomicU64,
        capture: AtomicU32,
        levelBits: AtomicU32,
        peakBits: AtomicU32,
        latencyBits: AtomicU32,
        recordMs: AtomicU32,
        delayBits: AtomicU32,
        hubMute: AtomicU32,
        trackSlot: AtomicI32,
        cmdWrite: AtomicU32,
        cmds: [Shared<Command>; kMailboxSize],
        appSeq: AtomicU32,
        appTo: Chars<kNameBytes>,
        writePos: A64<AtomicU64>,
    }
);

abi_struct!(
    #[repr(C, align(64))]
    LinkInHeader {
        sampleRate: AtomicU32,
        active: AtomicU32,
        heartbeatNs: AtomicU64,
        writePos: A64<AtomicU64>,
    }
);

abi_struct!(
    #[repr(C, align(64))]
    FriendHeader {
        state: AtomicU32,
        id: AtomicU32,
        nameSeq: AtomicU32,
        name: Chars<kNameBytes>,
        sampleRate: AtomicU32,
        heartbeatNs: AtomicU64,
        delayBits: AtomicU32,
        route: AtomicU32,
        feeder: AtomicI32,
        outSlot: AtomicI32,
        peakBits: AtomicU32,
        writePos: A64<AtomicU64>,
    }
);

abi_struct!(
    #[repr(C, align(64))]
    FeederRecord {
        state: AtomicU32,
        ownerPid: AtomicU32,
        friendId: AtomicU32,
        nameSeq: AtomicU32,
        trackName: Chars<kNameBytes>,
        colorARGB: Shared<u32>,
        heartbeatNs: AtomicU64,
        blockCount: AtomicU64,
        status: AtomicU32,
        reply: AtomicU32,
        replySeq: AtomicU32,
        hubCommand: AtomicU32,
        hubCommandSeq: AtomicU32,
        sampleRate: AtomicU32,
        tapWrite: A64<AtomicU64>,
    }
);

abi_struct!(
    /// Multi-writer queue like `RemoteCommand`; the Hub wipes `text` once read.
    #[repr(C)]
    PluginRequest {
        seq: AtomicU32,
        feeder: AtomicI32,
        kind: AtomicU32,
        arg: AtomicU32,
        text: Chars<kNameBytes>,
    }
);

abi_struct!(
    #[repr(C)]
    FeederTap {
        mono: Shared<[f32; kTapFrames]>
    }
);

abi_struct!(
    /// Fixed forever: a separate 64-byte segment with no version in its name.
    #[repr(C, align(64))]
    BeaconHeader {
        magic: AtomicU32,
        protocolVersion: AtomicU32,
        hubPid: AtomicU32,
        heartbeatNs: AtomicU64,
    }
);

abi_struct!(
    #[repr(C)]
    ChannelRing {
        ch: Shared<[[f32; kRingFrames as usize]; kMaxChannels]>
    }
);

abi_struct!(
    /// The whole segment (~53 MiB): only ever reached through a mapping, never constructed.
    #[repr(C)]
    BusLayout {
        header: BusHeader,
        slots: [SlotHeader; kMaxSlots],
        slotTags: [SlotTags; kMaxSlots],
        streamHeader: StreamOutHeader,
        sources: [SourceHeader; kMaxSources],
        slotAudio: A64<[ChannelRing; kMaxSlots]>,
        sourceAudio: A64<[ChannelRing; kMaxSources]>,
        linkIn: LinkInHeader,
        linkInAudio: A64<ChannelRing>,
        streamAudio: A64<[ChannelRing; kNumStreamOuts]>,
        friends: A64<[FriendHeader; kMaxFriends]>,
        feeders: [FeederRecord; kMaxFeeders],
        requests: [PluginRequest; kRequestQueueSize],
        requestReserve: A64<AtomicU32>,
        feederTap: A64<[FeederTap; kMaxFeeders]>,
        friendAudio: A64<[ChannelRing; kMaxFriends]>,
        friendMixWrite: A64<AtomicU64>,
        friendMixAudio: A64<ChannelRing>,
        friendVoiceWrite: A64<[AtomicU64; kMaxFriends / 2]>,
        friendVoiceAudio: A64<[ChannelRing; kMaxFriends / 2]>,
    }
);

const _: () = {
    assert!(size_of::<RemoteCommand>() == 16);
    assert!(size_of::<BusHeader>() == 4096);
    assert!(size_of::<TimelineTag>() == 32);
    assert!(size_of::<BeaconHeader>() == 64);
    assert!((kRingFrames & kRingMask) == 0);
};

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;

    fn fixture() -> Value {
        serde_json::from_str(include_str!(
            "../../../../compat/fixtures/shm-v14-layout.json"
        ))
        .unwrap()
    }

    /// Structs whose trailing C++ `pad` is already inside the last `A64` field.
    const PAD_IN_WRAPPER: &[&str] = &["SlotHeader"];

    fn check(
        fx: &Value,
        name: &str,
        size: usize,
        align: usize,
        fields: &[(&str, usize, usize, usize, usize)],
    ) {
        let s = &fx["structs"][name];
        assert!(s.is_object(), "{name} missing from fixture");
        assert_eq!(s["size"], size, "{name} size");
        assert_eq!(s["alignment"], align, "{name} alignment");
        let want = s["fields"].as_object().unwrap();
        let absorbed = usize::from(PAD_IN_WRAPPER.contains(&name));
        assert_eq!(want.len(), fields.len() + absorbed, "{name} field count");
        for &(f, off, sz, al, inner) in fields {
            let w = &want[f];
            assert_eq!(w["offset"], off, "{name}.{f} offset");
            // The value itself (looking through A64) must have exactly the C++ size.
            assert_eq!(w["size"], inner, "{name}.{f} size");
            // alignas(64) members: Rust's wrapper is 64-aligned, the C++ member keeps its own alignment.
            if al != 64 || w["alignment"] == 64 {
                assert_eq!(w["size"], sz, "{name}.{f} wrapper size");
                assert_eq!(w["alignment"], al, "{name}.{f} alignment");
            }
        }
    }

    macro_rules! check_all {
        ($fx:expr, $($t:ident),* $(,)?) => {
            $(check($fx, stringify!($t), size_of::<$t>(), align_of::<$t>(), &$t::fields());)*
        };
    }

    #[test]
    fn structs_match_v14_fixture() {
        let fx = fixture();
        check_all!(
            &fx,
            RemoteCommand,
            BusHeader,
            Command,
            TimelineTag,
            SlotHeader,
            SlotTags,
            StreamOutHeader,
            SourceHeader,
            LinkInHeader,
            FriendHeader,
            FeederRecord,
            PluginRequest,
            FeederTap,
            BeaconHeader,
            ChannelRing,
            BusLayout,
        );
        assert_eq!(fx["structs"].as_object().unwrap().len(), 16);
    }

    #[test]
    fn constants_match_v14_fixture() {
        let want = fixture()["constants"].as_object().unwrap().clone();
        assert_eq!(want.len(), CONSTANTS.len());
        for &(n, v) in CONSTANTS {
            assert_eq!(want[n].as_u64(), Some(v), "{n}");
        }
    }

    #[test]
    fn enums_match_v14_fixture() {
        let fx = fixture();
        let groups: &[(&str, &[(&str, u32)])] = &[
            ("HubFlags", HubFlags::ALL),
            ("RemoteParam", RemoteParam::ALL),
            ("SlotState", SlotState::ALL),
            ("SlotFlags", SlotFlags::ALL),
            ("ParamId", ParamId::ALL),
            ("SlotHubStatus", SlotHubStatus::ALL),
            ("SourceFlags", SourceFlags::ALL),
            ("SourceParam", SourceParam::ALL),
            ("FriendState", FriendState::ALL),
            ("FriendRoute", FriendRoute::ALL),
            ("FeederStatus", FeederStatus::ALL),
            ("RequestKind", RequestKind::ALL),
        ];
        let all = fx["enums"].as_object().unwrap();
        assert_eq!(all.len(), groups.len());
        for &(g, vals) in groups {
            let want = all[g].as_object().unwrap();
            assert_eq!(want.len(), vals.len(), "{g}");
            for &(n, v) in vals {
                assert_eq!(want[n].as_u64(), Some(u64::from(v)), "{g}::{n}");
            }
        }
    }
}
