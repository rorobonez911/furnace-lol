/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

// VGM importer: SN76489, YM2612, YM2151, YM3812, YMF262, AY8910, Game Boy DMG

#include "fileOpsCommon.h"
#include <cmath>
#include <vector>
#include <map>
#include <functional>
#include <cstring>

#define VGM_RATE    44100
#define VGM_PAT_LEN 64

// VGM header field offsets
#define VGM_OFF_VER       0x08
#define VGM_OFF_SN_CLK    0x0C
#define VGM_OFF_GD3       0x14
#define VGM_OFF_TOTALSAMP 0x18
#define VGM_OFF_OPN2_CLK  0x2C
#define VGM_OFF_OPM_CLK   0x30
#define VGM_OFF_DATA      0x34
#define VGM_OFF_OPN_CLK   0x44
#define VGM_OFF_OPNA_CLK  0x48
#define VGM_OFF_OPL2_CLK  0x50
#define VGM_OFF_OPL3_CLK  0x5C
#define VGM_OFF_AY_CLK    0x74
#define VGM_OFF_GB_CLK    0x80

// VGM command opcodes
#define OP_SN     0x50
#define OP_OPN2_0 0x52
#define OP_OPN2_1 0x53
#define OP_OPM    0x54
#define OP_OPL2   0x5A
#define OP_OPL3_0 0x5E
#define OP_OPL3_1 0x5F
#define OP_WAIT2  0x61
#define OP_W60    0x62
#define OP_W50    0x63
#define OP_END    0x66
#define OP_PCMBLK 0x67
#define OP_AY     0xA0
#define OP_GB     0xB3
#define OP_NES    0xB4
#define OP_PCE    0xB9

