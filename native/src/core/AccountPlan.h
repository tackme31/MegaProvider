#pragma once

// src/core and src/qml can't include SDK headers, so this mirrors
// mega::MegaAccountDetails' ACCOUNT_TYPE_* enum the same way MegaErrorCodes.h
// mirrors mega::MegaError. Kept in sync via a static_assert block in
// src/mega/MegaSdkClient.cpp -- the only file allowed to see both.
//
// The gaps (5..10, 14..99) are the SDK's, not a transcription error. kFeature
// is a real value MEGA uses for feature-only purchases; anything unrecognised
// must still render, so the QML that maps these to plan names needs a
// printable default rather than a blank.
namespace AccountPlan
{
constexpr int kFree = 0;
constexpr int kProI = 1;
constexpr int kProII = 2;
constexpr int kProIII = 3;
constexpr int kLite = 4;
constexpr int kStarter = 11;
constexpr int kBasic = 12;
constexpr int kEssential = 13;
constexpr int kBusiness = 100;
constexpr int kProFlexi = 101; // also known as PRO 4
constexpr int kFeature = 99999;
} // namespace AccountPlan
