// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Generated; do not edit. Descriptors only (element type, length and hashes of each table): the
// values are read from your DLL at runtime and verified by their XXH3-128 (dll/tables.hpp).
#include "syxg50/dll_tables.hpp"

#include "dll/tables.hpp"
#include "engine_tables.hpp"

#define DEST(m) [](EngineTables& t) -> void* { return t.m.data(); }

namespace hvyxg::syxg50::dll {

namespace {

using table_store::Elem;
using table_store::kSyxg50;

const table_store::TableDesc kDescs[] = {
    {"kAttackTime", Elem::F32, Elem::F32, 128, 0x7b8d2d58691835eeull, 0xb62b6f875627fbe7ull, 0x75dda06ffc647a22ull, DEST(syxg50.kAttackTime), kSyxg50},
    {"kLevelAtten", Elem::U8, Elem::U8, 129, 0x3c511f3d68072897ull, 0x414bb310089442c0ull, 0x3de7e6ae0de3b9ebull, DEST(syxg50.kLevelAtten), kSyxg50},
    {"kVelocityCurves", Elem::U8, Elem::U8, 896, 0x66083784ba5aab25ull, 0xdd2014e76d35ba99ull, 0xc26a55c251a3cf0cull, DEST(syxg50.kVelocityCurves), kSyxg50},
    {"kPegStep", Elem::S16, Elem::S16, 64, 0x294cc0456725c32bull, 0x3a4a4f517ddec6f2ull, 0x32e414b7ea73fcc9ull, DEST(syxg50.kPegStep), kSyxg50},
    {"kFegStep", Elem::S16, Elem::S16, 64, 0x27d6f73f275595f7ull, 0x984af871f4c29900ull, 0x9d000f0a37253da7ull, DEST(syxg50.kFegStep), kSyxg50},
    {"kFegPartRateClamp", Elem::U8, Elem::U8, 128, 0xa0c874ce4bd2d438ull, 0x4c6b4784eb1b3047ull, 0x589aa41c687a9b11ull, DEST(syxg50.kFegPartRateClamp), kSyxg50},
    {"kLfoSpeed", Elem::U8, Elem::U8, 64, 0x2c43a2f4d2e36292ull, 0xd449c69e453e35baull, 0xd7f5116ded34c2aeull, DEST(syxg50.kLfoSpeed), kSyxg50},
    {"kVibratoRate", Elem::U8, Elem::U8, 166, 0xf3bd54ac4a8ed806ull, 0xa62eeb1c7ef422aaull, 0xa4f72285ded71523ull, DEST(syxg50.kVibratoRate), kSyxg50},
    {"kCtrlPmdXg", Elem::U8, Elem::U8, 128, 0xf52e9e83460d23f8ull, 0xa0a2363099af5437ull, 0xc350cbac08878583ull, DEST(syxg50.kCtrlPmdXg), kSyxg50},
    {"kCtrlPmdGs", Elem::U8, Elem::U8, 128, 0x004827e08a7d34caull, 0xbcd68bbe1086c195ull, 0x6ca5ab61049b4d1eull, DEST(syxg50.kCtrlPmdGs), kSyxg50},
    {"kFmdTable", Elem::U8, Elem::U8, 16, 0x1d57fcd8afb6c4b1ull, 0x4bb7b16d3dad2020ull, 0xecea6d8a617da96eull, DEST(syxg50.kFmdTable), kSyxg50},
    {"kPartAttackRate", Elem::U8, Elem::U8, 64, 0x81a1d1b22f3c7bccull, 0xac29e9dbeaa40840ull, 0x8c4e7e50ca08260aull, DEST(syxg50.kPartAttackRate), kSyxg50},
    {"kPartReleaseRate", Elem::U8, Elem::U8, 112, 0xb7edc8903671ea55ull, 0x5d5e2c202e64bcfeull, 0x8c6aac64f2cfcd81ull, DEST(syxg50.kPartReleaseRate), kSyxg50},
    {"kVibratoDepth", Elem::S8, Elem::S8, 128, 0x739bf27c7c6b5a13ull, 0x8b506c67da6aa1daull, 0x0c81eb97d0ca2244ull, DEST(syxg50.kVibratoDepth), kSyxg50},
    {"kVibratoDelay", Elem::U8, Elem::U8, 384, 0x9007eccd21998579ull, 0x74fd111dfc78b112ull, 0xfbff0535b32bd74aull, DEST(syxg50.kVibratoDelay), kSyxg50},
    {"kPartPan", Elem::U8, Elem::U8, 128, 0x886c1e407474c103ull, 0xbb103f6081998411ull, 0x6e39c76507144b3dull, DEST(syxg50.kPartPan), kSyxg50},
    {"kElementPan", Elem::U8, Elem::U8, 128, 0x4e4b4c355f37b915ull, 0x490b47b6f349e03aull, 0xfa14909a35d3de52ull, DEST(syxg50.kElementPan), kSyxg50},
    {"kKeyPan", Elem::U8, Elem::U8, 128, 0x2d3c7a392d6ef600ull, 0xf3b056908046d249ull, 0x77d8144783ec576dull, DEST(syxg50.kKeyPan), kSyxg50},
    {"kChorusPanAtten", Elem::U8, Elem::U8, 128, 0xd8a403668639d588ull, 0x69d82c2dc0850a09ull, 0x3e32e358cafaf874ull, DEST(syxg50.kChorusPanAtten), kSyxg50},
    {"kKeyOnDelay", Elem::U16, Elem::U16, 16, 0x88fa5ce84a31404cull, 0x162f06f76d50cce8ull, 0x3a2bf98c93c38cc8ull, DEST(syxg50.kKeyOnDelay), kSyxg50},
    {"kKeyTrack", Elem::U8, Elem::U8, 8, 0x925bb29fb0ccbb6bull, 0xe2ebba7b7ee02d27ull, 0x92df5e7964fd0a59ull, DEST(syxg50.kKeyTrack), kSyxg50},
    {"kFegVelCurve0", Elem::U8, Elem::U8, 128, 0x62703876bfcb3d7eull, 0x068787f8dcef0897ull, 0xa0a7107af6a67432ull, DEST(syxg50.kFegVelCurve0), kSyxg50},
    {"kFegVelCurve1", Elem::U8, Elem::U8, 128, 0x98fdf376e0dd4fdeull, 0x1abd0c836a1fec63ull, 0xc11a9da77985e15eull, DEST(syxg50.kFegVelCurve1), kSyxg50},
    {"kPortaRate", Elem::U16, Elem::U16, 128, 0xf1a1f58184a9ac95ull, 0x43f2eeb810805bc9ull, 0xbe9f3b0f211f4c0bull, DEST(syxg50.kPortaRate), kSyxg50},
    {"kDrumDefault", Elem::U8, Elem::U8, 16, 0x5d69fcdedc3fc123ull, 0xf90fdcf950670e03ull, 0x1d82a1e7d2b73798ull, DEST(syxg50.kDrumDefault), kSyxg50},
    {"kDrumTemplate", Elem::U8, Elem::U8, 16, 0x2deabf612bd96e00ull, 0xe08b16ed4c19e687ull, 0x2f83318fd88c6311ull, DEST(syxg50.kDrumTemplate), kSyxg50},
    {"kDrumAutoReleaseXg", Elem::U8, Elem::U8, 128, 0xff496a202c2a66e3ull, 0x75fb5661ff458ec0ull, 0x11904a0c21119932ull, DEST(syxg50.kDrumAutoReleaseXg), kSyxg50},
    {"kDrumAutoReleaseGs", Elem::U8, Elem::U8, 128, 0x9e6c37bbb07b886dull, 0x55799cdcf3b03a0full, 0xd7dbfcc5f6dfa5d8ull, DEST(syxg50.kDrumAutoReleaseGs), kSyxg50},
    {"kDrumAutoReleaseBrush", Elem::U8, Elem::U8, 128, 0xc188924a0ad19fb1ull, 0x7911754553c8a0abull, 0x785d8ce9664b037aull, DEST(syxg50.kDrumAutoReleaseBrush), kSyxg50},
    {"kCutoffGlideRate", Elem::U8, Elem::U8, 128, 0x6adf6d96aaee01faull, 0xcc8f7943cddf54d5ull, 0x7f5ea5973aaae710ull, DEST(syxg50.kCutoffGlideRate), kSyxg50},
    {"kVibratoDepthGs", Elem::S8, Elem::S8, 128, 0xac74dbdf8276d31aull, 0xbdffd4e3b9b84747ull, 0xa73d325111523899ull, DEST(syxg50.kVibratoDepthGs), kSyxg50},
    {"kGsVibratoRateBase", Elem::U8, Elem::U8, 128, 0xd90c6141bb764ed6ull, 0xeaf9a1c9c3e91a53ull, 0xbae4aa37e17eba4full, DEST(syxg50.kGsVibratoRateBase), kSyxg50},
    {"kGsReleaseOffset", Elem::U8, Elem::U8, 128, 0xe8d614e67839d523ull, 0x445a605619e64451ull, 0x038fcb8ebd0e7ec4ull, DEST(syxg50.kGsReleaseOffset), kSyxg50},
    {"kGsVibratoDelayTable", Elem::U8, Elem::U8, 128, 0x799748b409df6d00ull, 0xd1b0495e1738e6e8ull, 0xdbbaac888c9af67eull, DEST(syxg50.kGsVibratoDelayTable), kSyxg50},
    {"kGsPartCutoff", Elem::S16, Elem::S16, 65, 0x0221d3faaa39691bull, 0x00d06ae145a72449ull, 0xffcb84bc383d2fa6ull, DEST(syxg50.kGsPartCutoff), kSyxg50},
    {"kGsDetuneUp", Elem::U16, Elem::U16, 128, 0xc294f6bbfd0610faull, 0xa420e46331deee92ull, 0xd6ff5a795f96d3e9ull, DEST(syxg50.kGsDetuneUp), kSyxg50},
    {"kGsDetuneDown", Elem::U16, Elem::U16, 128, 0xe03da3a297496aa2ull, 0xe2b58b8b0f3f65e7ull, 0xe5dc87aa88bd4498ull, DEST(syxg50.kGsDetuneDown), kSyxg50},
    {"kDecayTime", Elem::F32, Elem::F32, 128, 0xd407a1578d964609ull, 0x3ffcbc9a39918e6eull, 0x7723340628c1513dull, DEST(syxg50.kDecayTime), kSyxg50},
    {"kReleaseTime", Elem::F32, Elem::F32, 128, 0x3f7ff562e5ef8007ull, 0xceca8e0bedaa4680ull, 0xf369a47168402ed2ull, DEST(syxg50.kReleaseTime), kSyxg50},
    {"kCutoff", Elem::S16, Elem::S16, 128, 0x567de94565d502adull, 0x154f256a19a8e409ull, 0x623e7e5f84465f19ull, DEST(syxg50.kCutoff), kSyxg50},
    {"kVibratoPmd", Elem::U8, Elem::U8, 128, 0x0d8d0d41044f6c20ull, 0x15615178bf3bb280ull, 0xcdcb0dcbb91b9a04ull, DEST(syxg50.kVibratoPmd), kSyxg50},
    {"kElementPanPos", Elem::U8, Elem::U8, 16, 0x123e7db7fb0bf665ull, 0x06dae29b9f626563ull, 0xf3e40fb55baddbe1ull, DEST(syxg50.kElementPanPos), kSyxg50},
    {"kSendAtten", Elem::U8, Elem::U8, 128, 0xe036b337179e1157ull, 0x65b455b3c33287bdull, 0x5af197a6ad54f183ull, DEST(syxg50.kSendAtten), kSyxg50},
    {"kGsReverbMacro", Elem::U8, Elem::U8, 48, 0x199f110739919447ull, 0xf42157d29835ff74ull, 0x86f3d16b55397d95ull, DEST(syxg50.kGsReverbMacro[0]), kSyxg50},
    {"kGsCharacterType", Elem::U8, Elem::U8, 16, 0x53bc74ab65a2dc08ull, 0xea73699828281e10ull, 0x7263407c700231d3ull, DEST(syxg50.kGsCharacterType[0]), kSyxg50},
    {"kGsCharacterParams", Elem::U8, Elem::U8, 96, 0x4476d3305310893cull, 0xb42712cb8a00ade8ull, 0x330c5e785abca0d5ull, DEST(syxg50.kGsCharacterParams[0]), kSyxg50},
    {"kGsChorusMacro", Elem::U8, Elem::U8, 56, 0xe0cb28a0d8331644ull, 0x59a73b39f5ace189ull, 0x4bb101902731557cull, DEST(syxg50.kGsChorusMacro[0]), kSyxg50},
    {"kGsChorusParams", Elem::U8, Elem::U8, 16, 0x43d0c7f764e97233ull, 0xbc3683ba1fbef6caull, 0xdab7614ae8cd8e01ull, DEST(syxg50.kGsChorusParams), kSyxg50},
};

} // namespace

} // namespace hvyxg::syxg50::dll

std::span<const hvyxg::table_store::TableDesc> hvyxg::table_store::syxg50_voice_tables() {
    return syxg50::dll::kDescs;
}
