// SPDX-License-Identifier: MIT
#include "DirectoryScanner.h"
#include "UnavailableVolume.h"
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
#include <fcntl.h>
#include <map>
#include <sys/stat.h>
namespace {
void ReadLocalIcon(const entry_ref &ref, int32 pixels, BMessage &row) {
  BBitmap bitmap(BRect(0, 0, pixels - 1, pixels - 1), B_RGBA32);
  if (bitmap.InitCheck() == B_OK &&
      BNodeInfo::GetTrackerIcon(&ref, &bitmap, static_cast<icon_size>(pixels)) == B_OK)
    row.AddData("icon", B_RAW_TYPE, bitmap.Bits(), bitmap.BitsLength());
}

void ReadVirtualIcon(const std::string &name, bool directory, int32 pixels,
                     BMessage &row) {
  BMimeType mime(directory ? "application/x-vnd.Be-directory" : "application/octet-stream");
  BMimeType guessed;
  if (!directory && BMimeType::GuessMimeType(name.c_str(), &guessed) == B_OK)
    mime.SetTo(guessed.Type());
  BBitmap bitmap(BRect(0, 0, pixels - 1, pixels - 1), B_RGBA32);
  if (bitmap.InitCheck() == B_OK &&
      mime.GetIcon(&bitmap, static_cast<icon_size>(pixels)) == B_OK)
    row.AddData("icon", B_RAW_TYPE, bitmap.Bits(), bitmap.BitsLength());
}

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
    if (m->what == kReadVisibleIcon) {
      const int64 serial = m->GetInt64("serial", -1);
      if (generation->load() != serial)
        return;
      const char *path = m->GetString("path", "");
      const int32 pixels = m->GetInt32("iconSize", 16);
      BMessage reply(kVisibleIconReady);
      reply.AddInt64("serial", serial);
      reply.AddString("path", path);
      reply.AddInt32("iconSize", pixels);
      if ((pixels == 16 || pixels == 96) &&
          !Listi::InterruptedVolumes().ContainsResolved(path) &&
          !Listi::InterruptedVolumes().ContainsDevice(m->GetInt64("device", -1))) {
        if (Listi::IsVirtual(path)) {
          ReadVirtualIcon(m->GetString("name", ""), m->GetBool("directory", false), pixels, reply);
        } else {
          struct stat st, resolved;
          entry_ref ref;
          if (lstat(path, &st) == 0 && st.st_dev == m->GetInt64("device", -1) &&
              st.st_ino == m->GetInt64("node", -1) && stat(path, &resolved) == 0 &&
              (S_ISDIR(resolved.st_mode) || S_ISREG(resolved.st_mode)) &&
              get_ref_for_path(path, &ref) == B_OK)
            ReadLocalIcon(ref, pixels, reply);
        }
      }
      while (generation->load() == serial) {
        const auto status = target.SendMessage(&reply, (BHandler *)nullptr, 50000);
        if (status != B_TIMED_OUT && status != B_WOULD_BLOCK && status != B_INTERRUPTED)
          break;
      }
      return;
    }
    if (m->what != kScan) {
      BLooper::MessageReceived(m);
      return;
    }
    int64 serial = m->GetInt64("serial", 0);
    if (generation->load() != serial)
      return;
    BString path = m->GetString("path", "");
    if (Listi::InterruptedVolumes().ContainsResolved(path.String())) {
      BMessage failed(kScanDone);
      failed.AddInt64("serial", serial);
      failed.AddString("path", path);
      failed.AddString("error", Listi::kUnavailableVolumeText);
      Complete(failed, serial);
      return;
    }
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
    int batchLimit = (icons || m->GetBool("firstScreen", false)) ? 8 : 80;
    bool deliveryFailed = false;
    auto send = [&]() {
      if (delivery && !delivery->ReserveBatch(*generation, serial)) {
        deliveryFailed = true;
        return;
      }
      if (!Send(batch, serial)) {
        if (delivery)
          delivery->ReleaseBatch(serial);
        deliveryFailed = true;
        return;
      }
      batch.MakeEmpty();
      batch.what = kRows;
      batch.AddInt64("serial", serial);
      count = 0;
      batchLimit = 80;
    };
    auto row = [&](const char *full, const char *label, bool parent = false,
                   int directory = -1, const struct stat *opened = nullptr,
                   const Listi::DiskListing *disk = nullptr) {
      if (deliveryFailed || generation->load() != serial ||
          Listi::InterruptedVolumes().Contains(full))
        return;
      struct stat st;
      if ((directory < 0 ? lstat(full, &st)
                         : fstatat(directory, label, &st, AT_SYMLINK_NOFOLLOW)) ||
          generation->load() != serial)
        return;
      BMessage item;
      item.AddInt32("iconSize", iconPixels);
      item.AddString("name", label);
      item.AddString("path", full);
      item.AddBool("parent", parent);
      bool link = S_ISLNK(st.st_mode);
      bool dir = S_ISDIR(st.st_mode);
      struct stat resolved;
      const bool resolvedLink = link && !Listi::InterruptedVolumes().ContainsResolved(full) &&
          (directory < 0 ? stat(full, &resolved)
                         : fstatat(directory, label, &resolved, 0)) == 0;
      if (resolvedLink)
        dir = S_ISDIR(resolved.st_mode);
      const mode_t targetMode = resolvedLink ? resolved.st_mode : st.st_mode;
      item.AddInt32("kind", link ? 2 : dir ? 1 : 0);
      item.AddBool("directory", dir);
      item.AddInt64("device", st.st_dev);
      item.AddInt64("node", st.st_ino);
      item.AddInt64("size", st.st_size);
      item.AddInt64("date", st.st_mtime);
      if (disk && st.st_dev == disk->mountedDevice && !disk->protectedVolume)
        disk->partition.WriteTo(item);
      const bool cells = !parent && (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode)) &&
                         m->HasString("attribute");
      const bool localIcon = icons && (S_ISREG(targetMode) || S_ISDIR(targetMode));
      if (cells || localIcon) {
        entry_ref ref;
        if (opened)
          ref = entry_ref(opened->st_dev, opened->st_ino, label);
        else if (BEntry(full, false).GetRef(&ref) != B_OK)
          ref = entry_ref();
        if (cells && ref.name) {
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
        if (localIcon && ref.name) {
          ReadLocalIcon(ref, iconPixels, item);
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
        item.AddBool("modifiedKnown", entry.modifiedKnown);
        item.AddString("modifiedFraction", entry.modifiedFraction.c_str());
        item.AddString("providerIdentity", entry.providerIdentity.c_str());
        if (icons)
          ReadVirtualIcon(entry.name, entry.directory, iconPixels, item);
        batch.AddMessage("entry", &item);
        if (++count == batchLimit)
          send();
      };
      try {
        auto location = Listi::ParseLocation(path.String());
        if (location.Archive() && location.path == "/" && !Listi::IsVirtual(location.archive)) {
          struct stat file;
          if (lstat(location.archive.c_str(), &file) == 0) {
            done.AddInt64("folderDevice", file.st_dev);
            done.AddInt64("folderNode", file.st_ino);
          }
        }
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
      // Subscribe before enumerating so changes during the scan aren't lost.
      if (watch && watch->InstallDisks(serial, target) != B_OK)
        done.AddBool("watchFailed", true);
      std::map<int32, Listi::DiskListing> mounted;
      Listi::VisitDiskPartitions([&](const Listi::DiskListing &disk) {
        if (deliveryFailed || generation->load() != serial)
          return true;
        if (disk.mountedDevice >= 0) {
          mounted.emplace(disk.mountedDevice, disk);
          return false;
        }
        BMessage item;
        const auto key = "@partition/" + std::to_string(disk.partition.id);
        item.AddString("name", (disk.name + " (unmounted)").c_str());
        item.AddString("path", key.c_str());
        item.AddBool("sizeKnown", false);
        item.AddBool("unmountedDisk", true);
        disk.partition.WriteTo(item);
        batch.AddMessage("entry", &item);
        if (++count == batchLimit)
          send();
        return false;
      });
      BVolumeRoster roster;
      BVolume volume;
      while (!deliveryFailed && generation->load() == serial &&
             roster.GetNextVolume(&volume) == B_OK) {
        if (Listi::InterruptedVolumes().ContainsDevice(volume.Device()))
          continue;
        BDirectory root;
        BEntry e;
        BPath p;
        char name[B_FILE_NAME_LENGTH];
        if (volume.GetRootDirectory(&root) == B_OK &&
            root.GetEntry(&e) == B_OK && e.GetPath(&p) == B_OK &&
            volume.GetName(name) == B_OK) {
          const auto disk = mounted.find(volume.Device());
          row(p.Path(), name, false, -1, nullptr,
              disk != mounted.end() ? &disk->second : nullptr);
        }
      }
    } else {
      char resolved[PATH_MAX];
      if (!realpath(path.String(), resolved)) {
        done.AddString("error", strerror(errno));
      } else if (Listi::InterruptedVolumes().Contains(resolved)) {
        done.AddString("error", Listi::kUnavailableVolumeText);
      } else {
        if (generation->load() != serial)
          return;
        path = resolved;
        DIR *dir = opendir(path.String());
        if (!dir)
          done.AddString("error", strerror(errno));
        else {
          struct stat opened;
          const bool identified = fstat(dirfd(dir), &opened) == 0;
          if (identified) {
            done.AddInt64("folderDevice", opened.st_dev);
            done.AddInt64("folderNode", opened.st_ino);
          }
          if (watch) {
            node_ref ref;
            status_t status = B_ERROR;
            if (identified) {
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
          // A copy can latch the device before its completion reaches the
          // pane. Dropping individual rows is not enough: readdir itself
          // enters the filesystem, as does the final identity check.
          const auto blocked = [&] {
            auto &volumes = Listi::InterruptedVolumes();
            return (identified && volumes.ContainsDevice(opened.st_dev)) ||
                   volumes.Contains(path.String());
          };
          while (!deliveryFailed && generation->load() == serial && !blocked()) {
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
            row(Listi::Join(path.String(), e->d_name).c_str(), e->d_name,
                false, dirfd(dir), identified ? &opened : nullptr);
          }
          closedir(dir);
          if (generation->load() != serial)
            return;
          struct stat named;
          if (blocked()) {
            count = 0;
            done.RemoveName("error");
            done.AddString("error", Listi::kUnavailableVolumeText);
          } else if (identified && (stat(path.String(), &named) != 0 ||
              named.st_dev != opened.st_dev || named.st_ino != opened.st_ino) &&
              !done.HasString("error"))
            done.AddString("error", "Folder changed while reading. Reload to try again.");
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
