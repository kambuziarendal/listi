// SPDX-License-Identifier: MIT
#include "ConnectionView.h"
#include "Common.h"
#include "ExplicitSubmit.h"
#include "SshKeyFormat.h"
#include "TextFieldFocus.h"
#include "SshPaths.h"
#include "VirtualFile.h"
#include "WrappedLabel.h"
#include <Button.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <TextControl.h>
#include <TextView.h>
#include <Window.h>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
namespace {
constexpr uint32 kConnect = 'conn', kAdvanced = 'nadv', kCancelConnect = 'nccl',
                 kUnlockKey = 'nukp', kPassphraseFinished = 'npfd',
                 kDismissPassphrase = 'npdq';

struct PassphraseAnswer {
  std::mutex mutex;
  std::string text;
};

class PassphraseWindow : public BWindow {
  BMessenger target;
  std::shared_ptr<PassphraseAnswer> answer;
  BTextControl *passphrase;

public:
  PassphraseWindow(BWindow *owner, BMessenger receiver,
                   std::shared_ptr<PassphraseAnswer> result)
      : BWindow(BRect(0, 0, 415, 125), "SSH key passphrase", B_TITLED_WINDOW_LOOK,
                B_MODAL_SUBSET_WINDOW_FEEL,
                B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE |
                    B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE),
        target(receiver), answer(std::move(result)) {
    passphrase = new BTextControl("unlock ssh key", "Passphrase:", "", nullptr);
    passphrase->TextView()->HideTyping(true);
    auto unlock = new BButton("Unlock", new BMessage(kUnlockKey));
    BLayoutBuilder::Group<>(this, B_VERTICAL, 10)
        .SetInsets(12)
        .Add(new WrappedLabel("unlock ssh key hint",
                              "Enter the passphrase for the SSH private key."))
        .Add(passphrase)
        .AddGlue()
        .AddGroup(B_HORIZONTAL, 8)
        .AddGlue()
        .Add(new BButton("Cancel", new BMessage(B_QUIT_REQUESTED)))
        .Add(unlock)
        .End();
    SetDefaultButton(unlock);
    Listi::FocusTextAtEnd(*passphrase);
    AddToSubset(owner);
    CenterIn(owner->Frame());
    MoveOnScreen();
  }

  bool QuitRequested() override {
    passphrase->SetText("");
    target.SendMessage(kPassphraseFinished);
    return true;
  }

  void MessageReceived(BMessage *message) override {
    if (message->what == kDismissPassphrase) {
      passphrase->SetText("");
      Quit();
      return;
    }
    if (message->what != kUnlockKey) {
      BWindow::MessageReceived(message);
      return;
    }
    if (!passphrase->Text()[0]) {
      passphrase->MakeFocus(true);
      return;
    }
    {
      std::lock_guard<std::mutex> lock(answer->mutex);
      answer->text = passphrase->Text();
    }
    passphrase->SetText("");
    target.SendMessage(kPassphraseFinished);
    Quit();
  }
};

class ConnectionView : public BGroupView {
  BMessenger pane, closeTarget;
  BMessage closeMessage;
  BTextControl *url, *user, *password, *domain, *key, *hosts, *keyPassphrase;
  WrappedLabel *error;
  BGroupView *advanced;
  bool expanded = false, closing = false;
  struct PendingConnection {
    std::string location;
    Listi::Connection auth;
    std::shared_ptr<PassphraseAnswer> answer;
    BMessenger window;
  };
  std::optional<PendingConnection> pending;

