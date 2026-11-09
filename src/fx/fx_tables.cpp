// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Generated; do not edit. Descriptors only (element type, length and hashes of each table, the
// same in syxg50.dll and S-YXG2006LE.DLL): the values are read from your DLL at runtime and
// verified by their xxHash3-128 (dll/tables.hpp).
#include "fx/fx_tables.hpp"

#include "dll/tables.hpp"
#include "engine_tables.hpp"

#define DEST(m) [](EngineTables& t) -> void* { return t.m.data(); }

namespace hvyxg::fx {

namespace {

void clamp_flanger_delay(void* t) { static_cast<std::uint16_t*>(t)[7] = 63; }

using table_store::Elem;
using table_store::kBoth;
using table_store::kSyxg50;
using table_store::kSyxg2006le;

const table_store::TableDesc kDescs[] = {
    {"fx::kCoefFormat", Elem::U8, Elem::U8, 192, 0x8edb02bf4b83aa44ull, 0x348193cb0220a16cull, 0x162d5799846baaa7ull, DEST(fx.kCoefFormat), kBoth},
    {"fx::kReverbProgramIndex", Elem::U8, Elem::U8, 19, 0x67fcc10ac65cd520ull, 0x601dcad0088e69a1ull, 0xeab996a60c4c0f37ull, DEST(fx.kReverbProgramIndex), kBoth},
    {"fx::kReverbProgram", Elem::U16, Elem::U16, 288, 0xd444c0280ff6433aull, 0x45d6cfefed0f950bull, 0x35d8ccdbc23d07c5ull, DEST(fx.kReverbProgram), kBoth},
    {"fx::kReverbCoefList", Elem::U8, Elem::U8, 45, 0x2317017ac82b29c1ull, 0xe54c5827f94e4005ull, 0xffffdcc11d060d22ull, DEST(fx.kReverbCoefList), kBoth},
    {"fx::kReverbAddrList", Elem::U8, Elem::U8, 27, 0x8c1400add6a0a5aeull, 0xe88dbf03c669ec9dull, 0x3ca88d9c975cbb41ull, DEST(fx.kReverbAddrList), kBoth},
    {"fx::kChorusProgramIndex", Elem::U8, Elem::U8, 21, 0x0d2fd1fe02601258ull, 0x8aa469957eae533aull, 0xede79be2242a15ebull, DEST(fx.kChorusProgramIndex), kBoth},
    {"fx::kChorusProgram", Elem::U16, Elem::U16, 266, 0xb830f70d48671771ull, 0x199330f2cbca949eull, 0x1a6e6efed65f5f3eull, DEST(fx.kChorusProgram), kBoth},
    {"fx::kChorusCoefList", Elem::U8, Elem::U8, 28, 0x573ea3dbf29472ddull, 0xa82e516636f16f45ull, 0x4da8bf066122304aull, DEST(fx.kChorusCoefList), kBoth},
    {"fx::kChorusAddrList", Elem::U8, Elem::U8, 10, 0x4b5155ff86db8fefull, 0xcd5795289bf5837full, 0x27169d49b2d4357full, DEST(fx.kChorusAddrList), kBoth},
    {"fx::kMixerProgram", Elem::S16, Elem::S16, 23, 0xf11780cf3a1c8e3cull, 0xbed3c2b71f6063b3ull, 0x59eccc87c83b71b9ull, DEST(fx.kMixerProgram), kBoth},
    {"fx::kMixerCoefList", Elem::U8, Elem::U8, 23, 0xc23e5ffd296c9c9bull, 0xa21f1560dd865fe8ull, 0x084824bc759255fbull, DEST(fx.kMixerCoefList), kBoth},
    {"fx::kReverbTimeBase", Elem::U8, Elem::U8, 72, 0x605836aabbe11a3dull, 0xace228bc06f462c9ull, 0x3f79283a26dfc9caull, DEST(fx.kReverbTimeBase), kBoth},
    {"fx::kReverbTimeOffset", Elem::U8, Elem::U8, 16, 0xc5782e3075b61dfeull, 0xc01eedb36f86a879ull, 0x7fe72060e7c133a0ull, DEST(fx.kReverbTimeOffset), kBoth},
    {"fx::kCombGain", Elem::U16, Elem::U16, 416, 0x69fb97fc459b7c96ull, 0xcb797602811bf230ull, 0x300da7d4a477997bull, DEST(fx.kCombGain), kBoth},
    {"fx::kReverbTimeSet", Elem::U16, Elem::U16, 19, 0xd703b5c5bd6655a4ull, 0x3cdf600f41a27176ull, 0xc5765c736af1b9b9ull, DEST(fx.kReverbTimeSet), kBoth},
    {"fx::kDiffusionGeometry", Elem::U16, Elem::U16, 64, 0x4b1dec01894a1cc5ull, 0xc0ad10cad48a0a39ull, 0xea2a82e6b18cc08dull, DEST(fx.kDiffusionGeometry), kBoth},
    {"fx::kDiffusionSpread", Elem::U16, Elem::U16, 4, 0x83251e36fce33b31ull, 0x7c4468f9e7558af2ull, 0x7c2b1b8f3b13892aull, DEST(fx.kDiffusionSpread), kBoth},
    {"fx::kDiffusionDepth", Elem::U8, Elem::U8, 11, 0xe042f39346bd8965ull, 0x071c5b002dac56e0ull, 0xa22a665185854198ull, DEST(fx.kDiffusionDepth), kBoth},
    {"fx::kRoomSize", Elem::U16, Elem::U16, 200, 0x42a4223b6acbf4fbull, 0x4e977320ba66181bull, 0x9738b5db65bc0fe4ull, DEST(fx.kRoomSize), kBoth},
    {"fx::kRoomTimeOffset", Elem::U8, Elem::U8, 200, 0xc0bdc95dfa99b20aull, 0x269d89f89dbaf1a4ull, 0xea0ac1cb9f4139c0ull, DEST(fx.kRoomTimeOffset), kBoth},
    {"fx::kAllpassGeometry", Elem::U16, Elem::U16, 64, 0xa1ee921008948492ull, 0x19b93dd7eb1a283cull, 0x9f5fcc3271cb473cull, DEST(fx.kAllpassGeometry), kBoth},
    {"fx::kDensityMask", Elem::U8, Elem::U8, 4, 0x0028d6000022bdc5ull, 0x85b7b45c370f93e9ull, 0x02a65ac5dd1e7326ull, DEST(fx.kDensityMask), kBoth},
    {"fx::kChorusOutGain", Elem::U8, Elem::U8, 21, 0x8966b5ebb42d835aull, 0xdc66e2b73efeb25dull, 0xf17987b6a7bde403ull, DEST(fx.kChorusOutGain), kBoth},
    {"fx::kControlLists", Elem::U8, Elem::U8, 192, 0xf90cacf28f0cb542ull, 0xfc8b19aadc904fb1ull, 0xb8913cb5a0c68048ull, DEST(fx.kControlLists), kBoth},
    {"fx::kAllpassCoefList", Elem::U8, Elem::U8, 16, 0x7c8cef1eabb38490ull, 0xb6183ec93d6eee41ull, 0xe5c8824dbeae89ecull, DEST(fx.kAllpassCoefList), kBoth},
    {"fx::kChorusLists", Elem::U8, Elem::U8, 48, 0x82cd54a287a6449aull, 0x6407af79dee9f672ull, 0xf3a9ea0952a83bb9ull, DEST(fx.kChorusLists), kBoth},
    {"fx::kLfoWord", Elem::U16, Elem::U16, 128, 0x26fb29fa1d2d7e64ull, 0x4def0452e206ebf6ull, 0xe1111d27cd24880cull, DEST(fx.kLfoWord), kBoth},
    {"fx::kChorusDelay", Elem::U16, Elem::U16, 128, 0x23d2d7f793cc7f87ull, 0xe349b0193f825d38ull, 0x1469b9a5f1d088bfull, DEST(fx.kChorusDelay), kBoth},
    {"fx::kInitialDelay", Elem::U16, Elem::U16, 64, 0x58d7960ebb1aa6a9ull, 0x61a92875bd6bf122ull, 0xdeec7bb3b7db738bull, DEST(fx.kInitialDelay), kBoth},
    {"fx::kLevelCurve", Elem::S16, Elem::S16, 129, 0x6e08d61df0690060ull, 0x9cf3ea520e418112ull, 0x251ca2cf9348a16aull, DEST(fx.kLevelCurve), kBoth},
    {"fx::kPanCurve", Elem::S16, Elem::S16, 129, 0x5e599f396f0c7d9bull, 0x30c76c47fe3fa084ull, 0xeda575431d56b466ull, DEST(fx.kPanCurve), kBoth},
    {"fx::kErScale", Elem::U8, Elem::U8, 19, 0x33c4f9c82a9dde2aull, 0x3d8ea9f9fc53b4b7ull, 0xb94ab9dc0ba1ea06ull, DEST(fx.kErScale), kBoth},
    {"fx::kTan44100", Elem::F32, Elem::F32, 61, 0xd6541d15c878547aull, 0x86706c0f1f7a0831ull, 0x320e9662b9b061ffull, DEST(fx.kTan44100), kBoth},
    {"fx::kShelfGain", Elem::F32, Elem::F32, 25, 0x7069b4b8eef2b9beull, 0x1eb0963155b05caaull, 0x3b1b645049317687ull, DEST(fx.kShelfGain), kBoth},
    {"fx::kReverbLimits", Elem::U16, Elem::U16, 32, 0xfc2c1ad0b0aab6f6ull, 0xe0e0c661f26ca867ull, 0x3924bd291ac59f54ull, DEST(fx.kReverbLimits), kBoth},
    {"fx::kRoomReverbLimits", Elem::U16, Elem::U16, 32, 0x42b3445521b2bd02ull, 0xb7d8d4356cef4cadull, 0x27c776a21b7e33cdull, DEST(fx.kRoomReverbLimits), kBoth},
    {"fx::kChorusLimits", Elem::U16, Elem::U16, 32, 0x6953bbb8f05238e4ull, 0x972e040bc2f3294cull, 0xdab958133f706c70ull, DEST(fx.kChorusLimits), kBoth},
    {"fx::kMixerLimits", Elem::U16, Elem::U16, 32, 0x4e71ba8be897e519ull, 0x5b72651a941d1823ull, 0x98222c95ac610cb9ull, DEST(fx.kMixerLimits), kBoth},
    {"fx::kVarCoefFormat", Elem::U8, Elem::U8, 768, 0x08086c71c6536e9cull, 0xa3371aa814a10588ull, 0xf4078ddecc780b5cull, DEST(fx.kVarCoefFormat), kBoth},
    {"fx::kVarCoefList", Elem::U8, Elem::U8, 79, 0xcffec6bf7e304584ull, 0xf0ac09f82d781562ull, 0xea21ab83f291672dull, DEST(fx.kVarCoefList), kBoth},
    {"fx::kVarProgramIndex0", Elem::U8, Elem::U8, 24, 0x996872ceb5bd62adull, 0x0c01d287faa65d6dull, 0xfc20255c51d8f4e0ull, DEST(fx.kVarProgramIndex0), kBoth},
    {"fx::kVarProgramIndex1", Elem::U8, Elem::U8, 4, 0x000368000002e68full, 0x937fcafc26a36464ull, 0x6799a9357008ba03ull, DEST(fx.kVarProgramIndex1), kBoth},
    {"fx::kVarProgramIndex2", Elem::U8, Elem::U8, 12, 0xbcd910c006540bb3ull, 0x6df11e39dfb20b09ull, 0x66449d9ebfe7e345ull, DEST(fx.kVarProgramIndex2), kBoth},
    {"fx::kVarProgramIndex3", Elem::U8, Elem::U8, 16, 0x6530eba1e0d3358bull, 0x78c6a49333b5c7baull, 0x8f5159894f88e943ull, DEST(fx.kVarProgramIndex3), kBoth},
    {"fx::kVarProgram0", Elem::U16, Elem::U16, 1166, 0xe2855d7f9ad4fc52ull, 0x0f033342ef07c700ull, 0xf72ef0f35649abfdull, DEST(fx.kVarProgram0), kBoth},
    {"fx::kVarProgram1", Elem::U16, Elem::U16, 318, 0xa4297f2e176e5657ull, 0xd437d23d08deba1aull, 0xb7336af01c149d2aull, DEST(fx.kVarProgram1), kBoth},
    {"fx::kVarProgram2", Elem::U16, Elem::U16, 424, 0xe427979927dcbc84ull, 0xf422dbe6707d4d5dull, 0x760f4b6bda109f8full, DEST(fx.kVarProgram2), kBoth},
    {"fx::kVarProgram3", Elem::U16, Elem::U16, 530, 0x53c335c4517017e8ull, 0x79aef0f42db50c59ull, 0xac4adfd158e2023full, DEST(fx.kVarProgram3), kBoth},
    {"fx::kVarDefaults0", Elem::U16, Elem::U16, 368, 0x7cd4e2119784a82cull, 0xbd0a11136f9a620dull, 0xcc6b2d5bf351cb78ull, DEST(fx.kVarDefaults0), kBoth},
    {"fx::kVarDefaults1", Elem::U16, Elem::U16, 48, 0x711bd3844fb88ca9ull, 0x144d1ef66cd3aa85ull, 0x2f24532672eefb4cull, DEST(fx.kVarDefaults1), kBoth},
    {"fx::kVarDefaults2", Elem::U16, Elem::U16, 144, 0xd76fa84c9c1717a8ull, 0x4c438990b38f39deull, 0x9410bab7e4dd7d86ull, DEST(fx.kVarDefaults2), kBoth},
    {"fx::kVarDefaults3", Elem::U16, Elem::U16, 208, 0xe34eda7ef137b9efull, 0xca0cbcbf0023ad11ull, 0xd67de2d873151a77ull, DEST(fx.kVarDefaults3), kBoth},
    {"fx::kVarOutGain", Elem::U8, Elem::U8, 92, 0x29ac4a0b0acfc64cull, 0x58da6bc91a69fa92ull, 0x2cf131db57434669ull, DEST(fx.kVarOutGain), kBoth},
    {"fx::kVarTypeSub", Elem::U8, Elem::U8, 120, 0x13858357ceffc072ull, 0xf3f38c9be9ec1b3full, 0x55edc59f526d9daeull, DEST(fx.kVarTypeSub), kBoth},
    {"fx::kVarTypeCat", Elem::U8, Elem::U8, 120, 0x7efb905304e0f548ull, 0x0fbc07d3be462ec8ull, 0x24d8dab9932da8ccull, DEST(fx.kVarTypeCat), kBoth},
    {"fx::kVarTypeSub14", Elem::U8, Elem::U8, 10, 0x0a0c0380dc15d57full, 0x6db9b36982bc9dacull, 0xff0f29401e835ebeull, DEST(fx.kVarTypeSub14), kBoth},
    {"fx::kVarTypeCat14", Elem::U8, Elem::U8, 10, 0x1ced4804273fab4cull, 0x1badc61f81555ccdull, 0xcffa39db6671af4full, DEST(fx.kVarTypeCat14), kBoth},
    {"fx::kVarTypeSub40", Elem::U8, Elem::U8, 150, 0xd6decdb56c5b4506ull, 0x5741d2b279200fd0ull, 0x7ce10a430d39f0f4ull, DEST(fx.kVarTypeSub40), kBoth},
    {"fx::kVarTypeCat40", Elem::U8, Elem::U8, 150, 0x40220731d7740670ull, 0x4a54133d7c103cf9ull, 0xf61d017383281e9eull, DEST(fx.kVarTypeCat40), kBoth},
    {"fx::kVarListsA", Elem::U8, Elem::U8, 920, 0x4d40413eedbb5bedull, 0x21a453d74869092eull, 0xeebb080f44d3665bull, DEST(fx.kVarListsA), kBoth},
    {"fx::kVarListsB", Elem::U8, Elem::U8, 560, 0x86122c0e18a1f8f9ull, 0x0ea588a0acb829f5ull, 0xa7afb1afee7f4edbull, DEST(fx.kVarListsB), kBoth},
    {"fx::kLevelSquare", Elem::F32, Elem::F32, 128, 0xa3256c6649f9a2bcull, 0x6dd7b75181e9ed31ull, 0xfe5594ac2629f91aull, DEST(fx.kLevelSquare), kBoth},
    {"fx::kErSet", Elem::U16, Elem::U16, 30, 0xf01a7cadce1d44f6ull, 0xe9ef7bf6561b37fdull, 0x6323b1e64fe302c8ull, DEST(fx.kErSet), kBoth},
    {"fx::kKaraokeDelay", Elem::U16, Elem::U16, 128, 0x1398c56407187102ull, 0xf1f799882db895c0ull, 0x5f0f490bd1066ee9ull, DEST(fx.kKaraokeDelay), kBoth},
    {"fx::kReverbTypeMap", Elem::U8, Elem::U8, 50, 0x1f8795f50f386a17ull, 0xa5de864282693c5eull, 0xcba6fd7792d3a195ull, DEST(fx.kReverbTypeMap), kBoth},
    {"fx::kReverbTypeMapRoom", Elem::U8, Elem::U8, 40, 0x47e6b4fcfbf7409cull, 0xfe478faadf4b2b53ull, 0x1dc3e34f1932c8d1ull, DEST(fx.kReverbTypeMapRoom), kBoth},
    {"fx::kChorusTypeMap", Elem::U8, Elem::U8, 30, 0x7e9faa366953797dull, 0x674d2bee3854e1caull, 0x59b987f68fa84c2dull, DEST(fx.kChorusTypeMap), kBoth},
    {"fx::kReverbDefaults", Elem::U16, Elem::U16, 304, 0x13bfdaaec1ac1185ull, 0x4f2c338e2164e875ull, 0x9636ddff022aa3acull, DEST(fx.kReverbDefaults), kBoth},
    {"fx::kChorusDefaults", Elem::U16, Elem::U16, 336, 0xb9ad6d32444c1132ull, 0xb6caec8612071369ull, 0xd422a50112ed399bull, DEST(fx.kChorusDefaults), kBoth},
    {"fx::kMixerWord", Elem::U16, Elem::U16, 128, 0x7737a89d6ed064d0ull, 0x3957bdc8c4f3fdd2ull, 0x23c8201b4aea5125ull, DEST(fx.kMixerWord), kBoth},
    {"fx::kMixerLevel", Elem::U8, Elem::U8, 128, 0xe036b337179e1157ull, 0x65b455b3c33287bdull, 0x5af197a6ad54f183ull, DEST(fx.kMixerLevel), kBoth},
    {"fx::kMixerPan", Elem::U8, Elem::U8, 128, 0x850054f60972cbc6ull, 0xb21c796b66681409ull, 0xf23f2a7a97950ca8ull, DEST(fx.kMixerPan), kBoth},
    {"fx::kErTapAddr", Elem::U8, Elem::U8, 14, 0x0dc920efcf67b67cull, 0xeafc5258980a71d3ull, 0xcacf84cb171c208eull, DEST(fx.kErTapAddr), kBoth},
    {"fx::kGsTimeRoom1Plate", Elem::U8, Elem::U8, 128, 0x4ee75e016eced049ull, 0x87f0783e9988d26cull, 0x8e15c878c7376fe4ull, DEST(fx.kGsTimeRoom1Plate), kBoth},
    {"fx::kGsTimeOther", Elem::U8, Elem::U8, 128, 0x1c59a955d6964c17ull, 0x18728a495a712692ull, 0x4586cbcddbe01ba6ull, DEST(fx.kGsTimeOther), kBoth},
    {"fx::kGsPreLpf", Elem::U8, Elem::U8, 8, 0x76283d631d92dc2aull, 0xd76e739f4dfa570aull, 0xd5a1c49ca62831d2ull, DEST(fx.kGsPreLpf), kBoth},
    {"fx::kVarLimits2006LE", Elem::U16, Elem::U16, 768, 0xf3d5deaf7aa21667ull, 0x7d7026d6ad2bcc45ull, 0x4462fdf03f3cc3d3ull, DEST(fx.kVarLimits2006LE), kSyxg2006le},
    {"fx::kVarLimitsSyxg50", Elem::U16, Elem::U16, 768, 0x3ae21d7aa6b7777bull, 0x56ccb83d1bddb7abull, 0xc36f02659229bd38ull, DEST(fx.kVarLimitsSyxg50), kSyxg50},
    {"fx::kMixerVolume", Elem::U8, Elem::U8, 128, 0x5b84d0b43f2e79bfull, 0x6a429752b148ad30ull, 0xecb0e7af2c652ec5ull, DEST(fx.kMixerVolume), kSyxg50},
    {"fx::kVarInputClass", Elem::U8, Elem::U8, 128, 0x72b513e1c41bbb4bull, 0xc21f40c97592df6dull, 0x28a4e68d9ca32b23ull, DEST(fx.kVarInputClass), kSyxg50},
    {"fx::kFlangerLimits", Elem::U16, Elem::U16, 32, 0x99a1fdc32e2f0b21ull, 0xab3dbba49c934ab3ull, 0xdbdd97c3393240f7ull, DEST(fx.kFlangerLimits), kBoth},
    {"fx::kFlangerLimits (S-YXG50, clamped)", Elem::U16, Elem::U16, 32, 0x870bb9c2f2bb07e1ull, 0xad269c6b758e50c0ull, 0x61ed26eb16b46409ull, DEST(fx.kFlangerLimits), kBoth, clamp_flanger_delay},
};

} // namespace

} // namespace hvyxg::fx

std::span<const hvyxg::table_store::TableDesc> hvyxg::table_store::fx_tables() { return fx::kDescs; }
