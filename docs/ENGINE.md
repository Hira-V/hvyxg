# The sound engine (`src/`; public headers in `include/hvyxg/`)

The engine plays a Yamaha bank the way the Yamaha DLL that ships with it does:

| Bank | Files |
|---|---|
| S-YXG50 | `sxgbin41/21.tbl` + `sxgwave4/2.tbl` |
| S-YXG2006LE | `SXGBNW6L.TBL` + `SXGDAT6L.TBL` |

## Structure

```
MIDI front end ──actions──▶ hvyxg::Renderer ──▶ ToneModule::render(Buses&)      (voices)
   (XG RAM, parts)              fixed blocks      SystemEffects::process(Buses&)  (reverb, chorus, variation, mixer)
```

- `bank.hpp`: `Bank` — a loaded bank; makes its engine's tone module and system effects, and
  supplies the drum-setup RAM image of each kit. `load_bank` picks the engine from the header.
- `module.hpp`: `ToneModule`, `SystemEffects`, `ModuleContext`.
- `buses.hpp`: the per-block mix buses (dry L/R, mono reverb send, chorus L/R, variation L/R),
  in output units.
- `renderer.hpp`: runs the modules in the engine's block size (S-YXG50 128, 2006LE 64); MIDI
  actions take effect at block boundaries, as in the DLLs.
- `attenuation.hpp`: the attenuation → gain law both DLLs share.
- `eq.hpp`: superset EQs — the per-voice part / drum-note EQ and HPF (`VoiceEq`, run by both
  tone modules) and the multi EQ on the output (`MultiEqStage`, run by the renderer).
- `syxg50/`, `syxg2006le/`: one engine model each (bank access, voices, effects).
- `mu_rom.hpp`, `syxg50/mu_rom/`: MU ROM sets → an S-YXG50 MU bank (a port of SXG-Create-NF's
  converter, "MU ROMs" below).

The MIDI front end keeps the XG RAM, parts, controllers and SysEx handling. A
part records the program latched at the last program change; the tone
module resolves it against the bank at note-on, under the current reset mode.
Drum parts carry `hvyxg::kDrumBankMsbGs` (0x7B) for "the GS /
GM drum kit".

