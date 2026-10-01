// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "VUCommunication.h"

#include <gtest/gtest.h>

TEST(VUCommunication, EarlierActiveUnitAndTie)
{
	EXPECT_EQ(VUCommunication::SelectUnit(true, true, 100, 101), 0u);
	EXPECT_EQ(VUCommunication::SelectUnit(true, true, 100, 100), 0u);
	EXPECT_EQ(VUCommunication::SelectUnit(true, true, 101, 100), 1u);
	EXPECT_EQ(VUCommunication::SelectUnit(false, true, 0, 100), 1u);
	EXPECT_EQ(VUCommunication::SelectUnit(true, false, 100, 0), 0u);
}

TEST(VUCommunication, PendingVersionIsNotPublished)
{
	const int ready[] = {4, -1, -1, 0};
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 0), 3);
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 1), 3);
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 2), 3);
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 3), 3);
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 4), 0);
}

TEST(VUCommunication, SeveralOutstandingVersions)
{
	const int ready[] = {3, 4, 1, 2};
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 0), -1);
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 1), 2);
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 2), 3);
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 3), 0);
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 4), 1);
}

TEST(VUCommunication, AbsentInstancesRemainAbsent)
{
	const int ready[] = {-1, -1, -1, -1};
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 0), -1);
	EXPECT_EQ(VUCommunication::FindMatureFlagSlot(ready, 100), -1);
}
