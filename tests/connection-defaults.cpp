// SPDX-License-Identifier: MIT
#include "ConnectionView.h"
#include "Common.h"
#include "VirtualFile.h"
#include <Application.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr uint32 kTestClose = 'ctcl';

class ConnectionReceiver : public BLooper {
public:
  std::atomic<int> navigations{0}, closes{0};
  ConnectionReceiver() : BLooper("connection test receiver") {}
  void MessageReceived(BMessage *message) override {
    if (message->what == kNavigate)
      ++navigations;
    else if (message->what == kTestClose)
      ++closes;
    else
      BLooper::MessageReceived(message);
  }
};

bool WaitFor(const std::function<bool()> &condition) {
  for (int i = 0; i < 100; ++i) {
    if (condition())
      return true;
    snooze(20000);
  }
  return false;
}

bool MissingConnection(const std::string &url) {
  try {
    Listi::RequireConnection(url);
    return false;
  } catch (const std::runtime_error &) {
    return true;
  }
}

// The receiver stands in for the originating pane so no network is involved.
bool CheckPopup(BApplication &app, const std::string &identity,
                const std::string &knownHosts) {
  auto receiver = new ConnectionReceiver();
  receiver->Run();
  BMessenger events(receiver);
  auto owner = new BWindow(BRect(100, 100, 700, 340), "Connect test",
                           B_TITLED_WINDOW, 0);
  constexpr char remote[] = "sftp://listi-passphrase-test.invalid/";
  auto createForm = [&] {
    auto *view = CreateConnectionView(events, events, BMessage(kTestClose));
    owner->AddChild(view);
    auto field = [&](const char *name) {
      return dynamic_cast<BTextControl *>(view->FindView(name));
    };
    field("network address")->SetText(remote);
    field("network username")->SetText("test-user");
    field("network password")->SetText("server-password");
    field("ssh key")->SetText(identity.c_str());
    field("known hosts")->SetText(knownHosts.c_str());
    return view;
  };
  auto *form = createForm();
  owner->Show();
  auto prompt = [&]() -> BWindow * {
    for (int32 i = 0; i < app.CountWindows(); ++i) {
      auto *window = app.WindowAt(i);
      if (window && window != owner)
        return window;
    }
    return nullptr;
  };
  auto connect = [&] {
    BMessage message('conn');
    form->MessageReceived(&message);
  };
  owner->Lock();
  connect();
  connect();
  auto popup = prompt();
  bool ok = popup && app.CountWindows() == 2 &&
            receiver->navigations == 0 &&
            MissingConnection(remote);
  if (popup && popup->Lock()) {
    auto *field = dynamic_cast<BTextControl *>(popup->FindView("unlock ssh key"));
    ok &= field && field->TextView()->IsTypingHidden();
    popup->PostMessage(B_QUIT_REQUESTED);
    popup->Unlock();
  }
  owner->Unlock();
  ok &= WaitFor([&] { return app.CountWindows() == 1; });
  // Drain the canceled prompt's asynchronous result before trying again.
  BMessage frame(B_GET_PROPERTY), reply;
  frame.AddSpecifier("Frame");
  ok &= BMessenger(owner).SendMessage(&frame, &reply, 2000000, 2000000) == B_OK;
  owner->Lock();
  auto *user = dynamic_cast<BTextControl *>(form->FindView("network username"));
  ok &= user && std::string(user->Text()) == "test-user" &&
        receiver->navigations == 0 && receiver->closes == 0 &&
        MissingConnection(remote);
  connect();
  popup = prompt();
  owner->Unlock();
  if (popup && popup->Lock()) {
    auto *field = dynamic_cast<BTextControl *>(popup->FindView("unlock ssh key"));
    if (field) {
      field->SetText("key-only-passphrase");
      popup->PostMessage('nukp');
    }
    popup->Unlock();
  } else {
    ok = false;
  }
  ok &= WaitFor([&] { return receiver->navigations == 1 &&
                             receiver->closes == 1 && app.CountWindows() == 1; });
  const auto auth = Listi::GetConnection(remote);
  ok &= auth.username == "test-user" && auth.password == "server-password" &&
        auth.keyPassphrase == "key-only-passphrase" &&
        auth.privateKey == identity && auth.knownHosts == knownHosts;

  owner->Lock();
  owner->RemoveChild(form);
  delete form;
  form = createForm();
  connect();
  ok &= prompt() != nullptr;
  owner->RemoveChild(form);
  delete form;
  owner->Unlock();
  ok &= WaitFor([&] { return app.CountWindows() == 1; });
  ok &= receiver->navigations == 1 && receiver->closes == 1;

  owner->Lock();
  form = createForm();
  auto *inlinePassphrase =
      dynamic_cast<BTextControl *>(form->FindView("key passphrase"));
  if (inlinePassphrase) {
    inlinePassphrase->SetText("key-only-passphrase");
    connect();
    ok &= !prompt();
  } else {
    ok = false;
  }
  owner->Unlock();
  ok &= WaitFor([&] { return receiver->navigations == 2 &&
                             receiver->closes == 2; });
  ok &= Listi::GetConnection(remote).keyPassphrase == "key-only-passphrase";
  ok &= Listi::GetConnection("sftp://LISTI-PASSPHRASE-TEST.INVALID:22/other/entry").username ==
        auth.username;
  Listi::Connection second;
  second.username = "other-fixture-user";
  const auto secondRoute = "sftp://second-listi-test.invalid/";
  ok &= Listi::SetConnection(secondRoute, second) &&
        Listi::GetConnection(secondRoute).username == second.username &&
        Listi::GetConnection(remote).username == auth.username;
  owner->Lock();
  owner->Quit();
  if (receiver->Lock())
    receiver->Quit();
  return ok;
}
} // namespace

