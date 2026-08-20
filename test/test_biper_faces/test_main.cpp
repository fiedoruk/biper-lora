// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tomasz Fiedoruk
//
// Prawda doreczen na ekranie kostki — logika czysta (BiperLogic.h), na hoscie.
// Przypadki wprost z audytu Codexa (F-03/F-04/F-19): spoznione potwierdzenie,
// dwa oczekiwania naraz, potwierdzenie kanalowe (nigdy nie rejestrowane),
// duplikat ACK, wyparcie najstarszego wpisu, podtrzymanie okna hotspotu.
// Pelna kolejka RX i takeover sesji zyja w kodzie zaleznym od ESP —
// tam pozostaja NOT MEASURED na hoscie i sa kryte przegladem + para na biurku.

#include <gtest/gtest.h>
#include "helpers/biper/BiperLogic.h"

static const uint8_t TAG_A[4] = {0x11, 0x22, 0x33, 0x44};
static const uint8_t TAG_B[4] = {0xAA, 0xBB, 0xCC, 0xDD};

TEST(PendingTags, ConfirmMatchesOnlyAwaitedTag) {
  BiperPendingTags t;
  t.add(TAG_A, 1000);
  EXPECT_FALSE(t.confirm(TAG_B, 1500));  // cudze potwierdzenie: ignorowane
  EXPECT_TRUE(t.confirm(TAG_A, 2000));   // nasze: DOSZLO zasluzone
}

TEST(PendingTags, DuplicateAckIsIgnored) {
  BiperPendingTags t;
  t.add(TAG_A, 1000);
  EXPECT_TRUE(t.confirm(TAG_A, 2000));
  // Firmware ostrzega, ze ten sam ACK potrafi przyjsc kilka razy.
  EXPECT_FALSE(t.confirm(TAG_A, 2100));
}

TEST(PendingTags, LateAckAfterTtlIsIgnored) {
  BiperPendingTags t;
  t.add(TAG_A, 1000);
  // Po minucie ekran wrocil do ZYJE — potwierdzenie nie ma juz czego domykac.
  EXPECT_FALSE(t.confirm(TAG_A, 1000 + 60001));
}

TEST(PendingTags, ChannelSendRegistersNothing) {
  BiperPendingTags t;
  // Kanal publiczny nie rejestruje znacznika; jego "potwierdzenie" nie istnieje.
  EXPECT_FALSE(t.confirm(TAG_A, 500));
  EXPECT_EQ(t.count(500), 0);
}

TEST(PendingTags, TwoPendingConfirmOneKeepsOther) {
  BiperPendingTags t;
  t.add(TAG_A, 1000);
  t.add(TAG_B, 1100);
  EXPECT_TRUE(t.confirm(TAG_A, 2000));
  EXPECT_EQ(t.count(2000), 1);           // B nadal czeka
  EXPECT_TRUE(t.confirm(TAG_B, 2500));
}

TEST(PendingTags, OverflowEvictsOldest) {
  BiperPendingTags t;
  uint8_t tag[4] = {0, 0, 0, 0};
  for (int i = 0; i < BiperPendingTags::MAX_TAGS + 1; i++) {
    tag[0] = (uint8_t)i;
    t.add(tag, 1000 + (uint32_t)i);
  }
  tag[0] = 0;                             // najstarszy: wyparty
  EXPECT_FALSE(t.confirm(tag, 2000));
  tag[0] = BiperPendingTags::MAX_TAGS;    // najnowszy: zyje
  EXPECT_TRUE(t.confirm(tag, 2000));
}

TEST(WindowKeepalive, StationAloneDoesNotHoldWindow) {
  // Zapamietany laptop: stacja skojarzona, panel nigdy nie otwarty.
  EXPECT_FALSE(biper_window_keepalive(1, -1, 100000, 0, 180000));
}

TEST(WindowKeepalive, LivePanelHoldsWindow) {
  EXPECT_TRUE(biper_window_keepalive(1, 57, 100000, 95000, 180000));
}

TEST(WindowKeepalive, SleepingPhoneStopsHolding) {
  // Puls ustal 3 minuty temu (tab uspiony / gniazdo martwe).
  EXPECT_FALSE(biper_window_keepalive(1, 57, 400000, 100000, 180000));
}

TEST(WindowKeepalive, NoGuestsNeverHolds) {
  EXPECT_FALSE(biper_window_keepalive(0, 57, 100000, 99000, 180000));
}

TEST(WindowKeepalive, SocketWithoutAnyFrameNeverHolds) {
  // fd otwarty, ale klient nigdy sie nie odezwal (last_rx==0).
  EXPECT_FALSE(biper_window_keepalive(1, 57, 100000, 0, 180000));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
