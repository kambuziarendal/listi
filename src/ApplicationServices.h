// SPDX-License-Identifier: MIT
#pragma once
#include <Looper.h>
#include <Messenger.h>
#include <String.h>

constexpr uint32 kImageDropProbe = 'dimg', kImageDropOutputs = 'dfmt';
constexpr uint32 kServiceError = 'serr', kSavePreferences = 'save', kDrainServices = 'svdr';

BString SettingsPath(const char *name = "Lister_settings");
BLooper *CreateApplicationServices(BMessenger app, bool canSavePreferences);
