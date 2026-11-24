// SPDX-License-Identifier: MIT
#include "ApplicationServices.h"
#include "Common.h"
#include "ImageConvert.h"
#include "JobSnapshot.h"
#include "MessageStore.h"
#include "SavedPlaces.h"
#include "SavedSearches.h"
#include "VirtualFile.h"
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>
#include <Roster.h>
#include <cstring>

static std::string Quote(const std::string &s) {
  std::string out = "'";
  for (char c : s)
    out += c == '\'' ? "'\\''" : std::string(1, c);
  return out + "'";
}
BString SettingsPath(const char *name) {
  BPath p;
  find_directory(B_USER_SETTINGS_DIRECTORY, &p);
  p.Append(name);
  return p.Path();
}
namespace {
class Services : public BLooper {
  BMessenger app;
  const bool preferencesWritable;

public:
  Services(BMessenger a, bool canSavePreferences)
      : BLooper("file viewer and launcher"), app(a),
        preferencesWritable(canSavePreferences) {}
  void MessageReceived(BMessage *m) override {
    if (m->what == kDrainServices) {
      BMessage reply(B_REPLY);
      m->SendReply(&reply);
      return;
    }
    if (m->what == kImageDropProbe) {
      BMessenger target;
      if (m->FindMessenger("replyTo", &target) != B_OK)
        return;
      BMessage reply(*m);
      reply.what = kImageDropOutputs;
      try {
        auto outputs = Listi::ImageOutputs(Listi::DecodeJob(*m));
        for (const auto &output : outputs) {
          BMessage choice;
          choice.AddInt32("type", output.type);
          choice.AddString("mime", output.mime.c_str());
          choice.AddString("name", output.name.c_str());
          choice.AddString("extension", output.extension.c_str());
          reply.AddMessage("output", &choice);
        }
        if (outputs.empty())
          reply.AddString(
              "error",
              "No installed translator can encode another image format.");
      } catch (const std::exception &e) {
        reply.AddString("error", e.what());
      }
      target.SendMessage(&reply, (BHandler *)nullptr, 500000);
      return;
    }
    if (m->what == kSaveSearches) {
      BMessage reply(kSearchesSaved);
      reply.AddInt64("revision", m->GetInt64("revision", -1));
      reply.AddInt64("serial", m->GetInt64("serial", -1));
      BMessenger origin;
      if (m->FindMessenger("origin", &origin) == B_OK)
        reply.AddMessenger("origin", origin);
      try {
        auto file = SettingsPath("Lister_searches");
        auto current = Listi::SavedSearches::Load(file.String());
        current.ApplyEdit(*m);
        auto snapshot = current.Message();
        current.Save(file.String());
        reply.AddMessage("snapshot", &snapshot);
      } catch (const std::exception &e) {
        std::string error =
            "Could not save search criteria. The previous list is unchanged. ";
        error += e.what();
        reply.AddString("error", error.c_str());
      }
      app.SendMessage(&reply);
      return;
    }
    if (m->what == kSavePlaces) {
      BMessage reply(kPlacesSaved);
      reply.AddInt64("serial", m->GetInt64("serial", -1));
      BMessenger origin;
      if (m->FindMessenger("origin", &origin) == B_OK)
        reply.AddMessenger("origin", origin);
      try {
        auto file = SettingsPath("Lister_places");
        auto current = Listi::SavedPlaces::Load(file.String());
        current.ApplyEdit(*m);
        auto snapshot = current.Message();
        current.Save(file.String());
        reply.AddMessage("snapshot", &snapshot);
      } catch (const std::exception &e) {
        std::string error =
            "Could not save places. The previous list is unchanged. ";
        error += e.what();
        reply.AddString("error", error.c_str());
      }
      app.SendMessage(&reply);
      return;
    }
    if (m->what == kSavePreferences) {
      if (!preferencesWritable)
        return;
      try {
        BMessage data(*m);
        data.RemoveName("origin");
        Listi::SaveMessageStore(SettingsPath().String(), data);
      } catch (const std::exception &e) {
        BMessage error(kServiceError);
        BMessenger origin;
        if (m->FindMessenger("origin", &origin) == B_OK)
          error.AddMessenger("origin", origin);
        error.AddString(
            "error",
            (std::string("Could not save preferences: ") + e.what()).c_str());
        app.SendMessage(&error);
      }
      return;
    }
    if (m->what != kOpenPaths) {
      BLooper::MessageReceived(m);
      return;
    }
    try {
      BString path = m->GetString("path", ""),
              mode = m->GetString("mode", "open");
      BMessenger pane;
      m->FindMessenger("pane", &pane);
      if (mode == "open") {
        auto archive = Listi::OpenArchive(path.String());
        if (!archive.empty() && pane.IsValid()) {
          BMessage navigate(kNavigate);
          navigate.AddString("path", archive.c_str());
          navigate.AddInt64("serial", m->GetInt64("serial", -1));
          pane.SendMessage(&navigate);
          return;
        }
      }
      if (Listi::IsVirtual(path.String()))
        throw std::runtime_error(
            "Copy this entry locally before editing or opening it with an application. Remote edits are not uploaded automatically.");
      status_t s = B_OK;
      if (mode == "terminal" || mode == "command") {
        BString application =
            m->GetString("application", "/boot/system/apps/Terminal");
        entry_ref ref;
        s = get_ref_for_path(application, &ref);
        if (s == B_OK && mode == "terminal" &&
            application == "/boot/system/apps/Terminal") {
          const char *args[] = {"-t", "Listi CLI", "-w", path.String()};
          s = be_roster->Launch(&ref, 4, args);
        } else if (s == B_OK) {
          std::string script = "cd -- " + Quote(path.String()) + " || exit 1\n";
          if (mode == "command") {
            script += m->GetString("command", "");
            script += '\n';
          }
          script += "exec /bin/bash";
          const char *args[] = {"-t", "Listi CLI", "/bin/bash", "-c",
                                script.c_str()};
          s = be_roster->Launch(&ref, 5, args);
        }
      } else {
        entry_ref ref;
        s = get_ref_for_path(path, &ref);
        if (s == B_OK) {
          BMessage refs(B_REFS_RECEIVED);
          refs.AddRef("refs", &ref);
          if (mode == "edit") {
            entry_ref editor;
            s = get_ref_for_path(
                m->GetString("application", "/boot/system/apps/StyledEdit"),
                &editor);
            if (s == B_OK)
              s = be_roster->Launch(&editor, &refs);
          } else
            s = be_roster->Launch(&ref);
        }
      }
      if (s != B_OK && s != B_ALREADY_RUNNING) {
        BMessage error(kServiceError);
        BMessenger origin;
        if (m->FindMessenger("origin", &origin) == B_OK)
          error.AddMessenger("origin", origin);
        BString text("Could not open application: ");
        text << strerror(s);
        error.AddString("error", text);
        app.SendMessage(&error);
      }
    } catch (const Listi::Cancelled &) {
      return;
    } catch (const std::exception &e) {
      BMessage error(kServiceError);
      BMessenger origin;
      if (m->FindMessenger("origin", &origin) == B_OK)
        error.AddMessenger("origin", origin);
      error.AddString("error", e.what());
      app.SendMessage(&error);
    }
  }
};
} // namespace

BLooper *CreateApplicationServices(BMessenger app, bool canSavePreferences) {
  return new Services(app, canSavePreferences);
}