static inline uint16_t rl16(const unsigned char* p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t rl32(const unsigned char* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}

// MIDI note 69 = A4 = 440 Hz; furnace shifts by -12 (C-4 = note 48)
static int freqToNote(double freq) {
  if (freq <= 0.0) return -1;
  int note = (int)round(12.0 * log2(freq / 440.0) + 69.0) - 12;
  return (note >= 0 && note <= 119) ? note : -1;
}

static int snPeriodToNote(int period, int clock) {
  return (period > 0) ? freqToNote((double)clock / (32.0 * period)) : -1;
}

// freq = fnum * clock / (144 * 2^(21-block))
static int opn2FnumToNote(uint16_t fnum, uint8_t block, int clock) {
  return (fnum > 0) ? freqToNote((double)fnum * clock / (144.0 * (1u << (21 - block)))) : -1;
}

// OPM KC: bits[6:4]=octave, bits[3:0]=note (0-13, skips sharps)
static int opmKcToNote(uint8_t kc) {
  static const int map[14]={0,1,2,3,4,5,6,7,8,9,10,11,0,1};
  int semi = kc & 0x0F;
  return ((kc >> 4) & 0x07) * 12 + map[semi < 14 ? semi : 13];
}

static int ayPeriodToNote(int period, int clock) {
  return (period > 0) ? freqToNote((double)clock / (16.0 * period)) : -1;
}

// GB CH1/2: freq = 131072 / (2048 - x)
static int gbFreqToNote(uint16_t x) {
  return (x < 2048) ? freqToNote(131072.0 / (2048 - x)) : -1;
}

// GB CH3 wave: freq = 65536 / (2048 - x)
static int gbWaveFreqToNote(uint16_t x) {
  return (x < 2048) ? freqToNote(65536.0 / (2048 - x)) : -1;
}

enum VgmEvType { VGM_NOTE_ON, VGM_NOTE_OFF };

struct VgmEvent {
  uint32_t tick;
  int      chan;
  VgmEvType type;
  int      note;
  int      vol;
  int      ins;
};

struct PSGChan {
  bool noteOn = false;
  int  period = 0;
  int  atten  = 15;
  int  note   = -1;
};

struct FM2612Chan {
  uint16_t fnumLo = 0;
  uint16_t fnumHi = 0;
  uint8_t  block  = 0;
  bool     noteOn = false;
  int      note   = -1;
};

struct OPMChan {
  bool    noteOn = false;
  int     note   = -1;
};

struct AYChan {
  bool noteOn = false;
  int  note   = -1;
  int  vol    = 0;
  int  period = 0;
};

struct GBChan {
  bool     noteOn = false;
  int      note   = -1;
  uint16_t freq   = 0;
};

struct OPL2Chan {
  bool     noteOn = false;
  int      note   = -1;
  uint16_t fnum   = 0;
  uint8_t  block  = 0;
};

// FM instrument key: alg+fb + 4 ops of {ar,dr,sl,rr,tl,mul,dt,d2r,ssgEnv}
struct FMInsKey {
  uint8_t alg, fb;
  uint8_t op[4][9];
  bool operator<(const FMInsKey& o) const {
    return memcmp(this, &o, sizeof(FMInsKey)) < 0;
  }
};

static FMInsKey buildOPN2InsKey(uint8_t* r, int ch) {
  FMInsKey k = {};
  k.fb  = (r[0xB0 + ch] >> 3) & 7;
  k.alg = r[0xB0 + ch] & 7;
  // YM2612 op layout within a channel: OP1=+0, OP3=+4, OP2=+8, OP4=+12
  int opOff[4] = {0, 8, 4, 12};
  for (int op = 0; op < 4; op++) {
    int o = opOff[op] + ch;
    k.op[op][0] = r[0x50 + o] & 0x1F;
    k.op[op][1] = r[0x60 + o] & 0x1F;
    k.op[op][2] = (r[0x80 + o] >> 4) & 0x0F;
    k.op[op][3] = r[0x80 + o] & 0x0F;
    k.op[op][4] = r[0x40 + o] & 0x7F;
    k.op[op][5] = r[0x30 + o] & 0x0F;
    k.op[op][6] = (r[0x30 + o] >> 4) & 0x07;
    k.op[op][7] = r[0x70 + o] & 0x1F;
    k.op[op][8] = r[0x90 + o] & 0x0F;
  }
  return k;
}

static DivInstrument* makeOPN2Ins(const FMInsKey& k, const char* name) {
  DivInstrument* ins = new DivInstrument;
  ins->type = DIV_INS_FM;
  ins->name = name;
  ins->fm.alg = k.alg;
  ins->fm.fb  = k.fb;
  ins->fm.ops = 4;
  for (int op = 0; op < 4; op++) {
    ins->fm.op[op].ar     = k.op[op][0];
    ins->fm.op[op].dr     = k.op[op][1];
    ins->fm.op[op].sl     = k.op[op][2];
    ins->fm.op[op].rr     = k.op[op][3];
    ins->fm.op[op].tl     = k.op[op][4];
    ins->fm.op[op].mult   = k.op[op][5];
    ins->fm.op[op].dt     = k.op[op][6];
    ins->fm.op[op].d2r    = k.op[op][7];
    ins->fm.op[op].ssgEnv = k.op[op][8];
    ins->fm.op[op].enable = true;
  }
  return ins;
}

static FMInsKey buildOPL2InsKey(uint8_t* regs, int ch) {
  FMInsKey k = {};
  // OPL2 modulator/carrier operator slot offsets per channel
  static const int modOff[9]={0,1,2,8,9,10,16,17,18};
  static const int carOff[9]={3,4,5,11,12,13,19,20,21};
  if (ch >= 9) return k;
  int m = modOff[ch], c = carOff[ch];
  k.fb  = (regs[0xC0 + ch] >> 1) & 7;
  k.alg = regs[0xC0 + ch] & 1;
  // 0x20: KSR/MUL, 0x40: KSL/TL, 0x60: AR/DR, 0x80: SL/RR
  k.op[0][0] = (regs[0x60 + m] >> 4) & 0x0F;
  k.op[0][1] = regs[0x60 + m] & 0x0F;
  k.op[0][2] = (regs[0x80 + m] >> 4) & 0x0F;
  k.op[0][3] = regs[0x80 + m] & 0x0F;
  k.op[0][4] = regs[0x40 + m] & 0x3F;
  k.op[0][5] = regs[0x20 + m] & 0x0F;
  k.op[1][0] = (regs[0x60 + c] >> 4) & 0x0F;
  k.op[1][1] = regs[0x60 + c] & 0x0F;
  k.op[1][2] = (regs[0x80 + c] >> 4) & 0x0F;
  k.op[1][3] = regs[0x80 + c] & 0x0F;
  k.op[1][4] = regs[0x40 + c] & 0x3F;
  k.op[1][5] = regs[0x20 + c] & 0x0F;
  return k;
}

static DivInstrument* makeOPL2Ins(const FMInsKey& k, const char* name) {
  DivInstrument* ins = new DivInstrument;
  ins->type = DIV_INS_OPL;
  ins->name = name;
  ins->fm.alg = k.alg;
  ins->fm.fb  = k.fb;
  ins->fm.ops = 2;
  for (int op = 0; op < 2; op++) {
    ins->fm.op[op].ar     = k.op[op][0];
    ins->fm.op[op].dr     = k.op[op][1];
    ins->fm.op[op].sl     = k.op[op][2];
    ins->fm.op[op].rr     = k.op[op][3];
    ins->fm.op[op].tl     = k.op[op][4];
    ins->fm.op[op].mult   = k.op[op][5];
    ins->fm.op[op].enable = true;
  }
  return ins;
}

// OPL2 freq = fnum * clock / (1 << (20 - block))
static int opl2FnumToNote(uint16_t fnum, uint8_t block, int clock) {
  return (fnum > 0) ? freqToNote((double)fnum * clock / (double)(1u << (20 - block))) : -1;
}

bool DivEngine::loadVGM(unsigned char* file, size_t len) {
  struct BadVGM {};
  bool success = false;
  warnings = "";

  try {
    if (len < 0x40) throw BadVGM{};
    if (memcmp(file, "Vgm ", 4) != 0) { lastError = "not a VGM file"; return false; }

    uint32_t version   = rl32(file + VGM_OFF_VER);
    uint32_t totalSamp = rl32(file + VGM_OFF_TOTALSAMP);

    // data offset field is relative to its own position (0x34); 0 means use default 0x40
    uint32_t dataRelOff = (version >= 0x150) ? rl32(file + VGM_OFF_DATA) : 0;
    uint32_t dataStart  = (dataRelOff == 0) ? 0x40 : (VGM_OFF_DATA + dataRelOff);
    if (dataStart >= len) throw BadVGM{};

    auto readClock = [&](uint32_t off) -> int {
      return (len > off + 3) ? (int)(rl32(file + off) & 0x3FFFFFFF) : 0;
    };

    int snClock   = readClock(VGM_OFF_SN_CLK);
    int opn2Clock = readClock(VGM_OFF_OPN2_CLK);
    int opmClock  = readClock(VGM_OFF_OPM_CLK);
    int opnClock  = readClock(VGM_OFF_OPN_CLK);
    int opnaClock = readClock(VGM_OFF_OPNA_CLK);
    int opl2Clock = readClock(VGM_OFF_OPL2_CLK);
    int opl3Clock = readClock(VGM_OFF_OPL3_CLK);
    int ayClock   = readClock(VGM_OFF_AY_CLK);
    int gbClock   = (version >= 0x161) ? readClock(VGM_OFF_GB_CLK) : 0;

    bool hasSN   = snClock   > 0;
    bool hasOPN2 = opn2Clock > 0;
    bool hasOPM  = opmClock  > 0;
    bool hasOPN  = opnClock  > 0;
    bool hasOPNA = opnaClock > 0;
    bool hasOPL2 = opl2Clock > 0;
    bool hasOPL3 = opl3Clock > 0;
    bool hasAY   = ayClock   > 0;
    bool hasGB   = gbClock   > 0;

    logI("[VGM] v%03X %u samples SN=%d OPN2=%d OPM=%d OPN=%d OPNA=%d OPL2=%d OPL3=%d AY=%d GB=%d",
         version, totalSamp, hasSN, hasOPN2, hasOPM, hasOPN, hasOPNA, hasOPL2, hasOPL3, hasAY, hasGB);

    // channel layout: SN(4), OPN2(6), OPN(3), OPNA(9), OPM(8), OPL2(9), OPL3(18), AY(3), GB(4)
    int snBase   = 0,             snChans   = hasSN   ? 4  : 0;
    int opn2Base = snBase+snChans, opn2Chans = hasOPN2 ? 6  : 0;
    int opnBase  = opn2Base+opn2Chans, opnChans = hasOPN ? 3 : 0;
    int opnaBase = opnBase+opnChans,  opnaChans = hasOPNA ? 9 : 0;
    int opmBase  = opnaBase+opnaChans, opmChans = hasOPM  ? 8 : 0;
    int opl2Base = opmBase+opmChans,  opl2Chans = hasOPL2 ? 9 : 0;
    int opl3Base = opl2Base+opl2Chans,opl3Chans = hasOPL3 ? 18: 0;
    int ayBase   = opl3Base+opl3Chans, ayChans  = hasAY   ? 3 : 0;
    int gbBase   = ayBase+ayChans,     gbChans  = hasGB   ? 4 : 0;
    int totalChans = gbBase + gbChans;

    if (totalChans < 1) { lastError = "no supported chips found in VGM"; return false; }
    if (totalChans > DIV_MAX_CHANS) totalChans = DIV_MAX_CHANS;

    PSGChan    psg[4];
    FM2612Chan opn2[6];
    OPMChan    opm[8];
    AYChan     ay[3];
    GBChan     gb[4];
    OPL2Chan   opl2[9];

    uint8_t ayRegs[16]    = {};
    uint8_t opn2r0[0x100] = {};
    uint8_t opn2r1[0x100] = {};
    uint8_t opmRegs[0x100]= {};
    uint8_t opl2Regs[0x200]={};
    uint8_t opl3R0[0x200] = {};

    int  snLatchCh   = 0;
    bool snLatchType = false;

    // Count 60Hz (735 samples) vs 50Hz (882 samples) waits to detect frame rate
    int w735 = 0, w882 = 0;
    {
      size_t p = dataStart;
      while (p < len) {
        uint8_t op = file[p++];
        if (op == OP_END) break;
        if (op == OP_W60) { w735++; continue; }
        if (op == OP_W50) { w882++; continue; }
        if (op == OP_WAIT2) { p += 2; continue; }
        if (op >= 0x70 && op <= 0x7F) continue;
        if (op == OP_SN) { p++; continue; }
        if (op == OP_GB || op == OP_NES || op == OP_PCE) { p += 2; continue; }
        if (op == OP_AY) { p += 2; continue; }
        if ((op >= 0x51 && op <= 0x5F) || op == OP_OPN2_0 || op == OP_OPN2_1) { p += 2; continue; }
        if (op == OP_PCMBLK) { if (p+6>len) break; p += 6+rl32(file+p+2); continue; }
        if (op >= 0x80 && op <= 0x8F) continue;
        if (op==0x90||op==0x91) { p+=4; continue; }
        if (op==0x92) { p+=5; continue; }
        if (op==0x93) { p+=10; continue; }
        if (op==0x94) { p++; continue; }
        if (op==0x95) { p+=4; continue; }
        if ((op>=0xC0&&op<=0xD6)||(op>=0xA0&&op<=0xBF)) { p+=2; continue; }
        if (op>=0xE0) { p+=4; continue; }
        break;
      }
    }

    double vgmHz      = (w735 >= w882) ? 60.0 : 50.0;
    double sampPerRow = VGM_RATE / vgmHz;
    logI("[VGM] %.0f Hz detected (w60=%d w50=%d)", vgmHz, w735, w882);

    std::vector<VgmEvent>        events;
    std::map<FMInsKey, int>      insCache;
    std::vector<DivInstrument*>  instruments;

    auto getOPN2Ins = [&](int ch) -> int {
      int portCh = ch % 3;
      uint8_t* regs = (ch < 3) ? opn2r0 : opn2r1;
      FMInsKey k = buildOPN2InsKey(regs, portCh);
      auto it = insCache.find(k);
      if (it != insCache.end()) return it->second;
      char name[32]; snprintf(name, sizeof(name), "OPN2 ch%d", ch);
      int idx = (int)instruments.size();
      instruments.push_back(makeOPN2Ins(k, name));
      insCache[k] = idx;
      return idx;
    };

    auto getOPL2Ins = [&](int ch) -> int {
      FMInsKey k = buildOPL2InsKey(opl2Regs, ch);
      auto it = insCache.find(k);
      if (it != insCache.end()) return it->second;
      char name[32]; snprintf(name, sizeof(name), "OPL2 ch%d", ch);
      int idx = (int)instruments.size();
      instruments.push_back(makeOPL2Ins(k, name));
      insCache[k] = idx;
      return idx;
    };

    auto opn2Chan  = [&](int ch) { return opn2Base + ch; };
    auto opmChan_  = [&](int ch) { return opmBase  + ch; };
    auto ayChan_   = [&](int ch) { return ayBase   + ch; };
    auto gbChan_   = [&](int ch) { return gbBase   + ch; };
    auto opl2Chan_ = [&](int ch) { return opl2Base + ch; };

    uint32_t curTick = 0;
    size_t p = dataStart;

    auto emitNoteOn = [&](int chan, int note, int vol, int ins) {
      if (chan < 0 || chan >= totalChans || note < 0) return;
      events.push_back({curTick, chan, VGM_NOTE_ON, note, vol, ins});
    };
    auto emitNoteOff = [&](int chan) {
      if (chan >= 0 && chan < totalChans)
        events.push_back({curTick, chan, VGM_NOTE_OFF, 0, -1, -1});
    };

    while (p < len) {
      uint8_t op = file[p++];

      if (op == OP_END) break;
      if (op == OP_W60)  { curTick += 735; continue; }
      if (op == OP_W50)  { curTick += 882; continue; }
      if (op == OP_WAIT2){ if (p+2>len) break; curTick += rl16(file+p); p+=2; continue; }
      if (op >= 0x70 && op <= 0x7F) { curTick += (op & 0x0F) + 1; continue; }
      if (op >= 0x80 && op <= 0x8F) { curTick += (op & 0x0F);     continue; }

      if (op == OP_PCMBLK) {
        if (p+6>len) break;
        p += 6 + rl32(file+p+2);
        continue;
      }

      if (op == OP_SN && hasSN) {
        if (p >= len) break;
        uint8_t d = file[p++];
        if (d & 0x80) {
          snLatchCh   = (d >> 5) & 3;
          snLatchType = (d >> 4) & 1;
          if (!snLatchType) {
            psg[snLatchCh].period = (psg[snLatchCh].period & 0x3F0) | (d & 0x0F);
          } else {
            int atten = d & 0x0F;
            PSGChan& c = psg[snLatchCh];
            bool wasOn = c.noteOn;
            c.atten  = atten;
            c.noteOn = (atten < 15);
            if (wasOn && !c.noteOn) emitNoteOff(snBase + snLatchCh);
            if (!wasOn && c.noteOn && snLatchCh < 3) {
              int note = snPeriodToNote(c.period, snClock);
              if (note >= 0) {
                c.note = note;
                emitNoteOn(snBase + snLatchCh, note, 255 - (atten * 255 / 15), -1);
              }
            }
          }
        } else {
          if (!snLatchType) {
            psg[snLatchCh].period = ((d & 0x3F) << 4) | (psg[snLatchCh].period & 0x0F);
            PSGChan& c = psg[snLatchCh];
            if (c.noteOn && snLatchCh < 3) {
              int note = snPeriodToNote(c.period, snClock);
              if (note >= 0 && note != c.note) {
                c.note = note;
                emitNoteOn(snBase + snLatchCh, note, 255 - (c.atten * 255 / 15), -1);
              }
            }
          }
        }
        continue;
      } else if (op == OP_SN) { p++; continue; }

      if (op == OP_OPN2_0 && hasOPN2) {
        if (p+2>len) break;
        uint8_t reg = file[p++], dat = file[p++];
        opn2r0[reg] = dat;
        int ch = reg & 3;
        if (ch == 3) continue;
        if (reg >= 0xA4 && reg <= 0xA6) { opn2[ch].fnumHi=(dat&7); opn2[ch].block=(dat>>3)&7; continue; }
        if (reg >= 0xA0 && reg <= 0xA2) {
          opn2[ch].fnumLo = dat;
          if (opn2[ch].noteOn) {
            int note = opn2FnumToNote((uint16_t)(opn2[ch].fnumHi<<8)|opn2[ch].fnumLo, opn2[ch].block, opn2Clock);
            if (note >= 0 && note != opn2[ch].note) { opn2[ch].note=note; emitNoteOn(opn2Chan(ch),note,-1,getOPN2Ins(ch)); }
          }
          continue;
        }
        if (reg == 0x28) {
          int kch = dat & 7;
          if (kch == 3 || kch == 7) continue;
          int fCh = (kch >= 4) ? (kch - 1) : kch;
          bool on = (dat & 0xF0) != 0;
          FM2612Chan& c = opn2[fCh];
          if (on && !c.noteOn) {
            c.noteOn = true;
            int note = opn2FnumToNote((uint16_t)(c.fnumHi<<8)|c.fnumLo, c.block, opn2Clock);
            c.note = note;
            emitNoteOn(opn2Chan(fCh), note, 200, getOPN2Ins(fCh));
          } else if (!on && c.noteOn) {
            c.noteOn = false;
            emitNoteOff(opn2Chan(fCh));
          }
        }
        continue;
      } else if (op == OP_OPN2_0) { p+=2; continue; }

      if (op == OP_OPN2_1 && hasOPN2) {
        if (p+2>len) break;
        uint8_t reg = file[p++], dat = file[p++];
        opn2r1[reg] = dat;
        int ch = (reg & 3) + 3;
        if ((reg & 3) == 3) continue;
        if (reg >= 0xA4 && reg <= 0xA6) { opn2[ch].fnumHi=(dat&7); opn2[ch].block=(dat>>3)&7; continue; }
        if (reg >= 0xA0 && reg <= 0xA2) {
          opn2[ch].fnumLo = dat;
          if (opn2[ch].noteOn) {
            int note = opn2FnumToNote((uint16_t)(opn2[ch].fnumHi<<8)|opn2[ch].fnumLo, opn2[ch].block, opn2Clock);
            if (note >= 0 && note != opn2[ch].note) { opn2[ch].note=note; emitNoteOn(opn2Chan(ch),note,-1,getOPN2Ins(ch)); }
          }
          continue;
        }
        continue;
      } else if (op == OP_OPN2_1) { p+=2; continue; }

      if (op == OP_OPM && hasOPM) {
        if (p+2>len) break;
        uint8_t reg = file[p++], dat = file[p++];
        opmRegs[reg] = dat;
        if (reg == 0x08) {
          int ch = dat & 7;
          bool on = (dat & 0x78) != 0;
          OPMChan& c = opm[ch];
          if (on && !c.noteOn) {
            c.noteOn = true;
            int note = opmKcToNote(opmRegs[0x28 + ch]);
            c.note = note;
            DivInstrument* ins = new DivInstrument;
            ins->type = DIV_INS_FM; ins->name = "OPM";
            int idx = (int)instruments.size();
            instruments.push_back(ins);
            emitNoteOn(opmChan_(ch), note, 200, idx);
          } else if (!on && c.noteOn) {
            c.noteOn = false;
            emitNoteOff(opmChan_(ch));
          }
        }
        continue;
      } else if (op == OP_OPM) { p+=2; continue; }

      if (op == OP_OPL2 && hasOPL2) {
        if (p+2>len) break;
        uint8_t reg = file[p++], dat = file[p++];
        opl2Regs[reg] = dat;
        // 0xA0-0xA8: F-num low; 0xB0-0xB8: F-num high[1:0] + block[2:0] + keyon[5]
        if (reg >= 0xA0 && reg <= 0xA8) {
          opl2[reg-0xA0].fnum = (opl2[reg-0xA0].fnum & 0x300) | dat;
          continue;
        }
        if (reg >= 0xB0 && reg <= 0xB8) {
          int ch = reg - 0xB0;
          opl2[ch].fnum  = ((uint16_t)(dat & 0x03) << 8) | (opl2[ch].fnum & 0xFF);
          opl2[ch].block = (dat >> 2) & 7;
          bool on = (dat >> 5) & 1;
          OPL2Chan& c = opl2[ch];
          if (on && !c.noteOn) {
            c.noteOn = true;
            c.note = opl2FnumToNote(c.fnum, c.block, opl2Clock ? opl2Clock : 3579545);
            emitNoteOn(opl2Chan_(ch), c.note, 200, getOPL2Ins(ch));
          } else if (!on && c.noteOn) {
            c.noteOn = false;
            emitNoteOff(opl2Chan_(ch));
          }
          continue;
        }
        continue;
      } else if (op == OP_OPL2) { p+=2; continue; }

      if ((op == OP_OPL3_0 || op == OP_OPL3_1) && hasOPL3) {
        if (p+2>len) break;
        uint8_t reg = file[p++], dat = file[p++];
        if (op == OP_OPL3_0) {
          opl3R0[reg] = dat;
          if (reg >= 0xB0 && reg <= 0xB8) {
            int ch = reg - 0xB0;
            uint16_t fnum = ((uint16_t)(dat & 0x03) << 8) | opl3R0[0xA0 + ch];
            bool on = (dat >> 5) & 1;
            OPL2Chan& c = opl2[ch];
            if (on && !c.noteOn) {
              c.noteOn = true;
              c.note = opl2FnumToNote(fnum, (dat>>2)&7, opl3Clock ? opl3Clock : 14318181);
              DivInstrument* ins = new DivInstrument;
              ins->type = DIV_INS_OPL; ins->name = "OPL3";
              int idx = (int)instruments.size();
              instruments.push_back(ins);
              emitNoteOn(opl2Chan_(ch), c.note, 200, idx);
            } else if (!on && c.noteOn) {
              c.noteOn = false;
              emitNoteOff(opl2Chan_(ch));
            }
          }
        }
        continue;
      } else if (op == OP_OPL3_0 || op == OP_OPL3_1) { p+=2; continue; }

      if (op == OP_AY && hasAY) {
        if (p+2>len) break;
        uint8_t reg = file[p++], dat = file[p++];
        if (reg < 16) ayRegs[reg] = dat;
        // regs 8-10: volume A/B/C; reg 7: mixer (bit N=0 enables tone ch N)
        if (reg >= 8 && reg <= 10) {
          int ch = reg - 8;
          int vol = ayRegs[reg] & 0x0F;
          AYChan& c = ay[ch];
          bool wasOn = c.noteOn;
          c.vol    = vol;
          c.noteOn = !((ayRegs[7] >> ch) & 1) && vol > 0;
          if (wasOn && !c.noteOn) emitNoteOff(ayChan_(ch));
          if (!wasOn && c.noteOn) {
            c.period = ((ayRegs[ch*2+1] & 0x0F) << 8) | ayRegs[ch*2];
            c.note   = ayPeriodToNote(c.period, ayClock ? ayClock : 1789773);
            emitNoteOn(ayChan_(ch), c.note, vol * 17, -1);
          }
        }
        // regs 0-5: period bytes; emit new note if pitch changed while playing
        if (reg < 6) {
          int ch = reg / 2;
          AYChan& c = ay[ch];
          if (c.noteOn) {
            c.period = ((ayRegs[ch*2+1] & 0x0F) << 8) | ayRegs[ch*2];
            int note = ayPeriodToNote(c.period, ayClock ? ayClock : 1789773);
            if (note >= 0 && note != c.note) { c.note=note; emitNoteOn(ayChan_(ch),note,c.vol*17,-1); }
          }
        }
        continue;
      } else if (op == OP_AY) { p+=2; continue; }

      if (op == OP_GB && hasGB) {
        if (p+2>len) break;
        uint8_t reg = file[p++], dat = file[p++];
        // GB regs relative to 0xFF00: NR13/14=CH1 freq, NR23/24=CH2, NR33/34=CH3, NR44=CH4
        if (reg == 0x13) gb[0].freq = (gb[0].freq & 0x700) | dat;
        if (reg == 0x14) {
          gb[0].freq = ((uint16_t)(dat & 7) << 8) | (gb[0].freq & 0xFF);
          if (dat & 0x80) { int n=gbFreqToNote(gb[0].freq); gb[0].noteOn=(n>=0); gb[0].note=n; if(n>=0)emitNoteOn(gbChan_(0),n,200,-1); }
          if (!(dat & 0x40) && gb[0].noteOn) { gb[0].noteOn=false; emitNoteOff(gbChan_(0)); }
        }
        if (reg == 0x18) gb[1].freq = (gb[1].freq & 0x700) | dat;
        if (reg == 0x19) {
          gb[1].freq = ((uint16_t)(dat & 7) << 8) | (gb[1].freq & 0xFF);
          if (dat & 0x80) { int n=gbFreqToNote(gb[1].freq); gb[1].noteOn=(n>=0); gb[1].note=n; if(n>=0)emitNoteOn(gbChan_(1),n,200,-1); }
          if (!(dat & 0x40) && gb[1].noteOn) { gb[1].noteOn=false; emitNoteOff(gbChan_(1)); }
        }
        if (reg == 0x1D) gb[2].freq = (gb[2].freq & 0x700) | dat;
        if (reg == 0x1E) {
          gb[2].freq = ((uint16_t)(dat & 7) << 8) | (gb[2].freq & 0xFF);
          if (dat & 0x80) { int n=gbWaveFreqToNote(gb[2].freq); gb[2].noteOn=(n>=0); gb[2].note=n; if(n>=0)emitNoteOn(gbChan_(2),n,200,-1); }
          if (!(dat & 0x40) && gb[2].noteOn) { gb[2].noteOn=false; emitNoteOff(gbChan_(2)); }
        }
        if (reg == 0x23) {
          if (dat & 0x80) { gb[3].noteOn=true; emitNoteOn(gbChan_(3),48,200,-1); }
          if (!(dat & 0x40) && gb[3].noteOn) { gb[3].noteOn=false; emitNoteOff(gbChan_(3)); }
        }
        continue;
      } else if (op == OP_GB) { p+=2; continue; }

      if (op >= 0x51 && op <= 0x5F) { if (p+2<=len) p+=2; continue; }
      if (op >= 0xA1 && op <= 0xBF) { if (p+2<=len) p+=2; continue; }
      if (op == OP_NES || op == OP_PCE) { if (p+2<=len) p+=2; continue; }
      if (op >= 0xC0 && op <= 0xDF) { if (p+3<=len) p+=3; continue; }
      if (op >= 0xE0) { if (p+4<=len) p+=4; continue; }
      if (op==0x90||op==0x91) { if(p+4<=len)p+=4; continue; }
      if (op==0x92) { if(p+5<=len)p+=5; continue; }
      if (op==0x93) { if(p+10<=len)p+=10; continue; }
      if (op==0x94) { if(p+1<=len)p++; continue; }
      if (op==0x95) { if(p+4<=len)p+=4; continue; }
    }

    logI("[VGM] %zu events, %zu instruments", events.size(), instruments.size());

    DivSong ds;
    ds.version = DIV_VERSION_VGM;

    // read track name from GD3 tag (UTF-16LE, first null-terminated string)
    uint32_t gd3Off = rl32(file + VGM_OFF_GD3);
    if (gd3Off && (VGM_OFF_GD3 + gd3Off + 12) < len) {
      size_t gd3Pos = VGM_OFF_GD3 + gd3Off;
      if (memcmp(file + gd3Pos, "Gd3 ", 4) == 0) {
        size_t strOff = gd3Pos + 12;
        std::string name;
        while (strOff + 1 < len) {
          uint16_t wc = rl16(file + strOff); strOff += 2;
          if (wc == 0) break;
          if (wc < 128) name += (char)wc;
        }
        if (!name.empty()) ds.name = name;
      }
    }
    if (ds.name.empty()) ds.name = "Imported VGM";

    uint32_t totalRows = (uint32_t)ceil((double)totalSamp / sampPerRow);
    uint32_t patCount  = (totalRows + VGM_PAT_LEN - 1) / VGM_PAT_LEN;
    if (patCount < 1) patCount = 1;
    if (patCount > 128) patCount = 128;

    ds.subsong[0]->hz             = vgmHz;
    ds.subsong[0]->speeds.val[0]  = 1;
    ds.subsong[0]->speeds.len     = 1;
    ds.subsong[0]->patLen         = VGM_PAT_LEN;
    ds.subsong[0]->ordersLen      = (int)patCount;
    for (int i = 0; i < (int)patCount && i < 128; i++)
      for (int ch = 0; ch < totalChans; ch++)
        ds.subsong[0]->orders.ord[ch][i] = (unsigned char)i;

    ds.systemLen = 0;
    auto addSys = [&](DivSystem sys, const char* sname, int base, int count) {
      if (ds.systemLen >= DIV_MAX_CHIPS) return;
      ds.system[ds.systemLen++] = sys;
      ds.systemName = sname;
      for (int i = base; i < base + count && i < totalChans; i++) {
        ds.subsong[0]->chanShow[i] = ds.subsong[0]->chanShowChanOsc[i] = true;
      }
    };

    if (hasSN)   addSys(DIV_SYSTEM_SMS,    "Sega Master System", snBase,   snChans);
    if (hasOPN2) addSys(DIV_SYSTEM_YM2612, "Sega Genesis",       opn2Base, opn2Chans);
    if (hasOPN)  addSys(DIV_SYSTEM_YM2203, "YM2203",             opnBase,  opnChans);
    if (hasOPNA) addSys(DIV_SYSTEM_YM2612, "YM2608/OPNA",        opnaBase, opnaChans);
    if (hasOPM)  addSys(DIV_SYSTEM_YM2151, "YM2151/OPM",         opmBase,  opmChans);
    if (hasOPL2) addSys(DIV_SYSTEM_OPL2,   "YM3812/OPL2",        opl2Base, opl2Chans);
    if (hasOPL3) addSys(DIV_SYSTEM_OPL3,   "YMF262/OPL3",        opl3Base, opl3Chans);
    if (hasAY)   addSys(DIV_SYSTEM_AY8910, "AY8910",             ayBase,   ayChans);
    if (hasGB)   addSys(DIV_SYSTEM_GB,     "Game Boy",           gbBase,   gbChans);

    if (ds.systemLen == 0) { lastError = "no recognized chips in VGM"; return false; }

    auto nameRange = [&](int base, int count, const char* prefix) {
      for (int i = 0; i < count && base + i < totalChans; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s%d", prefix, i + 1);
        ds.subsong[0]->chanName[base+i] = buf;
        snprintf(buf, sizeof(buf), "%c%d", prefix[0], i + 1);
        ds.subsong[0]->chanShortName[base+i] = buf;
      }
    };
    if (hasSN)   nameRange(snBase,   snChans,   "PSG");
    if (hasOPN2) nameRange(opn2Base, opn2Chans, "FM");
    if (hasOPM)  nameRange(opmBase,  opmChans,  "OPM");
    if (hasOPL2) nameRange(opl2Base, opl2Chans, "OPL");
    if (hasAY)   nameRange(ayBase,   ayChans,   "AY");
    if (hasGB)   nameRange(gbBase,   gbChans,   "GB");

    for (int i = 0; i < totalChans; i++) ds.subsong[0]->pat[i].effectCols = 1;
    for (int i = totalChans; i < DIV_MAX_CHANS; i++) {
      ds.subsong[0]->chanShow[i] = ds.subsong[0]->chanShowChanOsc[i] = false;
      ds.subsong[0]->pat[i].effectCols = 1;
    }

    ds.ins    = instruments;
    ds.insLen = (int)ds.ins.size();

    int snInsIdx = ds.insLen;
    { DivInstrument* si=new DivInstrument; si->type=DIV_INS_FM;  si->name="PSG"; ds.ins.push_back(si); ds.insLen++; }
    int ayInsIdx = ds.insLen;
    { DivInstrument* ai=new DivInstrument; ai->type=DIV_INS_AY;  ai->name="AY";  ds.ins.push_back(ai); ds.insLen++; }
    int gbInsIdx = ds.insLen;
    { DivInstrument* gi=new DivInstrument; gi->type=DIV_INS_GB;  gi->name="GB";  ds.ins.push_back(gi); ds.insLen++; }

    for (auto& ev: events) {
      if (ev.ins >= 0) continue;
      if (hasSN && ev.chan >= snBase && ev.chan < snBase + snChans)    ev.ins = snInsIdx;
      if (hasAY && ev.chan >= ayBase && ev.chan < ayBase + ayChans)    ev.ins = ayInsIdx;
      if (hasGB && ev.chan >= gbBase && ev.chan < gbBase + gbChans)    ev.ins = gbInsIdx;
    }

    int lastIns[DIV_MAX_CHANS];
    memset(lastIns, -1, sizeof(lastIns));

    for (auto& ev: events) {
      uint32_t rowGlobal = (uint32_t)(ev.tick / sampPerRow);
      int pat = (int)(rowGlobal / VGM_PAT_LEN);
      int row = (int)(rowGlobal % VGM_PAT_LEN);
      if (pat >= (int)patCount || ev.chan >= totalChans) continue;

      short* cell = ds.subsong[0]->pat[ev.chan].getPattern(pat, true)->newData[row];
      if (ev.type == VGM_NOTE_ON) {
        cell[DIV_PAT_NOTE] = (short)ev.note;
        if (ev.ins >= 0) { cell[DIV_PAT_INS] = (short)ev.ins; lastIns[ev.chan] = ev.ins; }
        else if (lastIns[ev.chan] >= 0) cell[DIV_PAT_INS] = (short)lastIns[ev.chan];
        if (ev.vol >= 0) cell[DIV_PAT_VOL] = (short)(ev.vol & 0xFF);
      } else {
        cell[DIV_PAT_NOTE] = DIV_NOTE_OFF;
      }
    }

    ds.initDefaultSystemChans();
    ds.recalcChans();
    ds.findSubSongs();

    if (active) quitDispatch();
    BUSY_BEGIN_SOFT;
    saveLock.lock();
    song.unload();
    song = ds;
    hasLoadedSomething = true;
    changeSong(0);
    saveLock.unlock();
    BUSY_END;
    if (active) {
      initDispatch();
      BUSY_BEGIN;
      renderSamples();
      reset();
      BUSY_END;
    }
    success = true;

  } catch (BadVGM&) {
    lastError = "invalid or truncated VGM file";
  }
  return success;
}