Drum-part rules shared by both engines:
- A part in a drum mode (part mode ≠ 0) always plays a kit. A program change with a melodic
  bank MSB (GS: CC#0 0 + program on the rhythm part, e.g. `moon_p.mid`) selects the mode's
  default kit bank with that program (measured on the 2006LE: channel 10 program 16 plays the
  Power kit).
- The drum part's default note assign (08 pp 06) is per engine and reset mode
  (`Bank::drum_part_key_assign(mode)`, applied at every reset): S-YXG50 1 = multi, 0 = single
  after a GS reset [code: part template, part reset; meas: voice FIFO
  logs]; S-YXG2006LE 1 = multi.
- Parts in mode 0 / 1 that play a kit keep their own drum setup, loaded at the program change
  and edited by their drum NRPNs (measured on the 2006LE).

## Fidelity policy

The goal is the DLL's sound, at the highest quality we can give it:

1. **Everything that defines the sound follows the DLL**: tables, level / pitch / filter laws,
   envelope rates and shapes, LFO rates and depths, controller ranges, routing, effect
   algorithms and coefficients. These are verified against renders of the real DLL and the
   results are recorded in tests or in the docs.
2. **Where the DLL degrades quality only to save CPU, we may do better**, provided the
   average behaviour stays the same. Each such choice is listed below. Examples: per-sample
   smoothing of values the DLL steps once per block; exact pitch instead of a 1/1024-octave
   grid; the scalar effect kernels' exact delay addressing instead of the SSE kernels' 4-sample
   rounding; higher-order sample interpolation (the interpolation option; `Linear` is the
   DLL).
3. When in doubt, follow the DLL.

## Deliberate deviations from the DLLs

| Area | DLL | Here | Why |
|---|---|---|---|
| Output polarity | S-YXG50 output is inverted (render = −wave) | positive | inaudible; simpler comparisons |
| Event timing | an event acts from the start of the block that contains it (S-YXG50 128, 2006LE 64 samples) | the same (`Synth` applies the messages of a block at its start); the S-YXG50 tone module runs its 10-ms ticks after that block's events, as the DLL does, so its control-layer timeline (tick-quantised key-on delays, EG steps) is the DLL's [meas: note onsets at offsets 0..255 land on the DLL's to within a sample] | none (a message that arrives after its block has started acts from the next block) |
| S-YXG2006LE distortion drive with `--2006full` | the distortion family (distortion, overdrive, amp simulator; variation and insertion blocks) clips at ±131071 bus units, +24 dB above full scale (+12 dB on the S-YXG50), so it is driven ≈ 12 dB less than on the S-YXG50 | default: the same; with `--2006full` the S-YXG50's clip level (±131071 · 2^-15 of full scale): organ chord relative to Thru, insertion connection −6.2 / −5.2 / −3.7 dB (S-YXG50 −7.8 / −6.7 / −5.2, 2006LE law +5.5 / +6.0 / +7.8), system connection wet only −4.6 / −3.5 / −2.1 dB (S-YXG50 −3.4 / −2.4 / −0.8, 2006LE law +7.2 / +8.1 / +9.7) | the 2006LE's variation is silent in its DLL, so its law was read from code, never heard; full XG makes the same song drive its distortion as on the S-YXG50. The 2006LE's other effect laws are unchanged |
| XG bulk dumps on the S-YXG2006LE | every XG bulk dump is ignored: multi part `08 nn`, system `00 00`, effect `02 01`, multi EQ `02 40`, drum setup `3n rr`, whole blocks or parts of them, any device number [meas: volume, master volume, master tune, note shift, chorus type and drum cutoff bulks change nothing, while the same data as parameter changes act and the S-YXG50 obeys the same bulk messages] | applied byte by byte, checksum checked | deliberate superset: ignoring bulk dumps is a DLL limitation, not part of its sound (the S-YXG50 accepts them) |

### S-YXG50 voices

| Area | DLL | Here | Why |
|---|---|---|---|
| Sample phase | 17.15 | 32.32 (same increments: the DLL's integer pitch register is kept, since its ±0.8-cent rounding sets the beat rate of detuned elements) | finer read position, inaudible |
| Sample interpolation | linear | linear (`Linear`), or 4-point Hermite with loop wrap for any other interpolation option | quality option |
| Dry right gain | the mixer ramps the right dry gain from the *previous* block's value to itself (reads twice): level/pan changes reach the right channel one block late | both channels ramp from the previous to the new gain | DLL bug, ≤ 2.9 ms lag |
| Output rates ≠ 44.1 kHz | rate tables scaled by 44100/fs; cutoff offset by a register term | rate tables scaled the same way; SVF coefficient recomputed for the same pole frequency in Hz | exact at any rate |
| SVF coefficient | unbounded (C ≤ 0xfff keeps f ≤ 1.002, but a full FMD sweep on top can exceed 2) | clamped to 1.99 | keeps the filter stable |
| Voice pool | 128 register sets taken from a FIFO of free indices (0, 1, … from power-on; freed voices to the tail), 256 physical chip voices bound at each key-on (round-robin cursor); a freed register set's chip voice plays on; the AEG monitor (every 4th tick, own cursor) frees a voice by a score threshold, reading the status of the physical voice the register set last keyed, which a later key-on may have taken; drum auto-release frees the register set early; cuts, steals and note-offs walk the part's voice list (allocation order) and the note's element ring | the same, with the configured polyphony (twice as many physical voices) [code; meas: the FIFO's take / free log of all ten test songs is identical to the DLL's] | config-driven polyphony |
| Mono cut | a mono note-on cuts (rate 0x5a) only the previous mono note's register sets: while keys are held or the sustain pedal is down its keyed voices on the last key, when no key is held its release tail | the same [meas: −110 … −122 dB on legato / detached / sustain / mixed sequences; before: every keyed voice of the part was released, −3 … −17 dB on the detached and mixed ones] | none |
| Resets and part kills | a reset (XG / GM / GM2 / GS On, GM Off) damps and frees every register set (its chip voice fades on), restarts the free-voice FIFO at 0, 1, … and sets every part's portamento source to key 60 with an empty mono key list; mono / poly, part-mode and variation `5A` / `5B` writes damp the part and do the same to its note state; CC 120 only damps | the same (`ToneModule50::reset`, `kill_all`, `sound_off`; `action::Reset`, `KillAll::sound_off`) [meas: a reset under a held chord with new notes after it −113 dB (before −23 dB); the first portamento note after a reset, after `5B` and CC 126 −116 … −120 dB (before +3 … +4 dB); CC 120 in mono mode then a note-off of the sounding key: the held key plays again, −117 dB (before −2.7 dB)] | none |
| Sustain pedal on released voices | pedal down: a released poly voice's release rate becomes at most its D2 rate; pedal up: the element's release rate again; written at the voice's level round-robin turn (`0x500` without bit 7) | the same [meas: first changed sample 21121 / 24577, exact] | none |
| Register timing | pitch and cutoff written only at the note-on and by the 10-ms tick, and by the tick only while a PEG / FEG stage or portamento moves or the voice's update flags are set (a control change, bend, aftertouch, XG part parameter, master volume or GM2 tuning of its part; not a note-on / off, program change or XG master tune); own-wave drum keys get their pitch / cutoff at the level round robin instead; level, pan, sends and LFO only at the note-on and at the voice's level round-robin turn (every 80 ms) when flagged; the tick's slots run on cursors from power-on | the same (`ToneModule50::tick`, `update`, `set_lfo`, `Part::touch`) [code; meas: the value sequences of the pitch, cutoff and glide-rate registers of all 128 voices are identical to the DLL's over all ten test songs and 36 FEG / CC74 probes] | none |
| Cutoff glide | counter 0xf0 for the note-on commit, then 0xc1; 0xc1 again at each FEG stage change; a controller change 0x81; the FEG + element cutoff is capped at 0xfff before the controller terms | the same; the FEG steps as the DLL (stages 0-2, 3 hold, 4 release, 5 done; equal-level stages skipped at the advance, release skipped only from the hold) | none |
| Kit on a normal part | a part in part mode 0 with an XG kit bank uses drum setup 2 (its drum NRPNs and its kit change edit it, a part in mode 3 shares it) | the same (`FrontEnd::update_program` / `drum_note`, `ToneModule50::drum_mode`) [meas: drum NRPN probes] | none |
| Steal score | `voice[0x6d] = 0xff − 2·v71` at the note-on (v71 = the velocity attenuation, slot `0x520`), refreshed every 40 ms by the AEG monitor; a note scores the highest of its elements, the lowest note goes, ties to the earlier one | the same | none |
| GS mode | own controller PMD table, part cutoff table, vibrato depth table and rule, per-program vibrato rate base / delay table / amp release offset, key-dependent detune (see *GS mode*) | the same; the detune key is taken as the note [inferred] | none (each law measured exact on single notes); GM mode keeps the XG laws (the DLL's GM mode value is not traced) |
| GS drum-setup RAM | absolute copy of the kit entry | XG-style image (pitch / filter / EG bytes are offsets onto the entry) in every mode | one image format for the shared RAM; same sound unless GS NRPN drum edits are sent |
| Insertion-variation return update | the insertion part's pan and sends reach the variation return targets when its voices are updated, before the tick of that block; a volume / expression / master volume change moves them at once with the pan and sends of the last voice update; then they slew by 8 per tick | the voice update is taken as "the block's variation input is non-silent" (the note-on block, then every block that sounds), the gain change as a change of the part's volume attenuation (`EngineModel::insertion_update_in_block`) [meas: register log; first notes on an insertion part −108 … −120 dB, before −18 … −31 dB; the insertion song's first type −110 dB, before −37 dB] | the per-voice level round-robin order of the updates is not modelled |
| Portamento | CC84 source key, continuation of a gliding voice, legato (same program + held key) keeps AR/D1R/D2R and velocity | glide from the previous note-on key of the part; fresh envelopes | not traced; a 60→72 glide (time 40) runs ≈ one tick behind the DLL |
| All notes off | under sustain only marks the notes for the pedal | `release_all` releases every note (interface contract: pedals included) | interface |

### S-YXG2006LE voices

| Area | DLL | Here | Why |
|---|---|---|---|
| Sample interpolation | linear, 10-bit weights `32·(frac >> 5)` | same for `Linear`; `Hermite` (4-point) and `Lanczos3` (6-tap) optional | interpolation option |
| Voice pool | 32 voice handles (voice-manager entries, a FIFO free list) driving a pool of 64 DSP objects: handle *i* starts on object *i*; any removal (steal, key assign, mono, all sound off, score-refresh kill) kills the handle's object — it fades out unmapped, or stops at once if it was in its key-on delay or had ended — and gives the handle the next free object from a round-robin cursor (starting at 32); no free object: the killed one stops and stays mapped. Objects keep their state between notes: a non-key-synced LFO phase runs on and gains the object's offset `index·65°` (a 32-bit product, so it wraps from object 32 on) at each note-on | the same, with the configured polyphony in handles and polyphony + max(polyphony, 32) objects (with a steal fade: polyphony + max(polyphony, 64)) [code; meas: the handle → object map logged from the DLL over a TrekMedG solo is identical] | configurable superset; set 32 to match the DLL; see *Superset: steal fade* |
| Event timing | an event acts from the start of the 64-sample block that contains it | the same [meas] | none |
| Controller transitions | bend, aftertouch, MW and AC1 / AC2 reach the voices at the 12-ms flush, which runs after the block's events and before the block renders (the sample count already includes the block); no smoothing of the sums: pitch at the next block, cutoff / level / AMod glide as any change, PMod / FMod enter the running LFO at once | the same (`ToneModule2006::controller_timer` before the voices; SVF coefficient ramp from the previous block's value at sample 0; LFO depths `(cents << 12) / 75` in integers, outputs truncated from double products) [meas: CC1 / AC1 / AC2 / CAT / bend × pitch / filter / amp / LFO PMod / FMod / AMod, steps, sweeps, early and late steps, four LFO phases, four voices: −95 … −122 dB, the steady-state residual of each case (before: up to +9 dB on pitch sweeps, −8 … −35 dB elsewhere)] | none |
| Voice stealing | a score refresh on every 10th controller flush (120 ms, before the block renders; on the flush's scheduler list): per handle, the level component's velocity term plus — outside the key-on delay and the attack — the peak of \|EG gain × sample\| since the last of its 4096-sample resets, in level units (255 once the sound has ended); above 240 (224 with every handle in use) the voice is removed at rate 0x70 unless its EG may still rise; otherwise stored with the part volume's attenuation, and a drum voice above 0x60 is marked released. A voice whose sound has ended keeps its handle until that refresh. The steal takes the keyed-off voice (not a drum key without note-off) with the highest non-zero stored score, ties to the lowest handle; once that fails within a note-on, the voice with the highest score, ties to the highest handle; new voices score 1 | the same (`ToneModule2006::refresh_scores`, `allocate`) [code; meas: the DLL's per-handle allocation, flags and stored scores logged per 64-frame host buffer are identical over the whole of TrekMedG; a steal probe pins the refresh between 118 and 121 ms]. A note-off reaches the voice when the block renders (a refresh in the same block sees it keyed on); a drum note-off goes to the key's oldest note only. Not modelled: the 64 note-object limit (reclaims a note without voices; it needs more than 32 voiceless held notes), record flags 0x01 / 0x02 (paired-voice elements, none in the bank) | none |
| All notes off (CC123–125) | normal key-off; sustain and sostenuto keep holding | releases pedal-held notes too | `ToneModule::release_all` contract (also used on MIDI disconnect) |
| Mono mode | note handler list as the held-key stack; last- or high-note priority (`0A nn 01`); re-trigger of the held key with the released note's velocity; legato EG takeover (note-on modes 5 / 6: the old voice's level read from the gain at the start of its last tick on the linear stages) and mono kill of the previous note and of every release tail; a mode write removes the part's notes | the same [meas: −96 … −127 dB on strings / piano / organ / saw lead over ~25 sequences; legato 50 … 1610 ms into attacks lengthened by CC73 = 80 / 100 / 127: −108 … −123 dB] | none. The CC126 / CC127 value checks and the removal on an unchanged mode write are in the front end (*Front end*: mono / poly messages) |
| Portamento | modes 1–6: from the last key, or from the matching previous voice's pitch key + its current glide offset (`pos·75 >> 12`); down glides from a voice take its element; PEG / FEG continue in modes 3–7; CC5 changes the rate of running glides; CC84 forces time 0, takes the source note's voices over within an octave (mode 7), else starts legato from them | the same [meas: −98 … −127 dB, incl. interrupted glides, CC5 sweeps, CC84 near / far / unsounding / after CC121] | none |
| Soft pedal (CC67) | level key / velocity curves and the cutoff curve × the part's soft-pedal depth at note-on; the velocity part joins the steal score | the same; the depth stays at its default 64 (only an internal message writes it) | none [meas: −94 … −126 dB] |
| EG changes on sounding notes | XG attack / decay (CC73 / 75, `08 nn 1A` / `1B`) re-derive the rates of the part's sounding voices from their pre-XG rates (attack; decays 1–3), except the stages a legato takeover fixed; XG release (CC72) and every sustain-pedal message re-derive the release rate of voices already keyed off (damper mode 3: the damper rate while the pedal is down) | the same (`ToneModule2006::xg_eg_changes`, `sustain_changed`) [code; meas: attack / decay / release changes during attack, decay and release on strings / piano / organ / saw lead / guitar, pedal down during a release: −112 … −129 dB (before: −2 … −57 dB)] | none |
| Filter at note-on | the filter is set up from the cutoff with the filter EG at its start level; the first block's coefficient ramps from there | the same [meas: note-on block of synth brass / pad / bass / lead / e.piano / guitar −108 … −139 dB (before: synth brass and pad −35 dB)] | harp (program 46) still −58 … −72 dB in its note-on block (every later block exact); cause not found |
| LFO fade-in | the fade factor is counter / fade, but the fade stage ends when the counter reaches the LFO *delay* length: longer delays overshoot the depth (×delay / fade), shorter ones jump to full depth, no delay means full depth after the 30 ms gate; times in 10 ms units are converted in x87 precision (80 ms → 3527 samples) | the same [code; meas: violin / synth strings / shakuhachi / goblin / soprano sax with MW steps / sweeps, XG vibrato depth and delay: −100 … −124 dB (before: up to −4 dB)] | none |
| Level controls `0A nn 40..45` | part parameters 0x72..0x77 (MW, pitch bend, CAT, PAT, AC1, AC2; default 64): each adds `kVolumeAtt[min(255, 2·x)]` to the amplitude slot after its clamp, x = ((d·T[v]) >> 6) + 0x80 for d = depth − 64 < 0, else ((d·T[v]) >> 6) + (64 − d)·2 (d 63 counts as 64), v the source value (bend: \|bend\| >> 6; PAT per key 0x24..0x61 on normal parts), capped at 0x101: 64 nothing, 127 the level follows the source (0 = silence), 0 the inverse | the same (`syxg2006le/controllers.cpp`, `MultiPartExt::level_control`), also in DLL-exact mode [code; meas: depths 0 / 32 / 96 / 127 against source 0 / 127 for each of the six, organ note: every segment within 0.1 dB of the DLL (silence, −11.8 / −12.0 dB, unchanged)] | none. `0A nn 00` and `0A nn 10` (part parameters 0x6e / 0x70) change nothing audible (poly and mono probes) and are not stored |
| Amp EG states 10–12 | damper modes 1/2 continue attack/decay after note-off | not modelled | the bank only uses damper modes 0 and 3 |
| Held-time curve (block C 0x1c) | indexed by note-event field (unknown) | index 0 | unused by the bank |
| Part / drum 2-band EQ | per-voice shelving EQ computed, never audible [meas] | the superset part / drum-note EQ (see "Superset: EQ and HPF") | the DLL renders bit-identical output with any part / drum EQ |
| Filter flag path | `2^(23−e)` coefficients capped at 1.75 when the filter's flag is set | not modelled | flag source not found; never set at 44.1 / 48 kHz in the measured renders |
| Voice type 6 (MSB 118 / 119) | alternate component classes; amp-EG XG adjusters with their own tables; pitch class: drum coarse counted from the pitch key 60, part detune scaled by key (tables) | the amp-EG adjusters, the drum coarse and the key-scaled detune (`(index − 0x80)·T[ev[1]] / 4096` cents) are modelled; the other alternate classes (filter, LFO) use the type-0 code | the measured type-6 piano / strings voices and GS kits match exactly; detune measured on MSB 119 strings and an MSB 118 kit at five / six keys (−111 … −136 dB). Type-6 drum notes take the detune at ev[1] = 60, which matches the measured kit; the DLL reads the key through drum-setup parameter 0, not traced |
| Random pan | `r = rand & 0x7f` from a generator object passed to the pan component (not traced); attenuations `P[r] + 16` / `P[128 − r] + 16` | same law, r from this engine's LFSR model | the DLL's random sequence is not reproduced, so random-pan notes differ note by note (same distribution of laws) |
| Sample formats | 8/12/16/24/32-bit PCM and DPCM | 16-bit PCM | the only format in SXGBNW6L |
| Variation routing | variation send only in routing mode 0, which needs a module byte that is never set; insertion outputs gated off (variation never audible) | system connection: part dry level and variation send (mode 0 as coded); insertion connection: S-YXG50 routing — the insertion part goes to the variation only, at the dry output's attenuation (8 units), without part volume / expression / master and part pan, which the return applies with this engine's volume law; drum keys with variation send 0 bypass | superset: a Thru insertion keeps the part's level |

### System effects

| Area | DLL | Here | Why |
|---|---|---|---|
| Delay addressing | SSE kernels round every reverb / chorus address down to a multiple of 4 (delays off by up to 3 samples) | the plain-C kernels' exact addresses (identical to the DLLs' own scalar kernels, which `Process=1` / no-SSE selects) | allowed: SSE artefact |
| Arithmetic | x87 extended precision inside the kernels | float (≈ 1e-7 relative difference) | inaudible |
| Sample rate | effect control works in 44.1 kHz samples, the DSP scales addresses by fs/44100 (measured at 48 kHz: identical to here); per-rate tables only for 11.025 … 96 kHz (filter tan, fade pole) | addresses scaled the same way at any rate; filters from tan(πf/fs), fade pole (504/512)^(44100/fs), mixer ticks and sleep gate in seconds | any output rate, not just the DLLs' five |
| Mixer tick | 10 ms (S-YXG50) / ≈ 12 ms (2006LE) timer tested once per block | sample-counted, same average rate | — |
| S-YXG50 SysEx granularity | a type changes only with MSB + LSB in one message; a multi-byte parameter message sets only its first parameter | every effect RAM byte counts | the effects read the XG RAM, which stores every byte |
| Variation kernels | SSE kernels run: addresses rounded down to multiples of 4, the delay / chorus lines alias (one mono line), LFOs held 4 samples (ER 2 also reads in a different order) | the plain-C kernels (`Process=1`), exact addresses | allowed: SSE artefact; per type within −41 … −53 dB of a `Process=1` render |
| Variation, S-YXG2006LE | system connection clears the variation flag, insertion swaps in a no-op kernel; the DSP multiplies the return and send words (3, 6, 0xf, 0x14, 0x15, 0x18) by 0 | audible: the 2006LE's effect control and kernels (identical to syxg50.dll's, checked against its scalar kernels), the mixer words as its engine computes them, scaled ×0.5 (return) / ×0.3536 (to reverb / chorus) like the chorus words in the same DSP step | superset (hvyxg is a superset of the 2006LE); with ×1 the variation sat ≈ 7 dB louder relative to dry than on the S-YXG50 (Nes_Netopia: −4.8 vs −11.7 dB), with the chorus scaling −10.8 dB |
| Delay high damp at fs ≠ 44.1 kHz | the effect control sets the feedback FIR to {1, 0} (no damping) | the 44.1 kHz FIR at every rate | DLL degradation (all other variation control works in 44.1 kHz terms) |
| Variation symphonic / rotary | x87 extended precision in the slow LFO smoothers | float / double | within 0.35 % / 0.09 % of peak (correlation ≥ 0.999998) of the S-YXG50 plain-C kernels |
| Sleep gate | unit stops after its output stays below 1 bus unit (S-YXG50 bus · master) | 2^-15 / 2^-13 in bus units | CPU only |
| Insertion-variation control depths (`02 01 5C–60`) | S-YXG50: per 10-ms tick, offset slewed by 4 per tick, bend term `(bend14 − 32)·d/32` (saturates at a centred bend), 16-bit sum, out-of-range values rejected by the effect control; 2006LE: in the block of the controller event, no slew, bend `(bend14 − 8192)·d/8192`, offset scaled by the parameter's range (see "Control depths") | the same per engine; CBC1 / CBC2 (`61`, `62`) ignored as in both DLLs and the MU2000 | none (S-YXG50 renders match to −86 … −119 dB; 2006LE control words exact) |

## Variation and insertion effects

**Variation** (`02 01 40…`, `fx_variation.*`, `fx_control_variation.cpp`, `fx_control_var_*.cpp`,
`fx_var_*.cpp`). Both DLLs contain the same variation control, tables and plain-C kernels
(the tables are the same in both; only the parameter limits differ and are kept
per engine). One implementation serves both engines:

- `EffectControl` (the 2006LE controller code, block 2): type map (`variation_type`),
  per-category programs, the type's defaults (also loaded into the XG RAM on a type write), the
  per-type parameter handlers, the full filter designer (orders 1 and 2), the connection (mixer
  parameter 9: input-gain words, dry/wet 127 in system connection).
- `VariationUnit`: decodes the words with the category's formats, derives the kernel values the
  plain-C kernels read (lane 0 and the LFO integers), and runs one of 14 kernels
  in place on the variation pair (131072-float ring, exact addresses).
