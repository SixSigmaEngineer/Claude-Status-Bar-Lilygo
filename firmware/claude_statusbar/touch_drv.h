/*
 * touch_drv.h — dual-backend touch driver for the LilyGo T-Display S3 Long.
 *
 * The board shipped in two hardware revisions with different touch chips:
 *
 *   OLD (2023 - early 2026): touch is integrated in the AXS15231B display
 *     controller, read directly at I2C 0x3B. Touch reset shares GPIO16 with
 *     the LCD reset. No firmware upload involved.
 *
 *   NEW (~May 2026 on):  a separate Hynitron CST3530 chip at I2C 0x58
 *     (boot/ISP mode at 0x5A), reset on GPIO2. The chip stores its firmware
 *     in its own flash, but ships blank/stale: the HOST must verify and, if
 *     needed, upload a ~40KB firmware blob over I2C once (it persists).
 *     Protocol distilled from LilyGo's hyn_driver_for_esp32 (cst66xx family).
 *
 * touchDetect() probes the bus and picks the backend at runtime, so one
 * firmware supports both revisions. If neither chip answers, the sketch
 * falls back to its blind INT-pulse tap detection.
 *
 * The CST3530 firmware blob (cst3530_fw.h, from LilyGo's repo) is downloaded
 * by build_and_flash.{sh,ps1} next to this file. If absent, CST boards with
 * healthy firmware still work — we just can't (re)flash the touch chip.
 */
#pragma once
#include <Arduino.h>
#include <Wire.h>

#if defined(__has_include)
#if __has_include("cst3530_fw.h")
#include "cst3530_fw.h"
#define HAVE_CST_FW 1
#endif
#endif
#ifndef HAVE_CST_FW
#define HAVE_CST_FW 0
#endif

#define CST_MAIN_ADDR 0x58
#define CST_BOOT_ADDR 0x5A
#define CST_RST_PIN   2      // new-revision touch reset (TP_RST)
#define AXS_ADDR      0x3B
#define CST_FW_SIZE   (40 * 1024)

enum TouchBackend { TOUCH_NONE = 0, TOUCH_AXS, TOUCH_CST };

struct TouchSample {
  bool     valid;   // a report was read
  bool     down;    // finger currently on the panel
  int16_t  x, y;    // raw controller coordinates (portrait axis)
};

/* ---------------- low-level I2C ---------------- */

static bool twAck(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

static bool twWrite(uint8_t addr, const uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(buf, n);
  return Wire.endTransmission() == 0;
}

static bool twRead(uint8_t addr, uint8_t *buf, size_t n) {
  size_t got = Wire.requestFrom((int)addr, (int)n);
  for (size_t i = 0; i < got; i++) buf[i] = Wire.read();
  return got == n;
}

/* write the low `len` bytes of `reg`, big-endian (hyn_wr_reg encoding) */
static bool twReg(uint8_t addr, uint32_t reg, uint8_t len,
                  uint8_t *rbuf = nullptr, size_t rlen = 0) {
  uint8_t w[4];
  for (int i = len - 1; i >= 0; i--) { w[i] = reg & 0xFF; reg >>= 8; }
  if (!twWrite(addr, w, len)) return false;
  if (rlen) return twRead(addr, rbuf, rlen);
  return true;
}

/* ---------------- CST3530 (Hynitron cst66xx family) ---------------- */

static void cstReset() {
  pinMode(CST_RST_PIN, OUTPUT);
  digitalWrite(CST_RST_PIN, LOW);
  delay(8);
  digitalWrite(CST_RST_PIN, HIGH);
}

static bool cstWaitReady(uint16_t tries, uint8_t ms, uint16_t reg, uint16_t want) {
  uint8_t b[2];
  while (tries--) {
    if (twReg(CST_BOOT_ADDR, reg, 2, b, 2) &&
        ((uint16_t)(b[0] << 8) | b[1]) == want)
      return true;
    delay(ms);
  }
  return false;
}

static bool cstEnterBoot() {
  for (int retry = 1; retry < 20; retry++) {
    cstReset();
    delay(12 + retry);
    if (!twReg(CST_BOOT_ADDR, 0xA001A8, 3)) continue;
    if (cstWaitReady(10, 2, 0xA002, 0x22DD)) return true;
  }
  return false;
}

static void cstExitBoot() {
  cstReset();
  delay(60);
}

/* read 4 bytes from the chip's flash while in boot mode */
static uint32_t cstFlashReadWord(uint16_t addr) {
  for (int retry = 0; retry < 3; retry++) {
    bool ok = twReg(CST_BOOT_ADDR,
                    0xA0060000UL | ((uint32_t)(addr & 0xFF) << 8) | (addr >> 8), 4);
    ok &= twReg(CST_BOOT_ADDR, 0xA0080400, 4);
    ok &= twReg(CST_BOOT_ADDR, 0xA00A0300, 4);
    ok &= twReg(CST_BOOT_ADDR, 0xA004D2, 3);
    if (!ok) continue;
    uint8_t b[4];
    for (int w = 0; w < 20; w++) {
      if (twReg(CST_BOOT_ADDR, 0xA020, 2, b, 2) && b[0] == 0xD2 && b[1] == 0x88) {
        if (twReg(CST_BOOT_ADDR, 0xA040, 2, b, 4))
          return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) |
                 ((uint32_t)b[1] << 8) | b[0];
        break;
      }
      delay(1);
    }
  }
  return 0;
}

