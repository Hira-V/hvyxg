// Copyright (C) 2026 Kevin H. Patterson
// SPDX-License-Identifier: BSD-3-Clause (see LICENSE)
// Generated; do not edit. Descriptors only (element type, length and hashes of each table): the
// values are read from your DLL at runtime and verified by their XXH3-128 (dll/tables.hpp).
#include "syxg2006le/tbl/dll_tables.hpp"

#include "dll/tables.hpp"
#include "engine_tables.hpp"

#define DEST(m) [](EngineTables& t) -> void* { return t.m.data(); }

namespace hvyxg::syxg2006le::tbl::dll {

namespace {

using table_store::Elem;
using table_store::kSyxg2006le;

const table_store::TableDesc kDescs[] = {
    {"kAttackTime", Elem::F32, Elem::S32, 128, 0x9b4ba11936d0452dull, 0x4c404410510288bbull, 0x092e5d6331e6128bull, DEST(le.kAttackTime), kSyxg2006le},
    {"kAttackCap", Elem::U16, Elem::U8, 64, 0xe5dc8be9fcae6522ull, 0xaedab3970a87eed3ull, 0x2ac9b0f67b483615ull, DEST(le.kAttackCap), kSyxg2006le},
    {"kReleaseCap", Elem::U16, Elem::U8, 64, 0x8ae04a4be3ac9bacull, 0x4a643f2c262e3403ull, 0x9d67c8d9b7d54a0full, DEST(le.kReleaseCap), kSyxg2006le},
    {"kPegTimeCap", Elem::U8, Elem::U8, 64, 0xe5f20c69ff34975aull, 0x800ab8ee82dbfb25ull, 0x7a5f677dbd4df72aull, DEST(le.kPegTimeCap), kSyxg2006le},
    {"kFegRateLimit", Elem::U16, Elem::U8, 128, 0x10a4b6a8d1571470ull, 0x1a3963dbf86389bdull, 0x0bcbe30a936c261dull, DEST(le.kFegRateLimit), kSyxg2006le},
    {"kVibratoRate", Elem::S32, Elem::S16, 128, 0x10dad064c9d36788ull, 0x0926a2d34e5a183cull, 0xfac8f3509b4b0f44ull, DEST(le.kVibratoRate), kSyxg2006le},
    {"kVibratoDelay", Elem::U16, Elem::S16, 128, 0x21677acf5099c438ull, 0xc2bf97b056b34965ull, 0xea8be4f98e046dd1ull, DEST(le.kVibratoDelay), kSyxg2006le},
    {"kAttackCap6", Elem::U8, Elem::U8, 64, 0x0aba835c06481b48ull, 0x484c2f79e621929full, 0x428134050dee5d82ull, DEST(le.kAttackCap6), kSyxg2006le},
    {"kReleaseCap6", Elem::U8, Elem::U8, 128, 0x71993a118f4ea2c4ull, 0xfe136e9e89696305ull, 0x35f34c1388655aa9ull, DEST(le.kReleaseCap6), kSyxg2006le},
    {"kReleaseOffset6", Elem::U8, Elem::U8, 128, 0xe8d614e67839d523ull, 0x445a605619e64451ull, 0x038fcb8ebd0e7ec4ull, DEST(le.kReleaseOffset6), kSyxg2006le},
    {"kPortamentoTime", Elem::F32, Elem::F32, 128, 0xb7c9126980a0f451ull, 0x11eaaf7c9d0dae74ull, 0x071acab5d0853276ull, DEST(le.kPortamentoTime), kSyxg2006le},
    {"kLfoPhaseDegrees", Elem::S32, Elem::S16, 36, 0x7618e6a7e552e7d4ull, 0x56208331db54a2e7ull, 0xb255e981db8073a1ull, DEST(le.kLfoPhaseDegrees), kSyxg2006le},
    {"kAmpControl", Elem::S16, Elem::S16, 16384, 0xa1ce0acb95251361ull, 0xd914e62d8279aae9ull, 0xaf6d4e32ff0b4166ull, DEST(le.kAmpControl[0]), kSyxg2006le},
    {"kFilterCeiling[0]", Elem::U16, Elem::U16, 256, 0xe367db906fc522beull, 0x00c7b77be08b7b54ull, 0xc1ce993721366439ull, DEST(le.kFilterCeiling[0]), kSyxg2006le},
    {"kFilterCeiling[1]", Elem::U16, Elem::U16, 256, 0x5a92a008f97b924full, 0x42a1228785410af0ull, 0x1dc8ef8303157df4ull, DEST(le.kFilterCeiling[1]), kSyxg2006le},
    {"kDetuneKeyUp", Elem::U16, Elem::U16, 128, 0xc294f6bbfd0610faull, 0xa420e46331deee92ull, 0xd6ff5a795f96d3e9ull, DEST(le.kDetuneKeyUp), kSyxg2006le},
    {"kDetuneKeyDown", Elem::U16, Elem::U16, 128, 0xe03da3a297496aa2ull, 0xe2b58b8b0f3f65e7ull, 0xe5dc87aa88bd4498ull, DEST(le.kDetuneKeyDown), kSyxg2006le},
    {"kDecayTime", Elem::F32, Elem::S32, 128, 0x362fd697c1e229a1ull, 0x93aa4bd39492c2acull, 0xe23b74b0171aa6e2ull, DEST(le.kDecayTime), kSyxg2006le},
    {"kReleaseTime", Elem::F32, Elem::S32, 128, 0x59bb702bb438f62bull, 0xc05264ba2c5aa9c5ull, 0x516f48c7fe762bc7ull, DEST(le.kReleaseTime), kSyxg2006le},
    {"kVolumeAtt", Elem::U16, Elem::U16, 256, 0xeb1258516b377c28ull, 0x644fab9d9df780dcull, 0x367c327a015fc54aull, DEST(le.kVolumeAtt), kSyxg2006le},
    {"kSendAtt", Elem::U8, Elem::U8, 256, 0x621f81e90b333cbeull, 0xae9ec79192288ac9ull, 0xece65d6c1794b880ull, DEST(le.kSendAtt), kSyxg2006le},
    {"kDetune", Elem::S8, Elem::S8, 256, 0xdec8c681ae1bf53eull, 0xdf8fa22534fcbe8aull, 0xd83267ee3641016full, DEST(le.kDetune), kSyxg2006le},
    {"kFilterCeiling[2]", Elem::U16, Elem::U16, 256, 0xd0808c3f48306960ull, 0xdad65632e45127c4ull, 0x61241624d9c829f9ull, DEST(le.kFilterCeiling[2]), kSyxg2006le},
    {"kFilterCeiling[3]", Elem::U16, Elem::U16, 256, 0x9ff6376510d0be00ull, 0xcb489307dadc7677ull, 0xe4dc8a3391c6f21dull, DEST(le.kFilterCeiling[3]), kSyxg2006le},
    {"kPan", Elem::U8, Elem::U8, 129, 0x525aabce96a37fb3ull, 0xdee64d25aa0c2a3cull, 0x67d5969d40edfecbull, DEST(le.kPan), kSyxg2006le},
    {"kLfoSpeed", Elem::S32, Elem::S16, 94, 0xe49a0a5a6141fef3ull, 0xc3a7f9218fabef5full, 0xfb7000cfcdf90a49ull, DEST(le.kLfoSpeed), kSyxg2006le},
    {"kKeyTrackScale", Elem::U16, Elem::S32, 8, 0xc5edcc478e8af903ull, 0xfb14385dac49143full, 0x4d6b3ac592b2afe8ull, DEST(le.kKeyTrackScale), kSyxg2006le},
    {"kGsReverb", Elem::U8, Elem::U8, 152, 0x294e50335261868aull, 0x604d819cad733ca7ull, 0xcef998fe278c13c9ull, DEST(le.kGsReverb[0]), kSyxg2006le},
    {"kGsChorus", Elem::U8, Elem::U8, 152, 0x9e38d60c9581b846ull, 0xfbc58588d307610cull, 0x14ef9c385b9882b8ull, DEST(le.kGsChorus[0]), kSyxg2006le},
};

} // namespace

} // namespace hvyxg::syxg2006le::tbl::dll

std::span<const hvyxg::table_store::TableDesc> hvyxg::table_store::le_voice_tables() {
    return syxg2006le::tbl::dll::kDescs;
}