- `SystemFx`: the engine side: input gain −2.83 (system) / −1.0 (insertion) ramped from 0 after a type
  write or any connection write (S-YXG50 also of the same value; the connection write re-sends
  the type, clearing the ring, and restarts the control offset); an insertion-part write does none
  of this, only the return follows the new part at once (a part of 7F keeps the old return on
  the variation's tail); return / pan and variation → reverb / → chorus words
  (`att[level] + offset + pan`, offsets 0x18 / 0x25, or 0x15 with a stereo chorus), slewed by
  the send step per tick; in insertion connection the return, pan and sends come from the
  insertion part (master × volume × expression, part pan, part reverb / chorus sends) and the
  control depths modulate the type's controlled parameter. Post-mix and order as the DLLs:
  input gains of all units, then variation (into dry, reverb input, chorus pair), chorus,
  reverb. A type change keeps the unit running (its LFO starts at the change) and its fade.
- Voices (both tone modules): system connection: part dry level and variation send (S-YXG50:
  plus the centre-pan term for mono-input types, `variation_stereo_input`); insertion
  connection: other parts dry 127 and no variation send, the insertion part 100 % to the
  variation at unity part gain and without part pan, drum keys with variation send 0 bypass.
- S-YXG50 front end (measured): a write of
  `02 01 5A` silences the voices of the insertion part, a write of `02 01 5B` those of the old
  and of the new insertion part (parts below 16), as all sound off does (a linear fade over
  the block of the write, exactly 0 after it), also when the value does not change. Other
  parts keep sounding; new notes play normally. The variation keeps running (its tail goes on
  at the new part's return). The 2006LE silences nothing (measured: its insertion part plays
  on), so hvyxg's 2006LE re-routes the sounding voices instead.

Measured against syxg50.dll (S-YXG50 bank):

| Check | Result |
|---|---|
| Kernels, noise into buses 6/7, `Process=1`, every type and parameter variants | words / addresses identical; output ≤ 1e-5 of peak (symphonic 3.5e-3, rotary 8.8e-4) |
| `SystemFx` (returns, sends through reverb / chorus, ramps, fades, type change while running) | ≤ 1.1e-5 of peak, 9 cases |
| Song, every type in system connection (43 types, piano phrase, `Process=1` render) | 41 types: correlation ≥ 0.99997, −41 … −53 dB error, level ±0.04 dB |
| Song, every type in insertion connection on part 1 (+ a dry part) | 40 types: ≥ 0.99998, −40 … −53 dB |
| Organ chord on the inserted part alone, all 43 types (`Process=1`) | correlation 1.00000 (Hall 1, the first segment after the reset: 0.99988), −60 … −127 dB (symphonic −60, rotary −98), level ±0.00 dB |
| Rotary in insertion connection: every parameter at 0 / 40 / 127, the five control sources at depths 0 / 96 / 127 and bases 0 / 64 / 127, type changes into rotary under a held chord, organ programs 16–20, no XG reset / no connection write | correlation 1.00000, −73 … −115 dB, level ±0.00 dB (SSE DLL: 0.93 … 0.99, its address rounding) |
| Writes of `02 01 5A` / `5B` under held notes and a ringing delay tail (17 probes: part changes, re-sent values, part 7F, connection changes both ways, notes after the write) | −48 … −128 dB (reverb on: −48 … −64 dB, its ramp) |
| Song, drum part inserted | ±0.3 dB, correlation ≥ 0.995: the drum voices' own accuracy (same without variation) |
| Control depths (MW, PB, CAT, AC1, AC2 × distortion, overdrive, tremolo, auto pan, auto wah, delay, chorus, phaser, hall, flanger, symphonic; step changes and 3-ms sweeps; held controllers across type / parameter / depth changes) | correlation 1.00000, −86 … −119 dB error (symphonic −63 dB), level ±0.00 dB (see "Control depths") |

Phaser 2 and Karaoke 2 diverge from a note-on onwards deep into the long test songs (correlation
0.84 / 0.97); rendered after a shorter history the same type changes match (0.99997), so the
cause is history-dependent and not in the variation (not found). Against the default SSE DLL the
plain-C kernels differ by the SSE address rounding (as for reverb / chorus).

Against S-YXG2006LE.DLL (never audible): every type's control image and its scalar kernel's
impulse response (92 cases incl. parameter variants and both connections) match
to ≤ 4.5e-6 of peak (most ≤ 1e-7). Levels: the 2006LE voice sends are 1.40× the dry level, so
a system variation is ≈ 3 dB louder relative to the dry signal than on the S-YXG50 (the DLL
never shows what it intended).

**Insertion effects (XG2.0 `03 nn`, superset, `fx_insertion.*`).** Neither DLL makes them
audible (the S-YXG50 does not parse them; the 2006LE parses them, inert). hvyxg routes each part
with insertion blocks (part < 64, type ≠ Thru / No Effect) through a chain: its voices render
their dry pair at unity part gain and without part pan into a chain pre-bus (`Buses`: ≤ 8 chains,
part → chain map; nothing else is cleared or run when no block is assigned); the blocks run in
block-number order (≤ 4 per part); the result takes the part's volume / expression / master (the
voice law), pan, dry level (system connection) and reverb / chorus / variation sends, as the
XG2.0 routing chart shows. A part that is also the variation insertion part feeds the variation
at unity. Variation-equivalent types run the DLLs' variation control and kernels in insertion
connection (dry/wet respected, wet halved), each block with its own `EffectControl` + `VariationUnit`
(created with the synth, so rendering never allocates). Compressor (53), Noise
Gate (54), Ring Modulator (71) and Lo-Fi (5E) are hvyxg designs (peak detector + static curve,
gate with attack/release poles, sine carrier + LFO + filters, sample-and-hold + word length +
resonant low-pass); every other XG2.0-only type (pitch change, harmony, tempo, dynamic …) is
Thru. A type write reloads the DLL defaults; an XG RAM reset resets all blocks to Distortion, part
off. The block control depths `03 nn 0D–11` drive the type's controlled parameter as on the MU2000
(see "Control depths"). **Against the MU2000** (S-MU2000, MU2000 bank on both sides, organ chord on part 1; the voices
differ, so each renderer's change from the same probe without the block is compared). No song in
the corpus uses `03 nn` (none sends part EQ, `0A nn` or drum EQ either; one sends a multi EQ), so
these routing probes stand in for songs:

| Probe (block on part 1) | MU2000 | hvyxg |
|---|---|---|
| distortion: level / bands 50–200 Hz, 0.2–0.8, 0.8–3.2, 3.2–12.8 kHz | −3.7 dB / −4.8, −5.1, +0.3, +20.5 | −4.6 dB / −5.1, −7.0, −0.9, +19.2 |
| 3-band EQ (defaults) | +4.4 dB | +3.5 dB |
| reverb return of the block's output (rev 127, change with the block) | follows the block (EQ +3.6, distortion −4.2 dB) | follows the block (+2.8, −5.0 dB) |
| chorus return, distortion | −4.3 dB | −4.6 dB |
| CC7 = 40 / CC11 = 40 after the block | −15.5 / −20.3 dB, shape unchanged | −15.7 / −20.0 dB |
| CC10 = 96 / 20 (L − R) | −8.2 / +12.4 dB | −7.9 / +12.4 dB |
| CC10 = 0 | left only | left only (was centre: fixed, below) |
| two blocks EQ → distortion vs distortion → EQ; block 2 alone | order matters as in hvyxg; block 2 = block 1 | the same |
| distortion on the drum part | +12.3 dB | +10.2 dB |

Fixed: a part pan of 0 (CC10 = 0) was taken as "random" and centred the chain output; it is
hard left now on a melodic part (as its voices are panned), centre on a drum part. The other
differences are the effect algorithms' (the DLL kernels against the MU2000's DSP), not the
routing: the sends take the block's output, volume / expression / pan apply after it, blocks run
in number order.

CBC1 / CBC2 depths `03 nn 12–13` are stored and act on nothing, as on the MU2000
(*Control depths*). Not implemented: per-key drum sends of a chained part, an output fade on
insertion units.

### Insertion effects

| Area | DLL | Here | Why |
|---|---|---|---|
| XG2.0 insertion | S-YXG50 not parsed; 2006LE parsed, inert | chains as above | superset |
| Part volume / pan with an insertion | — | applied after the effect with the voice law | XG2.0 chart; a Thru / flat-EQ block is level-neutral within 0.3 dB |
| XG2.0-only types | — | four own designs, the rest Thru | no reference exists |
| LSB variants without a DLL entry | — | the base type | the DLLs' type map does the same |
| Drum keys of a chained part | — | per-key sends ignored | one chain per part |
| Block control depths (`03 nn 0D–11`) | — | MU2000 law (measured with S-MU2000), at once, kept on the parameter | superset; CBC1 / CBC2 (`12`, `13`) ignored, as on the MU2000 |

### Control depths

The part's MW, pitch bend, CAT, AC1 and AC2 move one parameter of the effect type (variation
`02 01 5C–60` in insertion connection, XG2.0 insertion blocks `03 nn 0D–11`). Code:
`control_offset`, `controlled_parameter`, `scale_control` in `fx_system.*`;
`SystemFx::variation_control`; `InsertionFx::apply_control`.

- **Offset** = Σ trunc((depth − 64) · source / 64) over MW, CAT, AC1, AC2 (the part's AC1 / AC2
  values, `Controller::ac`; see *Front end*), plus the bend term, clipped to ±127.
  - S-YXG50 bend term: trunc((bend14 − 32) · d / 32). It subtracts the centre of a
    6-bit bend from the 14-bit value, so a centred bend at a positive depth already drives the
    offset to +127 (and to −127 at a negative depth). The 32-bit sum is compared on its low 16 bits.
  - 2006LE and MU2000 bend term: trunc((bend14 − 8192) · d / 8192).
- **Controlled parameter** (also measured on the 2006LE's control words
  and on the MU2000):
  - parameter 1 for MSB 45–47 and 49–4B;
  - parameter 3 for the auto wah (4E);
  - none for 00;
  - dry/wet (parameter 10) for every other type. The 3-band and 2-band EQs, whose limit table
    has no dry/wet, do not respond. The flanger and the symphonic do respond on dry/wet
    (1..127) in both DLLs (measured on syxg50.dll; an earlier note here said they did not).
  - MU2000 XG2.0 types: ring modulator → frequency coarse (parameter 1), Lo-Fi → dry/wet,
    compressor and noise gate → none.
  - The MU2000 insertion blocks control the same parameter as the DLL variation for every
    variation-equivalent type (S-MU2000, a control render to the parameter's limit against a
    render writing the limit, for each of the 42 types and each of its parameters: −70 …
    −300 dB on the controlled parameter, the LFO types −35 … −53 dB, all other parameters
    ≥ −22 dB): dry/wet for the reverbs, delays, early reflections, gates, karaoke, chorus,
    celeste, flanger, symphonic and phaser; parameter 1 for rotary, tremolo, auto pan,
    distortion, overdrive and amp simulator; parameter 3 for the auto wah; none for the EQs.
    The flanger / symphonic dry/wet law is the chorus's (base + offset · 126 / 127, bit-exact
    on S-MU2000 at 8 test points).
- **S-YXG50** (measured):
  - The offset is evaluated on the 10-ms control tick and moves toward its target by 4 per tick.
  - The value sent is the RAM low byte + offset, clipped to 0..127, and it is sent only when the
    offset changes. The effect control rejects a value outside the parameter's range, so the
    parameter keeps the last value it accepted: dry/wet 64 driven down stops at 4, not 1
    (measured: flanger, symphonic, chorus, −90 … −122 dB).
  - A connection write (any, also of the same value) resets the offset to 0; an insertion-part
    write does not (measured). A parameter write sends the raw RAM value; the offset comes
    back at the next offset change.
- **S-YXG2006LE** (control words dumped after every block):
  - The offset applies in the block of the controller or depth event, with no slew.
  - It is scaled to the parameter's range: offset · (hi − lo) / 127, rounded toward zero, so a
    1..127 dry/wet moves by one less.
  - The base is the RAM value. A parameter write sends the raw value until the offset changes,
    and a type change re-applies the offset.
- **MU2000** (S-MU2000 with the genuine firmware: a render with the control against renders that
  write the parameter instead, matched to −65 … −72 dB, the emulator's noise floor between two
  SysEx timings):
  - Same law and scaling as the 2006LE, applied in the same block as a parameter write would be,
    with no slew.
  - The offset stays on the parameter through later parameter writes and type changes: effective
    value = RAM + offset.
  - hvyxg's insertion blocks do exactly this on both engines (superset). The ring modulator
    matches by spectrum, not by waveform, because its carrier phase restarts differently.

