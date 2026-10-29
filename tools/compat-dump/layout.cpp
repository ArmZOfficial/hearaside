// SPDX-License-Identifier: MIT
#include <ssbus/protocol.h>

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

namespace {
using namespace ssbus;

std::string layoutJson()
{
    std::ostringstream out;
    out << "{\n  \"schemaVersion\": 1,\n  \"constants\": {\n";
#define VALUE(name, value) out << "    \"" name "\": " << static_cast<uint64_t>(value)
    VALUE("kMagic", kMagic); out << ",\n";
    VALUE("kProtocolVersion", kProtocolVersion); out << ",\n";
    VALUE("kMaxSlots", kMaxSlots); out << ",\n";
    VALUE("kMaxStems", kMaxStems); out << ",\n";
    VALUE("kNumStreamOuts", kNumStreamOuts); out << ",\n";
    VALUE("kMaxChannels", kMaxChannels); out << ",\n";
    VALUE("kRingFrames", kRingFrames); out << ",\n";
    VALUE("kRingMask", kRingMask); out << ",\n";
    VALUE("kGuardFrames", kGuardFrames); out << ",\n";
    VALUE("kMailboxSize", kMailboxSize); out << ",\n";
    VALUE("kTagRingSize", kTagRingSize); out << ",\n";
    VALUE("kNameBytes", kNameBytes); out << ",\n";
    VALUE("kUuidBytes", kUuidBytes); out << ",\n";
    VALUE("kRemoteQueueSize", kRemoteQueueSize); out << ",\n";
    VALUE("kMaxSources", kMaxSources); out << ",\n";
    VALUE("kMaxFriends", kMaxFriends); out << ",\n";
    VALUE("kMaxFeeders", kMaxFeeders); out << ",\n";
    VALUE("kRequestQueueSize", kRequestQueueSize); out << ",\n";
    VALUE("kTapFrames", kTapFrames); out << ",\n";
    VALUE("kTapDecimation", kTapDecimation); out << ",\n";
    VALUE("kPairAutomatic", kPairAutomatic); out << ",\n";
    VALUE("kReplyNotInRoom", kReplyNotInRoom); out << ",\n";
    VALUE("kReplyRoomFull", kReplyRoomFull); out << ",\n";
    VALUE("kReplyFailed", kReplyFailed); out << ",\n";
    VALUE("kReplyNoMatch", kReplyNoMatch); out << ",\n";
    VALUE("kBeaconMagic", kBeaconMagic); out << ",\n";
    VALUE("kLayoutSize", kLayoutSize); out << "\n";
    out << "  },\n  \"enums\": {\n";
    out << "    \"HubFlags\": {\n";
    VALUE("kHubBypassed", HubFlags::kHubBypassed); out << ",\n";
    VALUE("kHubPreview", HubFlags::kHubPreview); out << ",\n";
    VALUE("kHubPanic", HubFlags::kHubPanic); out << ",\n";
    VALUE("kHubOffline", HubFlags::kHubOffline); out << ",\n";
    VALUE("kHubLimiter", HubFlags::kHubLimiter); out << "\n";
    out << "    },\n";
    out << "    \"RemoteParam\": {\n";
    VALUE("Panic", RemoteParam::Panic); out << ",\n";
    VALUE("Preview", RemoteParam::Preview); out << ",\n";
    VALUE("AutoSync", RemoteParam::AutoSync); out << ",\n";
    VALUE("Share", RemoteParam::Share); out << "\n";
    out << "    },\n";
    out << "    \"SlotState\": {\n";
    VALUE("kSlotFree", SlotState::kSlotFree); out << ",\n";
    VALUE("kSlotClaiming", SlotState::kSlotClaiming); out << ",\n";
    VALUE("kSlotActive", SlotState::kSlotActive); out << "\n";
    out << "    },\n";
    out << "    \"SlotFlags\": {\n";
    VALUE("kFlagMon", SlotFlags::kFlagMon); out << ",\n";
    VALUE("kFlagStr", SlotFlags::kFlagStr); out << ",\n";
    VALUE("kFlagSolo", SlotFlags::kFlagSolo); out << ",\n";
    VALUE("kFlagBypassed", SlotFlags::kFlagBypassed); out << ",\n";
    VALUE("kFlagOffline", SlotFlags::kFlagOffline); out << ",\n";
    VALUE("kFlagMono", SlotFlags::kFlagMono); out << ",\n";
    VALUE("kFlagApp", SlotFlags::kFlagApp); out << ",\n";
    VALUE("kFlagViewersViaHub", SlotFlags::kFlagViewersViaHub); out << "\n";
    out << "    },\n";
    out << "    \"ParamId\": {\n";
    VALUE("Mon", ParamId::Mon); out << ",\n";
    VALUE("Str", ParamId::Str); out << ",\n";
    VALUE("StrGainDb", ParamId::StrGainDb); out << ",\n";
    VALUE("StrPan", ParamId::StrPan); out << ",\n";
    VALUE("StrDelayMs", ParamId::StrDelayMs); out << ",\n";
    VALUE("MonTrimDb", ParamId::MonTrimDb); out << ",\n";
    VALUE("StrSolo", ParamId::StrSolo); out << ",\n";
    VALUE("StemIndex", ParamId::StemIndex); out << "\n";
    out << "    },\n";
    out << "    \"SlotHubStatus\": {\n";
    VALUE("kHubStatusNone", SlotHubStatus::kHubStatusNone); out << ",\n";
    VALUE("kHubStatusOk", SlotHubStatus::kHubStatusOk); out << ",\n";
    VALUE("kHubStatusRateMismatch", SlotHubStatus::kHubStatusRateMismatch); out << ",\n";
    VALUE("kHubStatusAhead", SlotHubStatus::kHubStatusAhead); out << ",\n";
    VALUE("kHubStatusUnderrun", SlotHubStatus::kHubStatusUnderrun); out << "\n";
    out << "    },\n";
    out << "    \"SourceFlags\": {\n";
    VALUE("kSrcOn", SourceFlags::kSrcOn); out << ",\n";
    VALUE("kSrcRecording", SourceFlags::kSrcRecording); out << ",\n";
    VALUE("kSrcFollowRec", SourceFlags::kSrcFollowRec); out << ",\n";
    VALUE("kSrcSystem", SourceFlags::kSrcSystem); out << ",\n";
    VALUE("kSrcDropped", SourceFlags::kSrcDropped); out << "\n";
    out << "    },\n";
    out << "    \"SourceParam\": {\n";
    VALUE("On", SourceParam::On); out << ",\n";
    VALUE("LevelDb", SourceParam::LevelDb); out << ",\n";
    VALUE("Record", SourceParam::Record); out << ",\n";
    VALUE("FollowRecord", SourceParam::FollowRecord); out << ",\n";
    VALUE("DelayMs", SourceParam::DelayMs); out << "\n";
    out << "    },\n";
    out << "    \"FriendState\": {\n";
    VALUE("kFriendFree", FriendState::kFriendFree); out << ",\n";
    VALUE("kFriendWaiting", FriendState::kFriendWaiting); out << ",\n";
    VALUE("kFriendLive", FriendState::kFriendLive); out << ",\n";
    VALUE("kFriendOffline", FriendState::kFriendOffline); out << "\n";
    out << "    },\n";
    out << "    \"FriendRoute\": {\n";
    VALUE("kRouteDirect", FriendRoute::kRouteDirect); out << ",\n";
    VALUE("kRouteDaw", FriendRoute::kRouteDaw); out << "\n";
    out << "    },\n";
    out << "    \"FeederStatus\": {\n";
    VALUE("kFeederFlowing", FeederStatus::kFeederFlowing); out << ",\n";
    VALUE("kFeederBypassed", FeederStatus::kFeederBypassed); out << ",\n";
    VALUE("kFeederOffline", FeederStatus::kFeederOffline); out << ",\n";
    VALUE("kFeederNoFriend", FeederStatus::kFeederNoFriend); out << "\n";
    out << "    },\n";
    out << "    \"RequestKind\": {\n";
    VALUE("kReqResolveToken", RequestKind::kReqResolveToken); out << ",\n";
    VALUE("kReqCreateFriend", RequestKind::kReqCreateFriend); out << ",\n";
    VALUE("kReqRelease", RequestKind::kReqRelease); out << ",\n";
    VALUE("kReqCopyLink", RequestKind::kReqCopyLink); out << ",\n";
    VALUE("kReqPairWith", RequestKind::kReqPairWith); out << ",\n";
    VALUE("kReqProbe", RequestKind::kReqProbe); out << "\n";
    out << "    }\n";
#undef VALUE
    out << "  },\n  \"structs\": {\n";
#define STRUCT(type) static_assert(std::is_standard_layout_v<type>); \
    out << "    \"" #type "\": {\n      \"size\": " << sizeof(type) \
        << ",\n      \"alignment\": " << alignof(type) << ",\n      \"fields\": {\n"
#define FIELD(type, member) out << "        \"" #member "\": {\"offset\": " \
    << offsetof(type, member) << ", \"size\": " << sizeof(type::member) \
    << ", \"alignment\": " << alignof(decltype(type::member)) << "}"
    STRUCT(RemoteCommand);
    FIELD(RemoteCommand, seq); out << ",\n";
    FIELD(RemoteCommand, target); out << ",\n";
    FIELD(RemoteCommand, paramId); out << ",\n";
    FIELD(RemoteCommand, valueBits); out << "\n";
    out << "      }\n    },\n";
    STRUCT(BusHeader);
    FIELD(BusHeader, magic); out << ",\n";
    FIELD(BusHeader, version); out << ",\n";
    FIELD(BusHeader, layoutSize); out << ",\n";
    FIELD(BusHeader, reserved0); out << ",\n";
    FIELD(BusHeader, hubOwnerToken); out << ",\n";
    FIELD(BusHeader, hubOwnerPid); out << ",\n";
    FIELD(BusHeader, hubSampleRate); out << ",\n";
    FIELD(BusHeader, hubHeartbeatNs); out << ",\n";
    FIELD(BusHeader, hubFlags); out << ",\n";
    FIELD(BusHeader, hubBlockSize); out << ",\n";
    FIELD(BusHeader, hubLatencyFrames); out << ",\n";
    FIELD(BusHeader, slotSequence); out << ",\n";
    FIELD(BusHeader, consumerHeartbeatNs); out << ",\n";
    FIELD(BusHeader, consumerCount); out << ",\n";
    FIELD(BusHeader, consumerBufferMs); out << ",\n";
    FIELD(BusHeader, remoteReserve); out << ",\n";
    FIELD(BusHeader, reserved1); out << ",\n";
    FIELD(BusHeader, remote); out << ",\n";
    FIELD(BusHeader, pad); out << "\n";
    out << "      }\n    },\n";
    STRUCT(Command);
    FIELD(Command, paramId); out << ",\n";
    FIELD(Command, value); out << "\n";
    out << "      }\n    },\n";
    STRUCT(TimelineTag);
    FIELD(TimelineTag, seq); out << ",\n";
    FIELD(TimelineTag, fifoPos); out << ",\n";
    FIELD(TimelineTag, timeSamples); out << ",\n";
    FIELD(TimelineTag, numFrames); out << ",\n";
    FIELD(TimelineTag, playing); out << "\n";
    out << "      }\n    },\n";
    STRUCT(SlotHeader);
    FIELD(SlotHeader, state); out << ",\n";
    FIELD(SlotHeader, epoch); out << ",\n";
    FIELD(SlotHeader, ownerPid); out << ",\n";
    FIELD(SlotHeader, sequence); out << ",\n";
    FIELD(SlotHeader, nameSeq); out << ",\n";
    FIELD(SlotHeader, colorARGB); out << ",\n";
    FIELD(SlotHeader, uuid); out << ",\n";
    FIELD(SlotHeader, name); out << ",\n";
    FIELD(SlotHeader, sampleRate); out << ",\n";
    FIELD(SlotHeader, numChannels); out << ",\n";
    FIELD(SlotHeader, heartbeatNs); out << ",\n";
    FIELD(SlotHeader, claimNs); out << ",\n";
    FIELD(SlotHeader, flags); out << ",\n";
    FIELD(SlotHeader, strGainBits); out << ",\n";
    FIELD(SlotHeader, strPanBits); out << ",\n";
    FIELD(SlotHeader, strDelayBits); out << ",\n";
    FIELD(SlotHeader, monTrimBits); out << ",\n";
    FIELD(SlotHeader, stemIndex); out << ",\n";
    FIELD(SlotHeader, cmdWrite); out << ",\n";
    FIELD(SlotHeader, cmds); out << ",\n";
    FIELD(SlotHeader, renameSeq); out << ",\n";
    FIELD(SlotHeader, renameTo); out << ",\n";
    FIELD(SlotHeader, peakInBits); out << ",\n";
    FIELD(SlotHeader, peakStreamBits); out << ",\n";
    FIELD(SlotHeader, hubStatus); out << ",\n";
    FIELD(SlotHeader, hubLeadFrames); out << ",\n";
    FIELD(SlotHeader, hubMute); out << ",\n";
    FIELD(SlotHeader, chainLatencyBits); out << ",\n";
    FIELD(SlotHeader, fedBy); out << ",\n";
    FIELD(SlotHeader, fxLatencyBits); out << ",\n";
    FIELD(SlotHeader, tagWrite); out << ",\n";
    FIELD(SlotHeader, writePos); out << ",\n";
    FIELD(SlotHeader, pad); out << "\n";
    out << "      }\n    },\n";
    STRUCT(SlotTags);
    FIELD(SlotTags, tags); out << "\n";
    out << "      }\n    },\n";
    STRUCT(StreamOutHeader);
    FIELD(StreamOutHeader, nameSeq); out << ",\n";
    FIELD(StreamOutHeader, sampleRate); out << ",\n";
    FIELD(StreamOutHeader, active); out << ",\n";
    FIELD(StreamOutHeader, reserved); out << ",\n";
    FIELD(StreamOutHeader, names); out << ",\n";
    FIELD(StreamOutHeader, peakBits); out << ",\n";
    FIELD(StreamOutHeader, loudnessMBits); out << ",\n";
    FIELD(StreamOutHeader, loudnessSBits); out << ",\n";
    FIELD(StreamOutHeader, writePos); out << "\n";
    out << "      }\n    },\n";
    STRUCT(SourceHeader);
    FIELD(SourceHeader, state); out << ",\n";
    FIELD(SourceHeader, ownerPid); out << ",\n";
    FIELD(SourceHeader, sequence); out << ",\n";
    FIELD(SourceHeader, nameSeq); out << ",\n";
    FIELD(SourceHeader, name); out << ",\n";
    FIELD(SourceHeader, app); out << ",\n";
    FIELD(SourceHeader, colorARGB); out << ",\n";
    FIELD(SourceHeader, flags); out << ",\n";
    FIELD(SourceHeader, heartbeatNs); out << ",\n";
    FIELD(SourceHeader, capture); out << ",\n";
    FIELD(SourceHeader, levelBits); out << ",\n";
    FIELD(SourceHeader, peakBits); out << ",\n";
    FIELD(SourceHeader, latencyBits); out << ",\n";
    FIELD(SourceHeader, recordMs); out << ",\n";
    FIELD(SourceHeader, delayBits); out << ",\n";
    FIELD(SourceHeader, hubMute); out << ",\n";
    FIELD(SourceHeader, trackSlot); out << ",\n";
    FIELD(SourceHeader, cmdWrite); out << ",\n";
    FIELD(SourceHeader, cmds); out << ",\n";
    FIELD(SourceHeader, appSeq); out << ",\n";
    FIELD(SourceHeader, appTo); out << ",\n";
    FIELD(SourceHeader, writePos); out << "\n";
    out << "      }\n    },\n";
    STRUCT(LinkInHeader);
    FIELD(LinkInHeader, sampleRate); out << ",\n";
    FIELD(LinkInHeader, active); out << ",\n";
    FIELD(LinkInHeader, heartbeatNs); out << ",\n";
    FIELD(LinkInHeader, writePos); out << "\n";
    out << "      }\n    },\n";
    STRUCT(FriendHeader);
    FIELD(FriendHeader, state); out << ",\n";
    FIELD(FriendHeader, id); out << ",\n";
    FIELD(FriendHeader, nameSeq); out << ",\n";
    FIELD(FriendHeader, name); out << ",\n";
    FIELD(FriendHeader, sampleRate); out << ",\n";
    FIELD(FriendHeader, heartbeatNs); out << ",\n";
    FIELD(FriendHeader, delayBits); out << ",\n";
    FIELD(FriendHeader, route); out << ",\n";
    FIELD(FriendHeader, feeder); out << ",\n";
    FIELD(FriendHeader, outSlot); out << ",\n";
    FIELD(FriendHeader, peakBits); out << ",\n";
    FIELD(FriendHeader, writePos); out << "\n";
    out << "      }\n    },\n";
    STRUCT(FeederRecord);
    FIELD(FeederRecord, state); out << ",\n";
    FIELD(FeederRecord, ownerPid); out << ",\n";
    FIELD(FeederRecord, friendId); out << ",\n";
    FIELD(FeederRecord, nameSeq); out << ",\n";
    FIELD(FeederRecord, trackName); out << ",\n";
    FIELD(FeederRecord, colorARGB); out << ",\n";
    FIELD(FeederRecord, heartbeatNs); out << ",\n";
    FIELD(FeederRecord, blockCount); out << ",\n";
    FIELD(FeederRecord, status); out << ",\n";
    FIELD(FeederRecord, reply); out << ",\n";
    FIELD(FeederRecord, replySeq); out << ",\n";
    FIELD(FeederRecord, hubCommand); out << ",\n";
    FIELD(FeederRecord, hubCommandSeq); out << ",\n";
    FIELD(FeederRecord, sampleRate); out << ",\n";
    FIELD(FeederRecord, tapWrite); out << "\n";
    out << "      }\n    },\n";
    STRUCT(PluginRequest);
    FIELD(PluginRequest, seq); out << ",\n";
    FIELD(PluginRequest, feeder); out << ",\n";
    FIELD(PluginRequest, kind); out << ",\n";
    FIELD(PluginRequest, arg); out << ",\n";
    FIELD(PluginRequest, text); out << "\n";
    out << "      }\n    },\n";
    STRUCT(FeederTap);
    FIELD(FeederTap, mono); out << "\n";
    out << "      }\n    },\n";
    STRUCT(BeaconHeader);
    FIELD(BeaconHeader, magic); out << ",\n";
    FIELD(BeaconHeader, protocolVersion); out << ",\n";
    FIELD(BeaconHeader, hubPid); out << ",\n";
    FIELD(BeaconHeader, heartbeatNs); out << "\n";
    out << "      }\n    },\n";
    STRUCT(ChannelRing);
    FIELD(ChannelRing, ch); out << "\n";
    out << "      }\n    },\n";
    STRUCT(BusLayout);
    FIELD(BusLayout, header); out << ",\n";
    FIELD(BusLayout, slots); out << ",\n";
    FIELD(BusLayout, slotTags); out << ",\n";
    FIELD(BusLayout, streamHeader); out << ",\n";
    FIELD(BusLayout, sources); out << ",\n";
    FIELD(BusLayout, slotAudio); out << ",\n";
    FIELD(BusLayout, sourceAudio); out << ",\n";
    FIELD(BusLayout, linkIn); out << ",\n";
    FIELD(BusLayout, linkInAudio); out << ",\n";
    FIELD(BusLayout, streamAudio); out << ",\n";
    FIELD(BusLayout, friends); out << ",\n";
    FIELD(BusLayout, feeders); out << ",\n";
    FIELD(BusLayout, requests); out << ",\n";
    FIELD(BusLayout, requestReserve); out << ",\n";
    FIELD(BusLayout, feederTap); out << ",\n";
    FIELD(BusLayout, friendAudio); out << ",\n";
    FIELD(BusLayout, friendMixWrite); out << ",\n";
    FIELD(BusLayout, friendMixAudio); out << ",\n";
    FIELD(BusLayout, friendVoiceWrite); out << ",\n";
    FIELD(BusLayout, friendVoiceAudio); out << "\n";
    out << "      }\n    }\n";
#undef FIELD
#undef STRUCT
    out << "  }\n}\n";
    return out.str();
}
} // namespace

int main(int argc, char** argv)
{
    const auto actual = layoutJson();
    if (argc == 1) {
        std::cout << actual;
        return std::cout ? 0 : 1;
    }
    if (argc != 3 || std::string(argv[1]) != "--check") {
        std::cerr << "Usage: hs-layout-dump [--check fixture.json]\n";
        return 2;
    }
    std::ifstream fixture(argv[2]);
    if (!fixture) {
        std::cerr << "Cannot open layout fixture: " << argv[2] << '\n';
        return 1;
    }
    const std::string expected{std::istreambuf_iterator<char>(fixture), {}};
    if (fixture.bad() || actual != expected) {
        std::cerr << "Shared-memory layout differs from " << argv[2] << '\n';
        return 1;
    }
    return 0;
}