/* boot-mode checksum of a flash region */
static bool cstRegionChecksum(uint16_t startVal, uint16_t startAddr,
                              uint16_t len, uint32_t *sum) {
  for (int retry = 0; retry < 3; retry++) {
    bool ok = twReg(CST_BOOT_ADDR,
                    0xA0060000UL | ((uint32_t)(startAddr & 0xFF) << 8) | (startAddr >> 8), 4);
    ok &= twReg(CST_BOOT_ADDR,
                0xA0080000UL | ((uint32_t)(len & 0xFF) << 8) | (len >> 8), 4);
    ok &= twReg(CST_BOOT_ADDR,
                0xA00A0000UL | ((uint32_t)(startVal & 0xFF) << 8) | (startVal >> 8), 4);
    ok &= twReg(CST_BOOT_ADDR, 0xA004D6, 3);
    if (!ok) continue;
    delay(len / 0xC00 + 1);
    if (!cstWaitReady(20, 2, 0xA020, 0xD688)) continue;
    uint8_t b[5];
    if (twReg(CST_BOOT_ADDR, 0xA040, 2, b, 5) && b[0] == 0xCA) {
      *sum = ((uint32_t)b[4] << 24) | ((uint32_t)b[3] << 16) |
             ((uint32_t)b[2] << 8) | b[1];
      return true;
    }
  }
  return false;
}

/* full-chip checksum as the hyn driver computes it; ok=false if boot bad */
static uint32_t cstChipChecksum(bool *ok) {
  uint32_t s1 = 0, s2 = 0;
  *ok = false;
  if (cstRegionChecksum(0, 0, 0x9000, &s1) &&
      cstRegionChecksum(1, 0xB000, 0x0FFC, &s2)) {
    *ok = true;
    return s1 + s2 * 2 - 0x55;
  }
  return 0;
}

static bool cstSetNormalMode() {
  bool ok = true;
  ok &= twReg(CST_MAIN_ADDR, 0xD0000400, 4);   // disable lp i2c pull
  delay(1);
  twReg(CST_MAIN_ADDR, 0xD0000400, 4);
  ok &= twReg(CST_MAIN_ADDR, 0xD0000000, 4);
  ok &= twReg(CST_MAIN_ADDR, 0xD0000C00, 4);
  ok &= twReg(CST_MAIN_ADDR, 0xD0000100, 4);
  return ok;
}

struct CstInfo {
  uint32_t chipType, projectId, fwVer;
  uint16_t resX, resY;
};

/* read chip/config info in normal mode */
static bool cstTpInfo(CstInfo *inf) {
  uint8_t b[50];
  for (int retry = 0; retry < 5; retry++) {
    cstSetNormalMode();
    if (twReg(CST_MAIN_ADDR, 0xD0030000, 4, b, 50) &&
        b[3] == 0xCA && b[2] == 0xCA) {
      inf->chipType  = ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) |
                       ((uint32_t)b[1] << 8) | b[0];
      inf->projectId = ((uint32_t)b[39] << 24) | ((uint32_t)b[38] << 16) |
                       ((uint32_t)b[37] << 8) | b[36];
      inf->fwVer     = ((uint32_t)b[35] << 24) | ((uint32_t)b[34] << 16) |
                       ((uint32_t)b[33] << 8) | b[32];
      inf->resX = ((uint16_t)b[29] << 8) | b[28];
      inf->resY = ((uint16_t)b[31] << 8) | b[30];
      return true;
    }
    delay(1);
    twReg(CST_MAIN_ADDR, 0xD0000400, 4);
  }
  return false;
}

