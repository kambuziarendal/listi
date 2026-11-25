// SPDX-License-Identifier: MIT
#pragma once

#include <SupportDefs.h>

#include <string>

class BHandler;
class BMessage;
class BView;
class BWindow;

constexpr uint32 kInspectorEmbeddedClose = 'ieEc';

void OpenInspectorEditor(const std::string &path, BWindow *owner);
bool InspectorEditorsAllowQuit(BWindow *owner = nullptr);

// The owning window must be locked. A non-null result is mounted in parent;
// requests to close the editor are posted to closeTarget.
BView *OpenEmbeddedInspectorEditor(const std::string &path, BView *parent,
                                   BHandler *closeTarget);
bool InspectorEditorAllowClose(BView *editor);
bool DetachInspectorEditor(BView *editor);
bool HandleInspectorEditorKey(BView *focus, const BMessage &key);