  void DismissPassphrase() {
    if (pending) {
      pending->window.SendMessage(kDismissPassphrase);
      pending.reset();
    }
  }
  void Close() {
    closing = true;
    DismissPassphrase();
    password->SetText("");
    keyPassphrase->SetText("");
    BMessage done(closeMessage);
    closeTarget.SendMessage(&done);
  }
  void Connect(const std::string &location, const Listi::Connection &auth) {
    if (!Listi::SetConnection(location, auth)) {
      error->SetMessage("This server already has a different connection. Restart Listi to change accounts or credentials.");
      password->SetText("");
      keyPassphrase->SetText("");
      return;
    }
    BMessage navigate(kNavigate);
    navigate.AddString("path", location.c_str());
    pane.SendMessage(&navigate);
    Close();
  }
  void AskPassphrase(std::string location, Listi::Connection auth) {
    auto answer = std::make_shared<PassphraseAnswer>();
    auto window = new PassphraseWindow(Window(), BMessenger(this), answer);
    pending.emplace(PendingConnection{std::move(location), std::move(auth),
                                      std::move(answer), BMessenger(window)});
    window->Show();
  }
  void FinishPassphrase() {
    if (!pending)
      return;
    auto connection = std::move(*pending);
    pending.reset();
    {
      std::lock_guard<std::mutex> lock(connection.answer->mutex);
      connection.auth.keyPassphrase = std::move(connection.answer->text);
    }
    if (connection.auth.keyPassphrase.empty())
      return;
    Connect(connection.location, connection.auth);
  }

public:
  ConnectionView(BMessenger target, BMessenger close, const BMessage &message)
      : BGroupView(B_VERTICAL, 6), pane(target), closeTarget(close),
        closeMessage(message) {
    // BTextControl invokes its message on focus loss as well as Enter. Never
    // submit partially entered credentials when the user tabs between fields.
    url = new BTextControl("network address", "Address:", "smb://", nullptr);
    user = new BTextControl("network username", "Username:", "", nullptr);
    password =
        new BTextControl("network password", "Password:", "", nullptr);
    password->TextView()->HideTyping(true);
    domain = new BTextControl("SMB domain:", "", nullptr);
    key = new BTextControl("ssh key", "SSH private key:",
                           Listi::DefaultSshPath(Listi::SshFile::Identity).c_str(), nullptr);
    hosts = new BTextControl("known hosts", "SSH known hosts:",
                             Listi::DefaultSshPath(Listi::SshFile::KnownHosts).c_str(), nullptr);
    keyPassphrase = new BTextControl("key passphrase", "SSH key passphrase:", "", nullptr);
    keyPassphrase->TextView()->HideTyping(true);
    advanced = new BGroupView(B_VERTICAL, 4);
    BLayoutBuilder::Group<>(advanced, B_VERTICAL, 4)
        .AddGroup(B_HORIZONTAL, 12)
        .Add(domain)
        .Add(key)
        .End()
        .Add(hosts)
        .Add(keyPassphrase);
    advanced->Hide();
    error = new WrappedLabel("network hint",
                             "SMB, SFTP, FTP or FTPS. Credentials are kept only "
                             "for this session. FTP is unencrypted.");
    BLayoutBuilder::Group<>(this, B_VERTICAL, 6)
        .Add(url)
        .AddGroup(B_HORIZONTAL, 12)
        .Add(user)
        .Add(password)
        .End()
        .Add(advanced)
        .Add(error, 0)
        .AddGroup(B_HORIZONTAL, 8)
        .Add(new BButton("Advanced", new BMessage(kAdvanced)))
        .AddGlue()
        .Add(new BButton("Cancel", new BMessage(kCancelConnect)))
        .Add(new BButton("Connect", new BMessage(kConnect)))
        .End();
  }
  void AttachedToWindow() override {
    BGroupView::AttachedToWindow();
    auto target = [&](auto &&self, BView *view) -> void {
      for (int32 i = 0; i < view->CountChildren(); ++i) {
        auto child = view->ChildAt(i);
        if (auto button = dynamic_cast<BButton *>(child))
          button->SetTarget(this);
        if (auto control = dynamic_cast<BTextControl *>(child)) {
          control->SetTarget(this);
          control->TextView()->AddFilter(
              new Listi::ExplicitSubmit(BMessenger(this), BMessage(kConnect)));
        }
        self(self, child);
      }
    };
    target(target, this);
  }
  void AllAttached() override {
    BGroupView::AllAttached();
    Listi::FocusTextAtEnd(*url);
  }
  void DetachedFromWindow() override {
    DismissPassphrase();
    BGroupView::DetachedFromWindow();
  }
  void MessageReceived(BMessage *m) override {
    if (closing)
      return;
    if (m->what == kCancelConnect) {
      Close();
      return;
    }
    if (m->what == kAdvanced) {
      expanded = !expanded;
      if (expanded)
        advanced->Show();
      else
        advanced->Hide();
      return;
    }
    if (m->what != kConnect && m->what != kPassphraseFinished) {
      BGroupView::MessageReceived(m);
      return;
    }
    try {
      if (m->what == kPassphraseFinished) {
        FinishPassphrase();
        return;
      }
      if (pending)
        return;
      auto location = Listi::ParseLocation(url->Text());
      if (location.Local() || location.Archive())
        throw std::runtime_error("Enter an SMB, SFTP, FTP or FTPS address.");
      Listi::Connection auth;
      auth.username = user->Text();
      if (location.scheme == "sftp" && auth.username.empty()) {
        error->SetMessage("Enter the SSH username for this server.");
        user->MakeFocus(true);
        return;
      }
      auth.password = password->Text();
      auth.domain = domain->Text();
      auth.privateKey = key->Text();
      auth.knownHosts = hosts->Text();
      if (location.scheme == "sftp") {
        if (!auth.privateKey.empty() && !Listi::ReadableSshFile(auth.privateKey))
          throw std::runtime_error("Cannot read the selected SSH private key. Check its local path under Advanced.");
        auth.keyPassphrase = keyPassphrase->Text();
        if (auth.keyPassphrase.empty() && !auth.privateKey.empty() &&
            Listi::SshKeyNeedsPassphrase(auth.privateKey)) {
          AskPassphrase(location.URL(), std::move(auth));
          return;
        }
      }
      Connect(location.URL(), auth);
    } catch (const std::exception &e) {
      error->SetMessage(e.what());
    }
  }
};
} // namespace
BView *CreateConnectionView(BMessenger pane, BMessenger close,
                            const BMessage &message) {
  return new ConnectionView(pane, close, message);
}