**CBC1 / CBC2** (`03 nn 12–13`, `02 01 61–62`) act on nothing, in both DLLs (they keep the
variation's bytes and never read them) and on the MU2000: its owner's manual lists only the
MW, bend, CAT, AC1 and AC2 control depths (insertion `0D–11`, variation `5C–60`; no `12–13` /
`61–62` rows), and S-MU2000 renders confirm it. Probe: a distortion insertion block and a
distortion variation in insertion connection on an organ chord, every CC 1…95 except volume,
pan, expression, data entry, pedals, portamento control and the effect sends, then CAT and bend,
swept to their maxima and back; CBC1 / CBC2 = 7F / 40, 40 / 00, 00 / 7F render bit-identical to
40 / 40 (insertion block and variation alike), while AC1 depth 7F in the same probe changes the
output (−22 dB). hvyxg stores them and ignores them (`tests/fx_control_depth_test.cpp`,
`fx_control_cbc_depths_do_nothing`).

**Is the S-YXG50 tick schedule a significant gap? No.** hvyxg already ran the variation control on
the DLL's tick, after the block's events. Probes (organ chord on the inserted part):
- method: step changes at odd offsets and 3-ms sweeps up and down for each of MW / PB / CAT /
  AC1 / AC2 at depth 127, across distortion, overdrive, tremolo, auto pan, auto wah, delay LCR,
  chorus, flanger, phaser, hall and symphonic (55 cases);
- result: every case matches the `Process=1` render at correlation 1.00000, −86 … −119 dB error,
  0.00 dB level difference (symphonic −63.5 dB, its x87 LFO). There is no tick or phase offset.

The real gap was the bend term. hvyxg used (bend >> 8 − 32), so it saw no offset at a centred
bend while the DLL saturated:
- distortion: correlation 0.9987, windows up to −7 dB;
- tremolo: 0.964;
- auto wah: 0.21, +23 dB;
- delay: 0.61.
With the DLL's term these probes are exact (−93 … −119 dB).

A scenario probe also matches (−47 dB overall, the rest exact): MW held through a type change,
a parameter write, depth changes, reset all controllers, bend depth 0 with the 16-bit wrap, and
switches to rotary and auto wah. The one remaining gap came from the front end and is resolved:
the DLL keeps the AC1 / AC2 value received on the old CC number when the AC number changes (part
updated only by a CC that matches the current number); hvyxg read the last value of
the new CC and now keeps a per-part AC value (*Front end*).

Found while probing, since resolved:
- an insertion-part write (`02 01 5B`) while a part sounds: the DLL silences the old and the
  new insertion part (all sound off), also a connection write the insertion part; see
  "Variation and insertion effects". Implemented; the part write no longer restarts the
  variation input, ring or control offset in hvyxg, and a connection write of the same value
  now does.
- rotary (45) in insertion connection with an organ chord, "≈ 9 dB louder than the DLL": not
  reproducible on the current code (see the rotary row of the table above: every parameter,
  control source and type-change case matches to ≤ −73 dB at ±0.00 dB). The probe that saw it
  is not available; the part / connection writes above (which hvyxg handled differently until
  now) are the only difference found, and they now match.
- the earlier figures in this document (0.983 / 0.9997 / 0.992) predate the bend fix and the
  current probes.

## Superset: steal fade (`steal_fade.hpp`)

`Synth::Options::steal_fade_ms` (`render_yxg --steal-fade MS`), default 0.

- **0 (default):** the modelled DLL's steal: the S-YXG50 damps the stolen voice's chip voice
  (`kDamp`) and lets it end on its fading slot; the 2006LE moves it to its 64-slot dying pool
  at release rate 0x7E. Either way the stolen sound is gone in ≈ 1.5 ms, which can click on
  sustained sounds.
- **Above 0:** the stolen sound keeps playing (its envelope continues) and is faded linearly
  to silence over that time, after the voice filter and EQ and before gain / pan / sends;
  then the fading slot ends. The S-YXG50 already has one fading slot per voice; the 2006LE's
  dying pool becomes max(polyphony, 64), since longer fades overlap more. All sound off,
  resets and every other note cut keep the DLL behaviour.

Only steals are affected, so songs that never exceed the polyphony render bit-identically.
Measured with 20 ms: no change on the S-YXG50 test songs (128 voices, no steals); on
the 2006LE (32 voices) dense songs steal constantly — TrekMedG changes in 84 % of its 64-frame
blocks, Full_Memory 74 %, Be_with_me 24 % (largely through the reverb / chorus tails of the
longer fades). Tests: `tests/steal_fade_test.cpp`.

## Superset: EQ and HPF (`eq.*`)

**Neither DLL renders them [meas].** A sustained saw-lead chord (part 1) and a drum phrase (part
10) were rendered through both DLLs (effects sends 0) with and without:
part EQ `08 00 72 4C / 76 28`, `73 34 / 77 1C`, `72 4C` alone; multi EQ type 1 and 3, band 1 +12
with band 5 −12, band 3 +12; part HPF `0A 00 20 7F`; `08 00 74 / 75 7F`; drum-setup EQ
(`30 rr 20 4C`, `21 34`) and HPF (`30 rr 50 7F`); and the XG2.0 drum NRPNs `30 / 31 / 24 rr`.
Every render is **bit-identical** to its reference in both DLLs, while the controls `08 00 0B`
(part volume) and `30 26 02` (drum level) change them. The 2006LE has a per-note EQ component
(0x1c4) that reads the part / drum EQ, but nothing
reaches the output. So hvyxg implements all of it as superset behaviour, after the XG
specification for addresses / ranges / defaults and the **MU2000** (S-MU2000 running the genuine
effect control) for shapes and presets. At the defaults nothing is processed, so
every existing DLL match is unchanged (bit-identical, below).

**Multi EQ (`02 40 00..14`), `MultiEqStage`, on the final stereo output after the effect
returns** (`Renderer`, every block):

| Item | Here | Source |
|---|---|---|
| Type `00` | 0 flat, 1 jazz, 2 pops, 3 rock, 4 classic; a write loads the five bands' gain / frequency / Q and sets both shapes to shelf | measured: each type written to the MU2000 firmware in S-MU2000 and the bands read back, `MultiEq::kTypePresets` |
| Gain | `0x34..0x4C` = −12..+12 dB | XG |
| Frequency | the XG EQ table (20 Hz…20 kHz, 61 steps); band 1 4..40, bands 2–4 14..54, band 5 28..58 | XG |
| Q | 1..120 = 0.1..12.0; RBJ peaking with `Q_rbj = Q / max(A, 1/A)` (`A = 10^(dB/40)`): the Q is the bandwidth of the boost, and a cut is the exact inverse of the boost | fitted to MU2000 renders: Q 0.1 / 0.7 / 1.0 / 2.0 / 12, ±6 / ±12 dB, rms 0.01–0.25 dB |
| Band 1 shelf | first-order low shelf, pole at the band frequency (boost), cut = inverse | MU2000 fit: 200 Hz / 1 kHz, ±12 dB, rms ≤ 0.1 dB |
| Band 5 shelf | two first-order high shelves of half the gain each (pole at the band frequency), one biquad | MU2000 fit: 2 / 8 kHz, ±12 dB, rms ≤ 0.03 dB |
| Defaults | flat: gains 0, 80 Hz / 500 Hz / 1 kHz / 4 kHz / 8 kHz, Q 0.7, shelves | MU2000 RAM after XG System On |
| Reset | every reset (XG / GM / GM2 / GS) restores the flat EQ | MU2000 (XG / GM On) |

hvyxg vs the MU2000 (S-YXG50 bank, same probes, response at the chord's partials 120 Hz–14 kHz):
every single-band case within 0.01–0.23 dB max, Jazz 0.20 dB, Rock 0.59 dB max (rms ≤ 0.09 dB).
Flat bands are skipped; with all five flat the stage does nothing. Coefficients are redesigned
only when the 21 bytes change (no allocation); a change acts from the next block and is smoothed
(below).

**Part EQ (`08 nn 72/73/76/77`) and part HPF (`0A nn 20`), `VoiceEq`, per voice** after the
voice filter and before the pan / output gains, so the EQ'd signal feeds the dry output and every
send (and an insertion chain). One biquad for the two shelves, one for the HPF; nothing runs while
both are flat. Melodic voices follow the part RAM live; drum notes latch theirs at note-on.

| Item | Here | Source |
|---|---|---|
| Bass / treble gain | `0x34..0x4C` = −12..+12 dB, first-order low / high shelf, pole at the band frequency (boost), cut = inverse — the shape of the 2006LE effect EQ designer (without its 1/128 quantisation or ×√2) | XG ranges; shape fitted to the MU2000 (corner within 2 % of the nominal frequency at 80 Hz, 200 Hz, 500 Hz, 2 kHz, 10 kHz, rms ≤ 0.02 dB) |
| Bass / treble frequency | XG EQ table, bass 4..40 (32 Hz–2 kHz, default 80 Hz), treble 28..58 (500 Hz–16 kHz, default 10 kHz) | XG |
| HPF cutoff | `0x40` (default) and below: off (neither bank has a voice HPF to lower); above: 12 dB/oct RBJ high-pass, Q 1.0, `fc = 29.2 Hz · 2^((v − 64) · 24/256)` (0x50 → 83 Hz, 0x60 → 234 Hz, 0x7F → 1.75 kHz) | MU2000: the HPF adds 24 filter-register units per step (measured on the MU2000 firmware in S-MU2000); measured 0x50 / 0x60 / 0x7F → 83 / 225 / 1811 Hz, Q 1.0–1.2 |
| Drum notes | the part EQ does **not** apply; the drum-setup note EQ (`3n rr 20/21/24/25`) does; HPF = part + note − 64 | MU2000 firmware (measured in S-MU2000) |
| 2006LE kit entries without XG2.0 data | 1708 entries hold 0 in the EQ / HPF bytes: gains outside `0x34..0x4C` are 0 dB, frequencies outside their range the defaults, and an entry with bass frequency and HPF 0 has no note HPF (a written HPF, SysEx or NRPN, applies; no bank entry has bass frequency 0 with an HPF) | own rule (keeps those kits flat) |
| NRPNs | part `01 30 / 31 / 34 / 35` (EQ bass / treble gain, bass / treble frequency) and `01 24` (part HPF); drum `30 / 31 / 34 / 35 rr` (note EQ) and `24 rr` (note HPF), with data entry and increment / decrement; ignored in DLL-exact mode | MU2000 (S-MU2000, a drum and a saw-chord probe: each NRPN changes the spectrum exactly as the SysEx it stands for, ≤ 0.1 dB per band; the MU128 / MU2000 NRPN tables list the same numbers); the 2006LE's own NRPN map holds `01 24 / 30 / 31 / 34 / 35` too (inaudible there) |
| Changes during playback | a change of a running part EQ / HPF (melodic voices follow the RAM live) or of the multi EQ, or one that switches it on or off, crossfades from the old to the new filter over 10 ms (`eq::kChangeFadeSeconds`), both running on the same input; a voice's first settings and the multi EQ's first setting apply at once | own design: coefficient steps in a biquad click and zipper; a crossfade cannot go unstable. Renders without EQ changes are bit-identical |

**Gain scale: XG nominal by default; the MU2000's as an option.** The MU2000's part EQ is far
weaker than its nominal range. By default hvyxg follows the XG specification's ±12 dB (as the
multi EQ does on the MU2000) and keeps only the MU2000's shape. `Synth::Options::mu_part_eq`
(`render_yxg --mu-part-eq`, off by default, ignored with `dll_exact`, any bank) plays the part
EQ and the drum-note EQ with the MU2000's strength instead (`eq::shelf_db`): the gain acts in
steps of four values, n = ⌊(value − 64) / 4⌋:

| Gain values | −12 … −9 | −8 … −5 | −4 … −1 | 0 … +3 | +4 … +7 | +8 … +11 | +12 |
|---|---|---|---|---|---|---|---|
| Shelf | −2.095 dB | −1.334 dB | −0.573 dB | 0 (flat, nothing runs) | +0.762 dB | +1.526 dB | +2.286 dB |

Measured on S-MU2000 (a saw chord on part 1, sends 0, each render's
spectrum at the chord's partials against a flat render, the shelf gain fitted with the shelf
shape above, rms 0.002–0.005 dB): every value 0x34…0x4C of the bass and the treble band at
1 kHz; ±12 / ±6 with the bass band at 80 Hz, 200 Hz, 500 Hz, 2 kHz and the treble band at 2, 4,
8, 16 kHz (within ±0.04 dB of the table: no dependence on the band frequency; the part EQ has no
Q); and the drum-note EQ (`30 rr 20 / 21`, five keys of the Standard Kit, ±12, ±6, −1, +1, +4:
the same values). The shape (corner at the nominal frequency) is unchanged. hvyxg with the
option, MU bank, the same probes: +2.29 / −2.09 / +0.76 / −0.57 / 0 (bass), +2.29 / −2.10 /
+1.53 / −1.33 (treble), 80 Hz / 16 kHz bands and drum notes alike (fit rms ≤ 0.005 dB).

**Transparency.** With the defaults (flat multi EQ, part EQ gains 0x40, HPF 0x40, drum EQ
flat), the ten test songs render **bit-identical** (max |difference| 0.0) before and after
this feature, on both banks.

**Other `0A nn` part parameters.** The MU2000 accepts no other: its state changes only for
`0A 00 20` when each of `0A 00 00..7F` is written with 0, 1, 0x20, 0x40 and 0x7F (measured in
S-MU2000, against a render without the write). The S-YXG50 has no `0A` block. The
2006LE's own `0A` map holds `00`, `01` (mono priority, modelled), `10`, `20` and
`40..45`: the level controls are DLL behaviour and are modelled for the 2006LE (*S-YXG2006LE
voices*); `00` and `10` change nothing audible. hvyxg adds no `0A` parameter to the S-YXG50
(there is no reference for one).

Not implemented: a voice's own HPF (no bank has one). The 2006LE's unused filter types 2 / 10
already add the note HPF to the cutoff; a voice using them would get both (no bank entry selects
them).


## Full XG on the S-YXG2006LE (`--2006full`)

`Synth::Options::full_xg_2006le` (`render_yxg --2006full`), off by default, for the 2006LE
bank only (the S-YXG50 ignores it) and never with `--dll-exact`. The 2006LE engine then accepts
and acts on the XG message set and parameter behaviour of the S-YXG50 instead of the subset the
S-YXG2006LE DLL acts on. Its voices, kits, GS translation and every sound law stay its own.

**What XGlite is.** Yamaha's public material describes XGlite only as "a simplified version of
XG" with fewer voices and a reduced set of effects and controllers; no public XGlite parameter
list was found. The S-YXG50 implements XGlite itself (INI `XGLite`), so its two XGlite modes were measured: a 61-segment probe (voice banks incl.
GM2, kits, reverb / chorus / variation types and connections, filter, EGs, every controller
destination incl. PAT and AC1, portamento, mono, velocity sense and limits, scale tuning, note
limits, PEG, detune, element reserve, dry level, master tune / transpose, drum setup by SysEx and
NRPN, RPN, a bulk dump, 48 held notes, GS reset, XG parameters after it and a two-parameter GS
message) through `syxg50.dll` (`Process=1`) with `XGLite=0`, `1` and `2`, compared segment by
segment. The INI value reaches the engine as a mode (0 / 1 / 2) of its voice renderer and effect
DSP and, for
`2` only, as the voice-map flag `engine[0x6a9a]`.

| | `XGLite=1` | `XGLite=2` |
|---|---|---|
| Voices | every voice rendered by a lighter path (−17 dB residual against `XGLite=0` on a plain piano note); same banks | identical; GM2 bank MSB 121 silent (the dummy row), XG LSB variations through the XGlite fallback map (none of the probed ones changed) |
| Filter | cutoff / resonance, the MW filter control and the cutoff NRPN do not act (correlation 0.44 … 0.67 against `XGLite=0`) | identical |
| Velocity | velocity sense depth / offset (`08 nn 0C / 0D`) and velocity limits (`6D / 6E`) ignored | identical |
| Effects | chorus off, reverb type changes ignored, an insertion-connection variation part silent; system variation plays | insertion connection off: the part plays dry (+7.7 dB); everything else identical |
| Unchanged in both | portamento, mono, MW / bend / CAT / PAT / AC1 pitch and LFO, scale tuning, note limits, PEG, detune, element reserve, dry level, master tune, transpose, drum setup SysEx / NRPN, RPN, bulk dumps, polyphony (48 notes), GS messages | |

**The 2006LE against that.** The 2006LE is not the S-YXG50's XGlite: it renders full voices
with filters, acts on velocity sense and limits, chorus and reverb types, and its own voice set
is fixed by its bank. Its measured limits (code and probes):

| 2006LE limit | XGlite in the S-YXG50? | `--2006full` |
|---|---|---|
| Variation silent (system and insertion connection) | `XGLite=2`: insertion connection off | already audible by default (superset) |
| XG bulk dumps ignored | no | already applied by default (superset) |
| XG parameter changes ignored after a GS reset (XG System On still works) | no | obeyed, as the S-YXG50 does |
| GS messages accepted only with exactly one parameter (part `40 1x`: one byte, two for `00` / `17`, twelve for `40`; `01`, `18`, `24…2F`, `38…3F`, `41…` never; effect `40 01 30…40` one byte) | no | every byte counts, through the 2006LE's GS translation (banks 119 / 118, variation number as LSB) |
| Variation parameter ranges: delay times 7150 / 3550 (S-YXG50 7200 / 3600, 0.1 ms), the variation flanger's delay offset 0..63 (0..127), early reflection / gate room size and initial delay 0..44 / 0..63 (0..127, clamped to 44 / 63 by the control on both) | no | the S-YXG50's ranges, and its chorus-flanger offset limit of 63 |
| Distortion family clipped +24 dB above full scale (S-YXG50 +12 dB) | no | the S-YXG50's clip level (*Deliberate deviations*) |
| Multi-byte XG parameters written byte by byte (S-YXG50: one whole parameter per message) | no | kept: nothing is lost, every byte is written; the S-YXG50's rule only drops data |
| Polyphony 32 | no (48 notes in both XGlite modes) | kept; `--polyphony` sets it |
| 2006LE behaviour that is not a limit: GM Off only leaves GM mode, GM2 On keeps the XG effects, Reset All Controllers clears AC1 / AC2, CC94 stored in either connection, `02 01 5A / 5B` writes do not silence the insertion part, the 2006LE's GS translation, the effect laws (input gains, return scales, slews) | — | kept |

**Reference songs.** None of the ten test songs uses a lifted feature: `--2006full` renders
all ten bit-identical to the default 2006LE render. Over the 130-song corpus (`midi/GM`, `GS`, `XG`)
one song changes, `XG/feelthenight.mid` (a Distortion variation in system connection): against the
S-YXG50 render of the same song its level goes from +2.74 to +0.52 dB, the mean 5-s spectral
correlation (linear magnitude) from 0.883 to 0.839 (log magnitude 0.934 → 0.924): the distorted
part is now driven as on the S-YXG50, but its 0.8–3.2 kHz band comes out 4.2 dB below the
S-YXG50's (default: 2.0 dB above), the two banks' guitar voices. For the ten songs the 2006LE bank
against the S-YXG50 bank (both hvyxg, effects on; full XG identical): level −3.55 … +7.54 dB, mean
5-s spectral correlation 0.830 … 0.960 (moon_p +3.17 dB / 0.856, TrekMedG −2.28 / 0.926,
Nes_Netopia_mix +2.86 / 0.928, ORC4GM −1.71 / 0.960, ORC2GM −3.13 / 0.942, Be_with_me +0.50 /
0.922, HUMAN3GM −3.55 / 0.951, Chaotic_destiny +1.21 / 0.908, Dubnesstopia +7.54 / 0.927,
Full_Memory +1.48 / 0.830); waveform correlation is near zero (different samples), so it says
nothing here. The remaining difference is the banks' own voices and levels, which full XG keeps.

Tests: `tests/full_xg_test.cpp` (distortion drive against the S-YXG50, delay ranges, XG after a
GS reset, the option off with `--dll-exact` and on the S-YXG50 bank), `tests/front_end_test.cpp`
(`front_end_full_xg_on_the_2006le`).

## Front end (`midi/`, `synth.*`)

The front end (`midi::FrontEnd`) keeps the XG parameter memory in XG address order
(`midi/xg_ram.hpp`: System 00 00, Effect 02 01, Insertion 03 nn, Multi Part 08 nn, Drum Setup
3n rr), the live part state (`midi/part.hpp`) and sends note / pedal actions to the
`Renderer`. `hvyxg::Synth` (`synth.hpp`) wraps both.

Measured DLL behaviour it follows:

| Behaviour | Both DLLs | Here |
|---|---|---|
| SysEx device number | any accepted (XG 1n with n = 3 obeyed) | any accepted by default (`Options::device_id`) |
| Universal master volume `04 01` | writes its MSB to the XG master volume `00 00 04` (LSB ignored; the last of the two messages wins; every reset restores 127); see *Master volume* below | the same (`FrontEnd::universal_realtime`); exact at every value in both engines |
| GM2 master fine / coarse tuning `04 03` / `04 04` | obeyed, also by sounding notes; the S-YXG50 keeps the GM2 fine tuning apart from the XG master tune `00 00 00`, which it takes at the note-on only | the 2006LE folds `04 03` into the master tune; the S-YXG50 (`Bank::xg_parameter_table()`) stores it as `System::master_fine_gm2`, live, and latches the XG master tune at the note-on [meas: a master tune change never reaches a sounding S-YXG50 note; GM2 fine / coarse at the next commit, exact] |
| XG parameters after a GS reset | S-YXG50 obeys; **2006LE ignores them** (XG System On still works) | same (`Bank::xg_parameters_in_gs_mode`); `--2006full` obeys them on the 2006LE |
| GS reset / GS reverb and chorus (`40 01 30…40`) | translated onto the XG reverb / Chorus 1 by tables, differently per DLL (*GS mode*) | same (`midi/gs_effects.cpp`) |
| 2006LE GS voices | GS layer: melodic MSB 119 with CC#0 as LSB, rhythm MSB 118; CC#32 ignored | same (`Bank::gs_banks`) |
| Effect type write | loads the type's defaults (2006LE also for a single byte) | every MSB / LSB write reloads and counts (`*_type_writes`) |
| AC1 / AC2 values | a CC updates the AC value only when its number is the AC's controller number (`08 nn 59` / `60`) at that moment (any number 0..95, also one with its own function, which it still performs); a number change keeps the value, and the old number no longer moves it; every reset clears it to 0. Reset All Controllers: the 2006LE clears both values, the S-YXG50 keeps them. Every consumer reads it: element pitch / filter / amp / LFO depths and the variation control depths `02 01 5F / 60` | `Controller::ac`, read by both tone modules and `fx::control_offset` [meas: AC1 and AC2 each on six element destinations, both DLLs, −89 … −90 dB overall on the S-YXG50 (the old model: −22 … −36 dB in the reassigned segments); the S-YXG50 variation (chorus, delay LCR, tremolo, `Process=1`): −122 dB (old: −29 … −47 dB); 2006LE variation controller words match the predicted value at every step; 2006LE element destinations: exact (−129 … −300 dB) through the reassignment and the ignored old CC (old model: −26 … −35 dB); after a value step the residual is the same as for the same steps without a reassignment; the 2006LE controller transitions (formerly −28 dB pitch, −53 … −78 dB amp / filter / LFO) now match: see *S-YXG2006LE voices*, controller transitions] |
| Variation send CC94 | S-YXG50: stored only while the variation is in system connection (tests `engine[0x69dd]`); in insertion connection the controller is lost, so a CC94 sent before a later `02 01 5A 01` leaves the variation silent; the part parameter `08 nn 14` is stored in either connection. 2006LE: always stored | the same (`FrontEnd` CC94) [meas: CC94 before / after the connection write, in the same block, across insertion → system → insertion → system, `08 nn 14` instead: all −114 … −116 dB; before: +0.4 … +3.8 dB louder than the DLL] |
| GM2 On / GM Off (S-YXG50) | GM2 On does not reload the effect RAM, sets every unit up again (a system-connection variation return slews up from 0), sets the GM2 default reverb Large Hall (= XG `01 07`) and chorus Chorus 3 (= XG `41 05`, to reverb 0) as type writes, and every part to MSB 121 (drum parts 120), LSB 0; GM Off (`09 02`) is the XG System On | the same (`FrontEnd::reset`, `EffectData::restart`) [meas: GM2 programs with sends −103 dB (before: −7.4 dB, the effects were XG's); effect settings made before GM2 On −110 dB (before −2 dB); GM Off −110 dB (before: ignored, −14.6 dB)]. The 2006LE keeps the XG image and only leaves GM mode on GM Off (not measured) |
| Mono / poly messages | CC126 counts only with a value ≤ 16, CC127 only with 0; an accepted message, or any `08 nn 05` write, sets the mode and silences the part's notes as all sound off does (S-YXG50 damp, 2006LE removal at rate 0x70), also when the mode does not change | the same (`KillAll`) [meas: CC126 0 / 1 / 16 / 17 / 127, CC127 0 / 1 / 127 and `08 nn 05` 0 / 1, from poly and from mono, organ chord: the kill window matches at −119 … −163 dB on both engines; earlier the front end accepted every value and only released the notes] |

### Master volume

Measured on a sustained drawbar organ (program 16, key 60, velocity
127, volume / expression 127, reverb / chorus sends 0), each render divided by the same render
without the master-volume message, every MSB 0..127 and LSB 0 / 1 / 0x10 / 0x20 / 0x3F / 0x40 /
0x60 / 0x7F at 8 MSBs, CC7 100, and both orders of the two messages:

- **Both DLLs:** `F0 7F 7F 04 01 ll mm F7` stores `mm` as the XG System master volume
  (`F0 43 10 4C 00 00 04 vv F7`); `ll` has no effect, the two messages share one byte (the
  last one wins: XG 100 then 04 01 7F 7F plays at full level), and GM On, GM2 On, XG System On
  and XG All Parameter Reset all restore 127. Master volume 0 is silent.
- **S-YXG2006LE:** the master volume is the third factor of the voice level law,
  `kVolumeAtt[min(V[CC11]·V[CC7] >> 8 · V[master] >> 8, 255)]` with `V[v] = 2v` (127 → 256)
  (output stage), applied from the engine block holding the message
  and gliding 3 units per tick. With volume and expression at 127 the gain of MSB m is
  `A = kVolumeAtt[2m]` units (0x2000 → 32 units = −12.04 dB, 0x3000 → −4.72 dB, 0x0800 →
  −35.85 dB, MSB 1 → muted).
- **S-YXG50:** the master volume is a factor of the part gain `g`;
  0x2000 → −12.04 dB, 0x3000 → −5.11 dB, 0x0800 → −35.21 dB. It reaches a sounding voice only
  at the voice's next turn in the control layer's level round robin (tick slot `0x4b8`, every 80 ms
  at a phase set by the voice index), as do volume, expression, pan and sends.

hvyxg follows this exactly: the per-segment levels of both engines match the DLLs to < 0.001 dB
over the whole 14-bit range (before: the 2006LE took `14-bit >> 6`, up to 2.7 dB off for LSB ≥
0x40 and 0x3F80..0x3FFE, and the GM value only limited the XG one, up to 4.7 dB off; the
S-YXG50 took `v·127 / 0x3FFF`, up to 1.0 dB off, played master 0 at −75.8 dB and applied every
change at once instead of up to 80 ms later). `tests/master_volume_test.cpp`.

hvyxg decisions where the DLLs were not measured (superset or unknown):

| Area | Here |
|---|---|
| Reset effect image | the 2006LE's XG image (`kXgEffectResetImage`) after every reset, GM included (S-YXG50 GM2 On: kept, see *Front end*); a GS reset then applies the DLL's GS effect setup |
| GS parameters | patch part `40 1x` (incl. the receive switches 03…12 / 23 → XG 30…3F / 40), controller `40 2x 00 / 04 / 10`, drum map `41 m n` mapped onto the XG equivalents; "use for rhythm part" 1 / 2 → part mode 2 / 3. On the 2006LE a part message counts only with exactly its bytes (tone number 00 needs both, 01 alone is ignored) [code]; on the S-YXG50 every byte of a longer message counts [meas for 40 01] |
| GM2 RPN 0,5 (modulation depth range) | mapped onto MW LFO PMod depth (5 cents per step; the GM2 default 50 cents = the XG default 10) |
| CC75..78 (decay, vibrato rate / depth / delay) | write the XG part parameters, as CC71..74 |
| Drum NRPN 17 rr (EG decay) | sets decay 1 and decay 2 |
| Drum setups | 4 setups (part modes 2..5, MU2000); a setup is reloaded from the kit at every program change of a part in its mode and at a part-mode change |
| Receive switches `08 nn 30..40`, receive channel `04` | obeyed (several parts may share a channel; 0x7F = off) |
| Insertion blocks `03 nn` | 4 blocks; parameters 1..10 as single bytes 02..0B or 14-bit pairs 30..43 per type (MU2000 layout) |
| Multi EQ `02 40`, part EQ `08 nn 72/73/76/77`, part HPF `0A nn 20`, drum EQ / HPF `3n rr 20/21/24/25/50`, their XG2.0 NRPNs | stored and rendered (superset, see "Superset: EQ and HPF"); `0A nn 01` and `40..45` as the 2006LE does (on both engines' RAM, rendered by the 2006LE), other `0A nn` addresses ignored |
| Event timing | a message takes effect from the start of the engine block that contains its frame (the DLLs' rule); a message that arrives after its block has started acts from the next block |
| XG parameter messages | S-YXG50 (`Bank::xg_parameter_table()`): the DLL's parameter table — one whole parameter per parameter change (a lone byte of the 2-byte detune `08 nn 09/0A`, of the 4-nibble master tune or of an MSB / LSB effect type or variation parameter does nothing; bytes past the parameter are ignored and the end-of-message handlers are skipped: the parameter is in the RAM but not applied — master tune, master volume, program, receive channel, part mode, volume — until a later event reads it: CC7 / CC11 the volume, a clean receive-channel change the routing, the next note-on the mono / poly mode; a part mode left that way silences the part until a program change [meas]), bulk dumps write whole parameters up to the block's end without a checksum check [code, meas]. Blocks the DLL has no table for (02 40, 03, 0A) keep the superset byte-per-address rule. 2006LE: every byte to its own address [meas] |

## GS mode

The two DLLs implement GS very differently; hvyxg follows each (front end `midi/front_end.cpp`,
`midi/gs_effects.cpp`; engines `syxg50/tone_module.cpp`, `syxg2006le/voice_setup.cpp`).

**S-YXG50: a native GS mode** (`engine[0x63ae]` = 0). Its own GS program / kit tables (already
modelled), plus GS voice laws: the controller PMD, the part cutoff table
the vibrato depth (the element depth alone when larger), and for
melodic programs of banks below 64 a per-program vibrato rate base, vibrato delay
table and amp release offset; the part detune is scaled by the key
. Effects: a GS effect RAM translated into the XG effect RAM; the GS
reset only switches the reverb to Hall 2 over the XG Hall 1 parameters (−2 dB: Hall 2 has no
input-HPF boost), chorus unchanged.

**S-YXG2006LE: a GS → XG translator** in front of its XG parser. A GS reset is XG System On plus
XG messages: every part to MSB 119 (rhythm part 118), LSB 0, program 0, element reserves
2 / 6 / 0, the Hall 2 reverb macro and a Chorus 3 macro with delay 5. In GS mode CC#0 is kept as
the GS variation number and sent as the LSB of MSB 119 at the next program change; CC#32 is
ignored; the rhythm part plays MSB 118 (the GS kits, voice type 6) with LSB 0. GS part and
effect messages become XG parameter changes, one message per parameter. Everything else (pan 0 =
random, the XG volume / expression laws) is XG.

Measured (`moon_p.mid` channel solos, 2006LE): every channel within ±0.11 dB of the DLL but
channel 16 (−0.36 dB) — the 2 dB quiet rhythm part and the 1.3 dB loud melodic parts came from
playing the XG banks instead of MSB 118 / 119. The DLL's channel 10 render after the GS reset is
bit-identical to an XG render with MSB 118.

## MU banks (`--enable-mu`, `--bank-path`)

`Synth::Options::enable_mu` (`render_yxg --enable-mu`) loads a bank made by
SXG-Create-NF (BSD-3-Clause, https://github.com/NightFright2k19/SXG-Create-NF, commit 0086663), which converts
the ROM sets of the MU50 … MU1000 / MU2000 into S-YXG50 bin / wave tables, and plays it as
SXG-Create-NF's patched `syxg50.dll` does. It is MU voices and samples on the S-YXG50 engine
and effects, not an MU emulation. The behaviour below was taken from SXG-Create-NF's docs
(`docs/dll-patching.md`, `docs/models-and-output.md`) and the patch tables in its
`utility/dllpatch.py` / `makeSYXG50.py`, confirmed by a section-by-section byte diff of the
patched plugin against the stock DLL (every changed byte of code belongs to a listed patch; the
rest is the PE header, the Output Level tables and resources) and by renders (below).

**Layouts.** SXG-Create-NF writes two table layouts:
- *classic* (MU50 / MU80 / MU90, the "Enhanced" DLL): the stock layout. Loops longer than
  65 535 samples need 24-bit loop / start fields, which hvyxg always reads (the stock DLL masks
  them to 16 bits; the stock banks never exceed 52 811).
- *big* (MU100 / MU128 / MU1000 / MU2000, the "Full" DLL). The header has no flag for it; hvyxg
  detects it from the tables (`BinTable::looks_big`): seg15 holds u32 wave-zone offsets that all
  land on 16-byte records of seg16 (classic: u16 offsets, which read as u32 pairs fall outside),
  and the program maps are whole rows of u32. `MuLayout` forces it. Without `enable_mu` a
  big-layout table is refused with a `BankError` (it used to load and play garbage).

| Field | Classic (stock) | Big |
|---|---|---|
| Program maps seg11 / seg12 | u16: bit 15 = bank B, offset / 2 | u32 byte offset from voice bank A; banks A and B are one range |
| Ext drum voice offsets seg6 | u16 into bank A, index = entry `+0x11` | u32 from bank A, index = u16 LE at `+0x10` |
| Wave-zone offsets seg15 | u16 | u32; up to 768 multisamples in three pages (below) |
| Zone sample address / format | 24 bit at `+9`, format `+0x0C` | 32 bit at `+9`, format `+0x0D` |
| Drum entry: own wave | `+0x10 == 0xFF` | `+0x11 == 0xFF` |
| Drum entry: root key | `+0x12` | `+0x0A` (so receive note on, normally `+0x0A`, defaults to 1) |
| Drum entry: start / loop length | 16 bit at `+0x13` / `+0x16` | 24 bit at `+0x12` / `+0x15` |
| Drum entry: address / format | 24 bit at `+0x18`, format `+0x1B` | 30 bit at `+0x18`, format = its top two bits |

**Multisample pages.** An element names its multisample in one byte. When seg15 has more than 256
entries, an element that lies in voice bank B adds 256; when it has more than 512, an element at
or after the page-2 start (entry 768, an offset from bank B) adds another 256
(`BankData::zone`).

**Voice maps.** MU100 / MU128 / MU1000 tables carry a second map (MU Basic, the MU90-compatible
voices) in an appendix after the last segment (`'MAP2'`, 8 + 9 × 128 bytes): `+4` the default
map, `+5` = 1 when the MSB 48 row is present, `+8` the Basic drum-program rows (GS, XG, SFX, GM2)
and voice-bank rows (GS, MSB, XG, GM2), `+0x408` the MSB 48 row. `MuVoiceMap` picks the map
(`Default` = the appendix's byte 4; the patched DLL's ini `MUBasic`); Basic swaps all eight rows.
XG MSB 48 takes its bank from the MSB 48 row by LSB (0xFF: the MSB row as before), and uses it
as it is (no "1 = take the LSB row" step).

**Behaviour patches** (`enable_mu` only):

| Patch | Here |
|---|---|
| Effect types without a kernel → the nearest type (reverb Canyon → Tunnel; chorus Symphonic → Celeste 1, Phaser → Flanger 1, Ensemble Detune → Chorus 1; variation Auto / Touch Wah + Dist / OD and the Dist / Comp / Wah + Delay chains → Distortion / Overdrive, Touch Wah and Talking Modulator → Auto Wah, 2-Way Rotary → Rotary, Ensemble Detune → Symphonic) | `fx::mu_effect_type`, applied by `EffectData::write` to the RAM before the type's defaults load (the patch rewrites the engine's type bytes) |
| Per-type return level of the reverb and of the system-connection variation: `min((level · f + 32) >> 6, 127)`, f / 64 = Hall +2.5 dB, Room 1 +3.5, Room 2 / 3 +2.0, Stage 1 +1.0, Stage 2 +2.5, Plate +1.5, White Room +3.5, Basement −1.0; variation also Delay LCR +1.0, Delay LR +2.5, Echo +2.0, Cross Delay ×2, ER 2 +2.5, (Reverse) Gate +2.0, Karaoke 1 / 2 −1.5 / −2.5, Symphonic ×1.56, Rotary ×1.45, Tremolo / Auto Pan ×2 | `fx::mu_return_level`, in `SystemFx` |
| The reverb return recomputed after a type change | `SystemFx::sync_parameters` |
| Input-gain ramps after a type change in steps of 32 (reverb), 52 (chorus), 40 (variation) per 10-ms tick instead of 4 | `EngineModel::*_input_step` |
| Drum-setup EG offsets (`3n rr 0D–0F`, NRPN) on keys that play a voice (ext drum voices) count half: `(offset + 2 · rate − 64) >> 1` | `ToneModule50` drum prevoice AEG |
| Port select `F5 nn` ignored | already: system common messages are ignored |
| Voice-map switch, MSB 48 | above |
| Polyphony 256: a second, unmodified engine B takes note-ons while the first (A) has fewer than two free voices | modelled (`Renderer`, below) |
| GUI, Setup page, panel, names, Output Level range (0 dB default unchanged), ini lookup | not sound-relevant |

**Second engine (polyphony 256).** The patch (SXG-Create-NF `utility/dev/poly/poly.asm`, `route`
and `poly_main`; the host-block loop of the plugin read in the patched DLL) runs a second,
unmodified engine B in the same plugin. hvyxg models it in `Renderer` as a second S-YXG50 tone
module with its own system effects and buses, rendered after A into the same output:

- **Dispatch, per MIDI message** (not per part: a note-on that several parts receive goes to one
  engine). At the start of each 128-frame block the free-voice counts of A and B are read (the
  free-voice FIFO's count, `ToneModule::free_voices()`). A note-on with velocity
  > 0 goes to A while A's count is ≥ 2 (count − 2), else to B while B's is ≥ 2 (− 2), else to A
  (A steals). Note-offs (also velocity 0), controllers, SysEx and everything else go to both, so
  both always hold the song's state.
- **Sleep.** B renders only while awake: 4 s after its last voice (awake while its free count is
  below its polyphony of 128, the patch's default of 128 extra voices) and at least 250 ms after a
  SysEx that reaches it asleep. A note that wakes B gives it A's control-rate clock (the plugin's
  sample / 10-ms counters: `ToneModule::follow_clock`, and the mixer tick
  phase, `SystemEffects::follow_clock`), so its ticks fall with A's. Asleep, B takes its messages
  without rendering: its voices, effect tails and ramps stand still.
- **When.** With `enable_mu`, under `dll_exact` or when no polyphony is asked for (the default);
  an explicit `max_polyphony` without `dll_exact` keeps one engine of that size (superset). Stock
  banks: never (default output bit-identical).

Measured against the patched `syxg1000.dll` (`Process=1`) with a probing host that reads
A's and B's free-voice counts and B's wake counter (patch slot + 8)
after every block: the hvyxg values are **identical in every block** over the whole of
Nes_Netopia_mix (91 532 blocks; B never takes a note there, but wakes and sleeps in step after
the SysEx) and over a dense probe of 150 held two-element notes plus a 30-note drum burst in one
block (B up to 113 voices): that probe matches at −81.9 dB, correlation 1.000000 (one engine:
−5.1 dB, correlation 0.83, level −1.7 dB).

Without `dll_exact` the usual superset behaviour applies on top (insertion blocks, EQ / HPF,
steal fade, interpolation, polyphony). SXG-Create-NF's own limits that are baked into the bank
data (2 elements per voice, HPF as filtered sample copies every 6 keys, folded 3 / 4-element
voices, drum velocity sense as ext voices) stay as converted: lifting them needs a native MU
bank model. `--bank-path` with the ROMs (below) builds the same bank from them.

**Match against the patched DLL** (SXG-Create-NF MU1000 bank, `syxg1000.dll` with
`Process=1`, `--enable-mu --dll-exact`, 44.1 kHz, effects on, lag 0; MU2000 = S-MU2000 with the
genuine firmware, for context):

| Render | hvyxg vs patched DLL: level, mean 5-s spectral corr, waveform corr, error | hvyxg vs stock DLL on the stock bank, for comparison |
|---|---|---|
| Nes_Netopia_mix | ±0.000 dB, 1.00000, 1.000000, −71.4 dB (every second at −100 … −112 dB but 207–209 s: −52 / −57 dB. Not the second engine: B takes no note in this song and the free-voice counts match block for block. A 12-s excerpt reproduces it; every channel solo and both channel halves of it match; the error is a centred ≈ 50 Hz component, −55 dB, in bursts over 0.6 s from the block of two note-ons (channels 6 and 13)) | −104.6 dB |
| ORC4GM | ±0.000 dB, 1.00000, 1.000000, −90.2 dB | −106.9 dB |
| Be_with_me | −0.000 dB, 0.99986, 0.99965, −31.5 dB | 0.99997, 0.99997, −41.7 dB |
| moon_p (GS) | +0.018 dB, 0.99922, 0.99847, −25.1 dB | +0.021 dB, 0.99921, 0.99841, −24.9 dB |
| TrekMedG | +0.002 dB, 0.99985, 0.97643, −13.3 dB | −0.001 dB, 0.99973, 0.97992, −14.0 dB |
| Reverb / chorus types incl. the substituted ones (Canyon, Symphonic, Phaser, Ensemble Detune; Hall 1, Room 1, Basement, White Room, Chorus 1), held chords | −110 … −112 dB per type | −108 … −110 dB |
| Variation types set with the reset, system connection (Delay LCR, Cross Delay, Karaoke 2, Rotary, Symphonic and the substituted 2-Way Rotary, Touch Wah, Dist + Delay) | ±0.000 dB, −91 … −112 dB (Symphonic −66 dB, as the stock pair's −69 dB) | −97 … −111 dB |
| Drum setup EG `30 rr 0D–0F` = 20 / 30 / 30 on all keys (incl. ext drum voices) | −109.5 dB (without the ext-drum EG rule: −17.8 dB) | −109.1 dB |
| MSB 48 LSB 0 / 8 / 16 / 48 / 120, MSB 64, XG LSB 1 / 8 / 16 / 40, SFX kit; the same with MU Basic (`MUBasic=1`) | −97 … −113 dB per segment, both maps | — |
| 10 XG kits (programs 0, 1, 8, 16, 24, 25, 32, 40, 48, 127) × 16 keys | −80 … −109 dB per second | — |

The remaining song gaps are the stock engine's (the same figures on both banks). Three probe
gaps the MU work found were stock-engine gaps too and are fixed on the stock engine (so on both
banks): CC94 sent before a switch to system connection, drum-setup attack offsets that push a kit
rate past 94, and GM2 On (see *Front end* and *S-YXG50 voices*). They were not re-rendered
through the patched DLL.

Against the real MU2000 (S-MU2000, genuine firmware; its output is ≈ 6–8 dB hotter, a
constant, and its timing differs, so only spectra count) the converted bank is much closer than
the stock one: mean 5-s spectral correlation patched MU1000 / stock S-YXG50 = Nes_Netopia
0.975 / 0.927, TrekMedG 0.973 / 0.929, ORC4GM 0.983 / 0.941, Be_with_me 0.993 / 0.913; moon_p
(GS) 0.962 / 0.974, worse because of its GS rhythm part (channel 10 solo 0.915 against 0.948;
most other channels improve, e.g. channel 1 0.918 → 0.969, 16 0.792 → 0.877).

### Engine tables and embedded banks (`--plugin`, `--plugin-path`)

**Engine tables.** hvyxg ships none of the DLLs' tables (effect programs, defaults, limits and
type maps, the voice engines' curves and laws): `src/dll/tables.cpp` reads them at runtime from
your DLLs into an `EngineTables` object (`hvyxg/engine_tables.hpp`). The caller owns its memory
(`engine_tables_size`, `init_engine_tables`) and fills it from DLL images
(`load_engine_tables(tables, bytes)`, no files touched, nothing thrown); once filled it is read
only, holds no pointers, and one object serves any number of banks and synths on any threads. A
bank keeps the tables it was loaded with (`BankOptions::tables`), and its tone module and effects
read them from there; the library itself keeps no state. `Synth` loads them from the DLLs given
with `Synth::Options::plugin_files` / `plugin_file` (`render_yxg --plugin DLL`, repeatable) and
`plugin_path` (`--plugin-path DIR`: every PE file under the folder, recursively, whatever its
name), unless `tables` or `bank` brings them. No file names and no addresses are stored. Each table is a
descriptor: its element type in the DLL, its length, a 64-bit rolling hash and an XXH3-128 hash of
its bytes. The loader runs one rolling pass per table length over the DLL's data sections (not code,
not resources), confirms a candidate by its XXH3-128 and copies it; the hash pins the values, so
any DLL that holds the table works, wherever it keeps it (the stock plugins, the 5 MB S-YXG50 of the VEG release (its bank embedded in the DLL),
SXG-Create-NF's patched plugins). Reading and searching the four reference DLLs takes about 40 ms
(Apple M1 Max). The S-YXG50 engine needs
syxg50.dll or an SXG-Create-NF plugin (the variation limits only it has), the S-YXG2006LE engine
the S-YXG2006LE DLL; `load_bank` and `Synth`'s constructor throw a `BankError` naming the missing
tables and the DLL that supplies them. Two tables are not copies of one DLL table: the flanger limits are the
2006LE table, or the S-YXG50 table with the delay-offset limit clamped to 63 (its engine clamps
it, measured), whichever DLL is there; the XG effect RAM reset image is composed from the default
types and their defaults plus seven reset values (`midi::xg::EffectData::reset`), and a test
checks it against the 2006LE image's hash.

**Banks over the caller's memory.** `load_bank(BankMemory, model, options)` reads the bank where
the caller keeps the files: each buffer holds a file's bytes and `BankMemory::kPad` (8) spare
bytes. The S-YXG50 wave is descrambled in place when its header says so and its spare bytes
become the interpolator's padding (0x80, silence); the S-YXG2006LE wave is decrypted in place and
read as 16-bit samples (the buffer must be 2-byte aligned). The bank object itself is a small
index over that memory; the buffers must outlive it and every synth that plays it. The vector
and file overloads of `load_bank` own their buffers and are built on it.

**Embedded banks.** SXG-Create-NF's `--embed` stores the converted table and wave file in the
patched DLL as RT_RCDATA resources named after the files (as the 5 MB S-YXG50 stores its
SXGBIN41.TBL / SXGWAVE4.TBL) and names the table in the DLL's string table. When no bank is given
otherwise (`bank`, `bin_data`, `bin_file` / `data_file`, `bank_paths`), the first of the DLLs that
carries one supplies the bank (with `--enable-mu` for an MU plugin), read with a small PE resource
reader (`hvyxg/plugin_tables.hpp`: `pe_rcdata`, `pe_strings`, `read_plugin_tables`; the PE parser
itself is `src/pe_image.hpp`): the bin table is the RCDATA resource with an S-YXG50 / S-YXG2006LE
header (among several, the one the string table names), the wave table the resource named by the
bin header (0x10). Nothing in the DLL runs; every offset is bounds-checked (`BankError` on a
malformed or truncated file). Verified on an MU1000 plugin made by SXG-Create-NF 0086663 `main.py
--embed` (91 MB): the two resources are byte-identical to the `.TBL` / `.UPCM` of a plain run, and
`render_yxg --plugin` renders moon_p identically to `--bin / --data`.

### Finding the bank (`--bank-path`)

`Synth::Options::bank_paths` (`render_yxg --bank-path PATH`, repeatable) names files and
folders (searched recursively) where the bank is found by content when `bin_file` /
`data_file` are not given (`src/bank_search.cpp`). Candidates: the stock S-YXG50 tables (4 MB,
2 MB) and the stock S-YXG2006LE tables, known by their XXH3-128; other S-YXG50 tables
(SXG-Create-NF's MU conversions, custom banks), the wave file the table names beside it; MU ROM
sets (below). Files found this way are accepted as they are. Within a kind the default follows
the DLLs (4 MB before 2 MB; MU1000, MU2000, MU128); different kinds (say the S-YXG50 and the
S-YXG2006LE banks) are an error that lists them, unless `--model`, `--enable-mu` or `--mu-model`
(which asks for an MU bank) choose; an MU candidate alone implies `--enable-mu`. Precedence:
`--bin` / `--data`, then `--bank-path`, then a bank embedded in a DLL; an embedded bank under a
stock name (SXGBIN41.TBL, SXGBIN21.TBL, SXGBNW6L.TBL) must be that stock bank byte for byte.

### MU ROMs (`--bank-path`)

MU ROM sets under `bank_paths` build the MU bank from your own ROM dumps instead of
SXG-Create-NF's files (`hvyxg/mu_rom.hpp`), and imply `enable_mu`; `--mu-layout`, `--mu-voice-map` and `--dll-exact` work as with the files. The
converter (`src/syxg50/mu_rom/`) is a C++ port of SXG-Create-NF's default conversion at
its commit 0086663 (what `main.py` does with no extra flags: external tables, the native voice
map as default and MU Basic in the `MAP2` appendix), and its output is **byte-identical** to
SXG-Create-NF's `.TBL` / `.UPCM`, so a bank from the ROMs plays exactly like one from the
files. Only data tables are read from the program ROM; nothing in it is executed.

**Recognition.** Every file named, and every ROM-sized file (512 KB … 16 MB) under a named
directory, is identified by CRC-32 (SXG-Create-NF's `dataCRCs.py` list), whatever its name.
`Auto` takes the first complete set of MU1000, MU2000, MU128 (SXG-Create-NF also prefers the
MU1000 over the MU2000, whose sound set is the same); `MuRomModel` / `--mu-model` picks one.
Errors name the missing ROMs of a partial set ("MU2000 v2.01 is missing wave ROM xw848a0.ic53
(CRC32 34913E42), …"), ROMs that are known but not supported, and unrecognised files. The
program ROM may be the high / low pair or one joined file: a file of no known CRC whose halves,
split as concatenated, as 16-bit words interleaved (the CPU's view) or interleaved with each word
byte-swapped (the S-MU2000 flash image), in either order, are a known pair (`MuProgramForm`).

| Model | ROMs | Status |
|---|---|---|
| MU1000 v2.01 | `mu1000-v2.01-h/l.bin` + `xv364a0`, `xv365a0`, `xw848a0`, `xw849a0` | verified (below) |
| MU2000 v2.01 | `mu2000-v2.01-h/l.bin` (or S-MU2000's `mu2000_flash.bin`) + the same wave ROMs | verified, both the h / l pair and the flash image (each byte-identical to SXG-Create-NF's MU2000 tables) |
| MU128 v2.00 | `mu128-v2.00-h/l.bin` + `xv364a0`, `xv365a0`, `xv366a0`, `xv376a0` | untested (no ROMs at hand): the same code path as the MU1000 with SXG-Create-NF's MU128 table addresses |
| MU50 / MU80 / MU90 / MU100, MU128 v1.06, MU2000 v1.01 | — | not ported: recognised and reported; convert them with SXG-Create-NF and use `--enable-mu` |

**How it stays exact.** The port follows the Python step by step: objects that Python shares
between dicts are shared (`shared_ptr`), since the converter mutates them in place (the longer
of two clipped copies of a sample wins and takes both offsets); dicts keep insertion order
(`ODict`); `round()` rounds half to even, `round(x, n)` correctly rounded to n decimals;
`sum()` of floats is Python 3.12's compensated (Neumaier) sum; integer true division is
correctly rounded; floating-point expressions keep Python's evaluation order, and the
converter's files are compiled with `-ffp-contract=off` (an FMA would change the elemreduce
powers, EG bakes and HPF filtering). The sample-pool keys (loop address plus the reverse / HPF /
baked-EG tags) are 128-bit integers like Python's. Places where the Python code raises an
exception that it catches (a sample that cannot be converted makes `wavebank_power` give up)
raise and catch the same way (`mu_rom::ConvertError`).

**Verification.** `tests/mu_rom_test.cpp` (`HVYXG_MU_ROMS`, plus `HVYXG_MU_BIN` / `_DATA` and
optionally `HVYXG_MU2000_BIN` / `_DATA`) compares the converted tables with SXG-Create-NF's
byte for byte and renders from both. Results: MU1000 `SXGMU1KX.TBL` (574 436 bytes) and
`SXGMU1KX.UPCM` (89 938 584 bytes) identical; MU2000 `SXGMU2KX.TBL` / `.UPCM` identical
(SXG-Create-NF 0086663 run on the MU2000 v2.01 pair; its `.UPCM` equals the MU1000's, its
`.TBL` differs from the MU1000's in 1 916 bytes); `render_yxg --bank-path` with the ROMs against
`--enable-mu` with the files: identical WAVs for moon_p, Nes_Netopia_mix and TrekMedG on both
models, each in the default map, `--mu-voice-map basic` and `--dll-exact`.

**Cost** (arm64 Mac, release build, `render_yxg` of a one-note song; peak RSS):

| Load | Time | Peak memory |
|---|---|---|
| `--enable-mu` with SXG-Create-NF's files | 0.04 s | 175 MB |
| `--bank-path` (ROMs), MU1000 | 1.5 s | 294 MB |
| `--bank-path` (ROMs), MU2000 (scaling tables inverted to break points) | 2.8 s | 281 MB |
| `--bank-path … --mu-cache DIR`, first run (converts, writes) | as above + 0.1 s | as above |
| `--bank-path … --mu-cache DIR`, later runs (CRC check + read) | 0.15 s | 177 MB |

SXG-Create-NF itself takes 3.7 min (MU1000) and 4.3 min (MU2000) for the same tables. The
cache (`Synth::Options::mu_cache`, `--mu-cache`) stores `<model>-<key>.TBL / .UPCM`; the key is
a CRC-32 of the ROMs' CRCs and the converter version, so a different ROM set or converter
writes new files.

## DLL-exact mode (`--dll-exact`)

`Synth::Options::dll_exact` (`render_yxg --dll-exact`) turns off every superset behaviour, so
that the output follows the reference DLL: the stock DLLs for the stock banks, the patched DLL
with `enable_mu`. It is off by default, and the default output is unchanged (bit-identical on
all the test songs with both stock banks). With it:

| Superset behaviour | DLL-exact |
|---|---|
| Polyphony option | the DLL's (S-YXG50 128, S-YXG2006LE 32; with `enable_mu` the patched DLL's two engines of 128) |
| Interpolation option | linear |
| Steal fade | off (the DLL's steal) |
| Ports > 1 | one port |
| XG2.0 insertion blocks `03 nn`, their control depths | messages ignored (S-YXG50 does not parse them; the 2006LE's are inert) |
| Multi EQ `02 40`, part EQ `08 nn 72–77`, part HPF `0A nn 20`, drum-note EQ / HPF, their NRPNs, EQ change smoothing | not rendered (`0A nn 20`, `02 40`, `03`, the EQ / HPF NRPNs ignored; part / drum EQ flat): neither DLL renders them. The S-YXG50 ignores the whole `0A` block; the 2006LE keeps `0A nn 01` and `40..45`, which its DLL acts on |
| Full XG on the S-YXG2006LE (`--2006full`) | off |
| S-YXG2006LE audible variation (system and insertion connection) | silent, and the insertion part plays like the others, as in the DLL |
| S-YXG2006LE XG bulk dumps | ignored, as in the DLL |

Kept, because they are not audible or are bugs / artefacts rather than behaviour (listed in
*Deliberate deviations*): output polarity, the plain-C (`Process=1`) effect kernels instead of the
SSE ones, float instead of x87 arithmetic, the 32.32 sample phase, the SVF coefficient bound,
the S-YXG50's one-block-late right dry gain, the sleep-gate thresholds, any output rate. Not
covered (unknown DLL behaviour or not yet modelled): the hvyxg decisions listed under *Front
end* where the DLLs were not measured, and all-notes-off under sustain.

## Threads and real-time use

The contract (`include/hvyxg/synth.hpp`):

| What | Thread | Real-time safe |
|---|---|---|
| `load_bank`, `load_mu_roms`, `find_mu_roms`, `convert_mu_roms`, `Synth` constructor / destructor | any | no: file I/O, allocation, may throw (`BankError`, `std::invalid_argument`) |
| a loaded `Bank` (`std::shared_ptr<const Bank>`) | any number of synths on any threads at once | read-only after loading; no mutable or global state anywhere in the library |
| `Synth::render`, `midi`, `midi_stream`, `reset`, the queries | one thread at a time per synth (not synchronised) | yes: no locks, no I/O, no exceptions; no allocation (`render` never; `midi` while ≤ 1024 messages / 16 KB wait for a later block; `midi_stream` for SysEx ≤ 4 KB) |

A host whose MIDI arrives on another thread passes it to the audio thread itself (e.g. a
lock-free FIFO) and calls `midi` there, before `render`. How the code keeps to it:

- No mutable statics or globals, and no tables built at first use: the core's objects have no
  writable data at all (the engine tables live in the caller's `EngineTables`); the banks have
  no caches. `tests/threading_test.cpp` renders four synths on
  one bank on four threads and compares with single-threaded renders (also under
  ThreadSanitizer, `-DHVYXG_SANITIZE=thread`).
- Everything a block may need is allocated at construction: voices, the S-YXG50 free-voice FIFO
  (a fixed ring), scratch lists for steals and releases, the 2006LE note lists (128 per part),
  the four XG2.0 insertion units. `hvyxg_alloc_test` replaces `operator new` and checks that
  `render` (applying note-ons, steals, releases, resets, effect and insertion type changes),
  `midi`, `midi_stream` and `reset` allocate nothing, from the first call on.
- The message queue of `midi` is compacted as messages are applied, so a host that always has
  a later message queued does not grow it.
- Malformed input never throws or crashes: banks, ROMs, MIDI, SysEx and SMF are fuzzed
  (fuzz/README.md); bank values the engines index with (curve rows, record offsets) are
  bounds-checked at load or at use.

Cost and the polyphony cap (`Synth::kMaxPolyphony` = 1024): [PERFORMANCE.md](PERFORMANCE.md).

## Embedded hosts: no exceptions, no files

The core of the library builds with `-fno-exceptions -fno-rtti` and `HVYXG_NO_FILES=1`, which
hides every declaration that names a file or throws; it then keeps no writable globals at all,
so it can live in a shared, read-only module (an operating system's instrument library). What
it offers:

- `EngineTables` in the caller's memory, filled from DLL images (`hvyxg/engine_tables.hpp`);
  `check_engine_tables` reports what is missing.
- `load_bank(BankMemory, model, options, error)` over the caller's memory, or
  `load_bank(bin, data, model, options, error)` owning vectors: `nullptr` and an `Error` (kind and
  message) for a malformed bank.
- `convert_mu_roms(images, model, tables, out, error)`: an MU ROM set found by CRC-32 among ROM
  images in memory (pairs or joined program files), converted. Its internal checks cannot fail
  for the verified ROMs; without exceptions they would abort (`src/fail.hpp`). It needs 128-bit
  integers: the compiler's `__int128`, or, where there is none (32-bit RISC-V), a host's own
  types with the same operators, named by `HVYXG_INT128_HEADER`, `HVYXG_I128` and `HVYXG_U128`
  (`src/syxg50/mu_rom/py.hpp`); without either it reports that, and the converted tables are
  given instead.
- `Synth(bank, Settings)`: never throws (out-of-range settings are clamped; `Synth::check`).
  `ump(words, offset)` takes Universal MIDI Packets (MIDI 1.0 channel voice, 7-bit SysEx gathered
  per group, a System Reset as an XG System On; the group is the port), and
  `render(int32_t*, frames)` writes 32-bit integers at a 16-bit sample's scale, unclipped.

The hosted API (paths, `BankError`, `Synth(Options)`) is a layer over these, in the `*_files.cpp`
sources, `bank_io.cpp`, `bank_search.cpp` and `plugin_tables.cpp`. The build checks the core
both ways: `hvyxg_core_check` (`HVYXG_BUILD_CORE_CHECK`, on by default) compiles it with
`-fno-exceptions -fno-rtti` and `hvyxg_core_link` links it alone (a test runs it), and the tests cover the non-throwing API (`tests/bank_memory_test.cpp`,
`tests/ump_test.cpp`, the in-memory MU conversion in `tests/mu_rom_test.cpp`).

## Verifying against the DLLs

- Reference renders come from the real DLLs, run as black boxes by tools kept outside this
  repository; `render_yxg` renders the same MIDI file through hvyxg. Pitch, level, spectra
  and envelopes are compared with numpy, and single effect kernels are checked the same way.
- Tests that need a bank read `HVYXG_TBL_BIN` / `HVYXG_TBL_DATA` (S-YXG50) and
  `HVYXG_2006LE_BIN` / `HVYXG_2006LE_DATA` (S-YXG2006LE) and skip when they are unset.
  `export` them (in zsh, `env $VARS` does not word-split and silently skips the bank cases).
- MU2000 reference: renders through S-MU2000 (a third-party MU2000 emulator running the genuine firmware; needs your own MU2000 ROMs). Useful for XG
  behaviour neither DLL has, e.g. the XG2.0 insertion effects.

## Current match against the DLLs

The songs named here are a private test set, not distributed with hvyxg.

| Check | S-YXG50 | S-YXG2006LE |
|---|---|---|
| Isolated notes (pitch, level, EGs, filter, LFO, pan, drums) | 0.01-0.5 dB, ±1 cent | −90 … −120 dB residual on most cases; controller steps and sweeps on held notes at the same residual |
| Five test songs, dry, mean 5-s spectral correlation | 0.9990-0.9998 | 0.979-0.9996 (random pan) |
| Nes_Netopia_mix, effects on | level ≈ 0 dB (−0.17 … +0.01 dB by method), correlation 0.9995 | — (the DLL's variation is silent) |
| With the hvyxg front end (`render_yxg` vs the DLL, 10 test songs) | effects on: level −0.14 … +0.29 dB, spectral corr 0.9959 … 0.9998 (Nes_Netopia +0.007 dB, 0.99956); dry: 0.9962 … 0.99999 | dry: level −0.19 … +0.001 dB, spectral corr 0.9958 … 0.99999 (GM / XG songs); effects on: the audible variation adds up to +0.64 dB |
| Same, effects on, no time alignment (sample-exact timing) | waveform corr 0.957 … 0.998 on GM / XG songs (Be_with_me 0.9946, Chaotic_destiny 0.9921, Dubnesstopia 0.9977, Full_Memory 0.9899, HUMAN3GM 0.9572, Nes_Netopia 0.9940, ORC2GM 0.9740, ORC4GM 0.9663); TrekMedG 0.885; moon_p (GS) 0.984, +0.02 dB (SSE DLL); the voice FIFO log and the pitch / cutoff / glide-rate register changes of every voice are identical to the DLL's in all ten songs | waveform corr 0.897 … 0.977 on GM / XG songs (the audible variation and random pan); TrekMedG 0.881 (0.864 before the voice-manager model, 0.857 before the note handler); moon_p (GS) 0.989, +0.01 dB (0.987 before the voice-manager model, 0.978 before the controller transitions). Against the DLL with its plain-C kernels and 64-frame host buffers (no SSE address rounding, no half-buffer event shift), `--dll-exact`, waveform error at lag 0: ORC2GM / HUMAN3GM −111 dB, ORC4GM −78, Be_with_me −38, Dubnesstopia −30, Full_Memory −24, Nes_Netopia −22, moon_p −20, Chaotic_destiny −20, TrekMedG −16 (TrekMedG's voice manager is identical to the DLL's throughout; its remaining difference starts where the DSP object map diverges, at 20 s; random-pan parts limit Dubnesstopia, Full_Memory and moon_p: Full_Memory went from −23.79 to −23.71 dB with the voice-manager work, but with its pan-0 messages replaced by pan 1 from −29.58 to −29.66 dB). moon_p channel 16's former −0.36 dB is the SSE reverb / chorus rounding: +0.002 dB against the plain-C render |
| Reverb / chorus kernels | float-exact vs the plain-C kernels | float-exact vs the plain-C kernels |
| Variation, all types | 43 / 43 types ≥ 0.99999 in the system and insertion song tests (`Process=1`, lag 0; symphonic −62 dB, the others −97 … −110 dB) | kernel IRs ≤ 4.5e-6 of peak |