#if HAVE_CST_FW
static bool cstEraseRegion(uint16_t addr, uint16_t len, uint16_t type) {
  bool ok = true;
  ok &= twReg(CST_BOOT_ADDR, 0xA0060000UL | ((uint32_t)(addr & 0xFF) << 8) | (addr >> 8), 4);
  ok &= twReg(CST_BOOT_ADDR, 0xA0080000UL | ((uint32_t)(len & 0xFF) << 8) | (len >> 8), 4);
  ok &= twReg(CST_BOOT_ADDR, 0xA00A0000UL | ((uint32_t)(type & 0xFF) << 8) | (type >> 8), 4);
  ok &= twReg(CST_BOOT_ADDR, 0xA018CACA, 4);
  ok &= twReg(CST_BOOT_ADDR, 0xA004E0, 3);
  if (!ok) return false;
  delay(20);
  return cstWaitReady(100, 1, 0xA020, 0xE088);
}

/* upload fw_cst3530[] into the touch chip (persists in its flash).
 * Returns true on success. ~3-4 s. Only runs when the resident firmware
 * is missing or older. */
static bool cstUploadFw() {
  const uint8_t *fw = fw_cst3530;
  const uint8_t *p = fw + CST_FW_SIZE;
  uint32_t fwChecksum = ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
                        ((uint32_t)p[1] << 8) | p[0];
  static uint8_t pkt[1024 + 2];

  for (int attempt = 0; attempt < 3; attempt++) {
    if (!cstEnterBoot()) continue;
    if (!cstEraseRegion(0x0000, 0x8000, 0x02)) continue;   // 32K
    if (!cstEraseRegion(0x8000, 0x1000, 0x01)) continue;   // 4K
    if (!cstEraseRegion(0xB000, 0x1000, 0x04)) continue;   // 4K

    uint16_t eepAddr = 0;
    uint32_t off = 0;
    bool ok = true;
    for (int i = 0; i < CST_FW_SIZE / 1024 && ok; i++) {
      ok &= twReg(CST_BOOT_ADDR,
                  0xA0060000UL | ((uint32_t)(eepAddr & 0xFF) << 8) | (eepAddr >> 8), 4);
      ok &= twReg(CST_BOOT_ADDR, 0xA0080004, 4);
      ok &= twReg(CST_BOOT_ADDR, i >= 36 ? 0xA00A0300 : 0xA00A0000, 4);
      ok &= twReg(CST_BOOT_ADDR, 0xA018CACA, 4);
      pkt[0] = 0xA0; pkt[1] = 0x40;
      memcpy(pkt + 2, fw + off, 1024);
      ok &= twWrite(CST_BOOT_ADDR, pkt, sizeof(pkt));
      delay(5);
      ok &= twReg(CST_BOOT_ADDR, 0xA004E1, 3);
      off += 1024;
      eepAddr += 1024;
      if (eepAddr == 0x9000) eepAddr = 0xB000;
      delay(20);
      cstWaitReady(100, 1, 0xA020, 0xE188);
      if ((i & 7) == 0) Serial.printf("[touch] fw upload %d%%\n", i * 100 / 40);
    }
    if (!ok) continue;

    bool bootOk = false;
    uint32_t chipSum = cstChipChecksum(&bootOk);
    if (bootOk && chipSum == fwChecksum) {
      cstExitBoot();
      Serial.println("[touch] fw upload OK");
      return true;
    }
    Serial.printf("[touch] fw verify failed chip=%08x want=%08x\n",
                  (unsigned)chipSum, (unsigned)fwChecksum);
  }
  cstExitBoot();
  return false;
}
#endif  // HAVE_CST_FW

/* Decide whether the resident touch firmware needs (re)flashing and do it.
 * Mirrors cst66xx_init + cst66xx_updata_judge. */