int main() {
  BApplication app("application/x-vnd.Listi-ConnectionDefaultsTest");
  char home[] = "/tmp/listi-ssh-XXXXXX";
  if (!mkdtemp(home) || setenv("HOME", home, 1) != 0)
    return 1;
  const std::string directory = std::string(home) + "/.ssh";
  if (mkdir(directory.c_str(), 0700) != 0)
    return 1;
  std::string expectedHosts = std::string(home) + "/config/settings/ssh/known_hosts";
  auto check = [&](const std::string &expected) {
    std::unique_ptr<BView> view(CreateConnectionView({}, {}, BMessage()));
    auto key = dynamic_cast<BTextControl *>(view->FindView("ssh key"));
    auto hosts = dynamic_cast<BTextControl *>(view->FindView("known hosts"));
    return key && key->Text() == expected && hosts &&
           hosts->Text() == expectedHosts;
  };
  const auto rsa = directory + "/id_rsa", ed = directory + "/id_ed25519";
  std::ofstream(ed + ".pub") << "public key only";
  bool ok = check("");
  std::ofstream(rsa) << "test";
  ok &= check(rsa);
  std::ofstream(ed) << "test";
  ok &= check(ed);
  const auto native = std::string(home) + "/config/settings/ssh";
  mkdir((std::string(home) + "/config").c_str(), 0700);
  mkdir((std::string(home) + "/config/settings").c_str(), 0700);
  mkdir(native.c_str(), 0700);
  std::ofstream(directory + "/known_hosts") << "test";
  expectedHosts = directory + "/known_hosts";
  ok &= check(ed);
  std::ofstream(native + "/id_ed25519") << "test";
  ok &= check(native + "/id_ed25519");
  std::ofstream(native + "/known_hosts") << "test";
  expectedHosts = native + "/known_hosts";
  ok &= check(native + "/id_ed25519");
  const auto encrypted = directory + "/encrypted.pem";
  // Header-only fixture; contains no test private key or encrypted key data.
  std::ofstream(encrypted) << "-----BEGIN RSA PRIVATE KEY-----\n"
                              "Proc-Type: 4,ENCRYPTED\n"
                              "DEK-Info: AES-128-CBC,00000000000000000000000000000000\n"
                              "\n"
                              "AAAA\n"
                              "-----END RSA PRIVATE KEY-----\n";
  const bool popup = CheckPopup(app, encrypted, expectedHosts);
  unlink(encrypted.c_str());
  unlink((directory + "/known_hosts").c_str());
  unlink((native + "/known_hosts").c_str());
  unlink((native + "/id_ed25519").c_str());
  rmdir(native.c_str());
  rmdir((std::string(home) + "/config/settings").c_str());
  rmdir((std::string(home) + "/config").c_str());
  unlink(ed.c_str());
  unlink(rsa.c_str());
  unlink((ed + ".pub").c_str());
  rmdir(directory.c_str());
  rmdir(home);
  puts(ok ? "PASS editable SSH path defaults" : "FAIL SSH path defaults");
  puts(popup ? "PASS encrypted SSH key passphrase prompt"
             : "FAIL encrypted SSH key passphrase prompt");
  return ok && popup ? 0 : 1;
}
