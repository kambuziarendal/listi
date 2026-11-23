// SPDX-License-Identifier: MIT
#include "DirectoryScanner.h"
#include "ReadWorker.h"
#include "Common.h"
#include "FileOperation.h"
#include "FileProperties.h"
#include "VirtualFile.h"
#include "Trash.h"
#include <Bitmap.h>
#include <Directory.h>
#include <Entry.h>
#include <MimeType.h>
#include <NodeInfo.h>
#include <Path.h>
#include <Volume.h>
#include <VolumeRoster.h>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
namespace {
class Scanner : public Listi::ReadWorker {
  BMessenger target;
  std::shared_ptr<std::atomic<int64>> generation;
  std::shared_ptr<DirectoryScanDelivery> delivery;

  std::shared_ptr<DirectoryWatch> watch;

  bool Send(BMessage &message, int64 serial) {
    for (int attempt = 0; attempt < 3 && generation->load() == serial;
         ++attempt) {
      status_t status =
          target.SendMessage(&message, (BHandler *)nullptr, 100000);
      if (status == B_OK)
        return true;
      if (status != B_TIMED_OUT && status != B_WOULD_BLOCK &&
          status != B_INTERRUPTED)
        break;
    }
    return false;
  }
  void Complete(BMessage &message, int64 serial) {
    if (!Send(message, serial) && delivery && generation->load() == serial)
      delivery->failedSerial.store(serial);
  }

public:
  Scanner(BMessenger t, std::shared_ptr<std::atomic<int64>> g,
          std::shared_ptr<DirectoryScanDelivery> d, std::shared_ptr<DirectoryWatch> w)
      : ReadWorker("directory reader"), target(t), generation(g), delivery(d), watch(w) {}
  void MessageReceived(BMessage *m) override {
    if (m->what != kScan) {
      BLooper::MessageReceived(m);
      return;
    }
    int64 serial = m->GetInt64("serial", 0);
    if (generation->load() != serial)
      return;
    BString path = m->GetString("path", "");
    bool icons = m->GetBool("icons", true);
    int32 iconPixels = m->GetInt32("iconSize", 16);
    if (m->GetBool("trash", false)) {
      try {
        path = Listi::TrashDirectory(path.String(), true).c_str();
      } catch (const std::exception &error) {
        BMessage failed(kScanDone);
        failed.AddInt64("serial", serial);
        failed.AddString("path", path);
        failed.AddString("error", error.what());
        Complete(failed, serial);
        return;
      }
    }
    BMessage batch(kRows);
    batch.AddInt64("serial", serial);
    int count = 0;
    // A small first batch lets the pane draw before another screenful of
    // Tracker icons is decoded. Keep later batches large to limit UI work.
    int batchLimit = icons ? 8 : 80;
    bool deliveryFailed = false;
    auto send = [&]() {
      if (!Send(batch, serial)) {
        deliveryFailed = true;
        return;
      }
      batch.MakeEmpty();
      batch.what = kRows;
      batch.AddInt64("serial", serial);
      count = 0;
      batchLimit = 80;
    };
    auto row = [&](const char *full, const char *label, bool parent = false) {
      if (deliveryFailed || generation->load() != serial)
        return;
      struct stat st;
      if (lstat(full, &st) || generation->load() != serial)
        return;
      BMessage item;
      item.AddInt32("iconSize", iconPixels);
      item.AddString("name", label);
      item.AddString("path", full);
      item.AddBool("parent", parent);
      bool link = S_ISLNK(st.st_mode);
      bool dir = S_ISDIR(st.st_mode);
      struct stat resolved;
      const bool resolvedLink = link && stat(full, &resolved) == 0;
      if (resolvedLink)
        dir = S_ISDIR(resolved.st_mode);
      const mode_t targetMode = resolvedLink ? resolved.st_mode : st.st_mode;
      item.AddInt32("kind", link ? 2 : dir ? 1 : 0);
      item.AddBool("directory", dir);
      item.AddInt64("device", st.st_dev);
      item.AddInt64("node", st.st_ino);
      item.AddInt64("size", st.st_size);
      item.AddInt64("date", st.st_mtime);
      entry_ref ref;
      BEntry e(full, false);
      if (e.GetRef(&ref) == B_OK) {
        item.AddRef("ref", &ref);
        if (!parent && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) &&
            m->HasString("attribute")) {
          BNode node(&ref);
          node_ref observed;
          bool same = node.GetNodeRef(&observed) == B_OK &&
                      observed.device == st.st_dev &&
                      observed.node == st.st_ino;
          const char *key;
          for (int32 i = 0;
               i < 4 && m->FindString("attribute", i, &key) == B_OK; ++i) {
            type_code type = 0;
            auto cell =
                same ? Listi::AttributeCell(node, key, &type) : "(unavailable)";
            // Bound the BMessage batch even for large string attributes.
            if (cell.size() > 160) {
              size_t end = 160;
              while (end &&
                     (static_cast<unsigned char>(cell[end]) & 0xc0) == 0x80)
                --end;
              cell.resize(end);
              cell += "…";
            }
            item.AddString("cell", cell.c_str());
            item.AddUInt32("cellType", type);
          }
        }
        // Tracker icon lookup may open the node. Never open a device, FIFO,
        // or a link to one just to display a directory listing.
        if (icons && (S_ISREG(targetMode) || S_ISDIR(targetMode))) {
          BBitmap bitmap(BRect(0, 0, iconPixels - 1, iconPixels - 1), B_RGBA32);
          if (BNodeInfo::GetTrackerIcon(
                  &ref, &bitmap, static_cast<icon_size>(iconPixels)) == B_OK)
            item.AddData("icon", B_RAW_TYPE, bitmap.Bits(),
                         bitmap.BitsLength());
        }
      }
      batch.AddMessage("entry", &item);
      if (++count == batchLimit)
        send();
    };
    BMessage done(kScanDone);
    done.AddInt64("serial", serial);
    if (Listi::IsVirtual(path.String())) {
      Listi::TransferControl control;
      control.proceed = [&] { return generation->load() == serial; };
      auto virtualRow = [&](const Listi::FileEntry &entry,
                            bool parent = false) {
        BMessage item;
        item.AddInt32("iconSize", iconPixels);
        item.AddString("name", parent ? ".." : entry.name.c_str());
        item.AddString("path", entry.path.c_str());
        item.AddBool("parent", parent);
        item.AddBool("directory", entry.directory);
        item.AddInt32("kind", entry.link ? 2 : entry.directory ? 1 : 0);
        item.AddInt64("size", entry.size);
        item.AddBool("sizeKnown", entry.sizeKnown);
        item.AddInt64("date", entry.modified);
        if (icons) {
          BMimeType mime(entry.directory ? "application/x-vnd.Be-directory"
                                         : "application/octet-stream");
          if (!entry.directory) {
            BMimeType guessed;
            if (BMimeType::GuessMimeType(entry.name.c_str(), &guessed) == B_OK)
              mime.SetTo(guessed.Type());
          }
          BBitmap bitmap(BRect(0, 0, iconPixels - 1, iconPixels - 1), B_RGBA32);
          if (mime.GetIcon(&bitmap, static_cast<icon_size>(iconPixels)) == B_OK)
            item.AddData("icon", B_RAW_TYPE, bitmap.Bits(),
                         bitmap.BitsLength());
        }
        batch.AddMessage("entry", &item);
        if (++count == batchLimit)
          send();
      };
      try {
        auto entries = Listi::ListFiles(path.String(), control);
        auto parent = Listi::ParentLocation(path.String());
        if (parent != path.String()) {
          Listi::FileEntry entry;
          entry.path = parent;
          entry.directory = true;
          virtualRow(entry, true);
        }
        for (auto &entry : entries) {
          control.Check();
          if (deliveryFailed)
            break;
          virtualRow(entry);
        }
      } catch (const Listi::Cancelled &) {
        return;
      } catch (const std::exception &error) {
        done.AddString("error", error.what());
      }
    } else if (path == "@disks") {
      BVolumeRoster roster;
      BVolume volume;
      while (!deliveryFailed && generation->load() == serial &&
             roster.GetNextVolume(&volume) == B_OK) {
        BDirectory root;
        BEntry e;
        BPath p;
        char name[B_FILE_NAME_LENGTH];
        if (volume.GetRootDirectory(&root) == B_OK &&
            root.GetEntry(&e) == B_OK && e.GetPath(&p) == B_OK &&
            volume.GetName(name) == B_OK)
          row(p.Path(), name);
      }
    } else {
      char resolved[PATH_MAX];
      if (!realpath(path.String(), resolved)) {
        done.AddString("error", strerror(errno));
      } else {
        if (generation->load() != serial)
          return;
        path = resolved;
        DIR *dir = opendir(path.String());
        if (!dir)
          done.AddString("error", strerror(errno));
        else {
          struct stat opened;
          if (watch) {
            node_ref ref;
            status_t status = B_ERROR;
            if (fstat(dirfd(dir), &opened) == 0) {
              ref.device = opened.st_dev;
              ref.node = opened.st_ino;
              status = watch->Install(serial, ref, target);
            }
            if (status != B_OK)
              done.AddBool("watchFailed", true);
          }
          if (generation->load() == serial && path != "/") {
            std::string parent = path.String();
            parent = parent.substr(0, parent.find_last_of('/'));
            if (parent.empty())
              parent = "/";
            row(parent.c_str(), "..", true);
          }
          while (!deliveryFailed && generation->load() == serial) {
            errno = 0;
            auto e = readdir(dir);
            if (!e) {
              if (errno)
                done.AddString("error", strerror(errno));
              break;
            }
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
                Listi::IsListiPrivateEntry(Listi::Join(path.String(), e->d_name)))
              continue;
            row(Listi::Join(path.String(), e->d_name).c_str(), e->d_name);
          }
          closedir(dir);
          if (generation->load() != serial)
            return;

        }
      }
    }
    if (generation->load() == serial) {
      if (count && !deliveryFailed)
        send();
      if (deliveryFailed)
        done.AddString("error",
                       "Directory delivery failed. Reload to try again.");
      done.AddString("path", path);
      Complete(done, serial);
    }
  }
};
}
Listi::ReadWorker *
CreateDirectoryScanner(BMessenger target,
                       std::shared_ptr<std::atomic<int64>> generation,
                       std::shared_ptr<DirectoryScanDelivery> delivery,
                       std::shared_ptr<DirectoryWatch> watch) {
  return new Scanner(target, generation, delivery, watch);
}