static bool cstInit() {
  if (!cstEnterBoot()) {
    Serial.println("[touch] CST: enter boot failed");
    cstExitBoot();
    return false;
  }
  uint32_t partNo   = cstFlashReadWord(0xFF10);
  uint32_t moduleId = cstFlashReadWord(0xA400);
  bool bootOk = false;
  uint32_t chipSum = cstChipChecksum(&bootOk);
  Serial.printf("[touch] CST partNo=%08x moduleId=%08x chipSum=%08x bootOk=%d\n",
                (unsigned)partNo, (unsigned)moduleId, (unsigned)chipSum, bootOk);
  cstExitBoot();

  CstInfo inf = {};
  bool infoOk = cstTpInfo(&inf);
  if (infoOk)
    Serial.printf("[touch] CST fwVer=%08x project=%08x res=%dx%d\n",
                  (unsigned)inf.fwVer, (unsigned)inf.projectId, inf.resX, inf.resY);

#if HAVE_CST_FW
  const uint8_t *hdr = fw_cst3530 + 35 * 1024;
  uint32_t fIcType  = ((uint32_t)hdr[3] << 24) | ((uint32_t)hdr[2] << 16) |
                      ((uint32_t)hdr[1] << 8) | hdr[0];
  uint32_t fFwVer   = ((uint32_t)hdr[35] << 24) | ((uint32_t)hdr[34] << 16) |
                      ((uint32_t)hdr[33] << 8) | hdr[32];
  uint32_t fProject = ((uint32_t)hdr[39] << 24) | ((uint32_t)hdr[38] << 16) |
                      ((uint32_t)hdr[37] << 8) | hdr[36];
  bool needUpdate = !bootOk ||
                    (infoOk && fIcType == inf.chipType &&
                     fProject == inf.projectId && fFwVer > inf.fwVer);
  if (needUpdate) {
    Serial.println("[touch] CST firmware missing/stale - uploading (a few seconds)...");
    if (!cstUploadFw()) {
      Serial.println("[touch] CST fw upload FAILED");
      return false;
    }
    delay(50);
    infoOk = cstTpInfo(&inf);
  }
#else
  if (!bootOk || !infoOk) {
    Serial.println("[touch] CST fw bad and cst3530_fw.h not compiled in - no touch");
    return false;
  }
#endif
  return cstSetNormalMode();
}

/* read one CST report. Mirrors cst66xx_report (first finger only). */
static TouchSample cstRead() {
  TouchSample s = {};
  uint8_t buf[80];
  if (!twReg(CST_MAIN_ADDR, 0xD0070000, 4, buf, 9)) return s;
  uint8_t typ = buf[2];
  uint8_t fingers = buf[3] & 0x0F;
  uint8_t keys = (buf[3] & 0xF0) >> 4;
  uint8_t total = fingers + keys;
  if (total > 5) { twReg(CST_MAIN_ADDR, 0xD00002AB, 4); return s; }
  if (total > 1) twRead(CST_MAIN_ADDR, &buf[9], (total - 1) * 5);
  // checksum: sum16(0x55, buf+4, total*5) must equal buf[0] | buf[1]<<8
  uint16_t sum = 0x55;
  for (int i = 0; i < total * 5; i++) sum += buf[4 + i];
  bool sumOk = (total == 0) || sum == (uint16_t)(buf[0] | (buf[1] << 8));
  twReg(CST_MAIN_ADDR, 0xD00002AB, 4);   // ack/end
  if (!sumOk) return s;

  s.valid = true;
  if (typ == 0xFF && fingers > 0) {
    uint16_t idx = keys * 5;
    uint8_t event = buf[idx + 8] >> 4;
    s.x = buf[idx + 4] | ((uint16_t)(buf[idx + 7] & 0x0F) << 8);
    s.y = buf[idx + 5] | ((uint16_t)(buf[idx + 7] & 0xF0) << 4);
    s.down = event != 0;   // event 0 = lift
  }
  return s;
}

/* ---------------- AXS15231B integrated touch (old revision) ---------------- */

static TouchSample axsRead() {
  TouchSample s = {};
  static const uint8_t cmd[8] = {0xB5, 0xAB, 0xA5, 0x5A, 0x00, 0x00, 0x00, 0x08};
  uint8_t buf[14] = {0};
  if (!twWrite(AXS_ADDR, cmd, 8)) return s;
  if (!twRead(AXS_ADDR, buf, 14)) return s;
  uint8_t num = buf[1];
  uint8_t gesture = buf[0];
  s.valid = true;
  if (num > 0 && num <= 2 && gesture == 0) {
    uint8_t event = buf[2] >> 6;         // 0=down 1=up 2=contact
    s.x = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
    s.y = ((uint16_t)(buf[4] & 0x0F) << 8) | buf[5];
    s.down = event != 1;
  }
  return s;
}

/* ---------------- backend selection ---------------- */

static TouchBackend touchDetect() {
  if (twAck(CST_MAIN_ADDR) || twAck(CST_BOOT_ADDR)) return TOUCH_CST;
  if (twAck(AXS_ADDR)) return TOUCH_AXS;
  // CST might be asleep - kick its reset and re-probe
  cstReset();
  delay(60);
  if (twAck(CST_MAIN_ADDR) || twAck(CST_BOOT_ADDR)) return TOUCH_CST;
  return TOUCH_NONE;
}

static TouchSample touchRead(TouchBackend be) {
  if (be == TOUCH_CST) return cstRead();
  if (be == TOUCH_AXS) return axsRead();
  TouchSample s = {};
  return s;
}
