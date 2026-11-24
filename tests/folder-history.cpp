// SPDX-License-Identifier: MIT
// Native BFS regression; run with make audit TESTS=folder-history.
#include "FolderHistory.h"
#include "FolderHistoryListing.h"
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <OS.h>
#include <unistd.h>
#include <fcntl.h>
#include <fs_attr.h>
#include <sys/stat.h>
#include <sys/file.h>
namespace fs=std::filesystem;
using Listi::HistoryStore;
namespace {
std::atomic_bool *cancelAfterWrite = nullptr;
}

extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __wrap_write(int fd, const void *bytes, size_t count) {
  auto written = __real_write(fd, bytes, count);
  if (cancelAfterWrite && written > 0) {
    *cancelAfterWrite = true;
    cancelAfterWrite = nullptr;
  }
  return written;
}

int main() {
  char path[] = "/boot/home/listi-history-tests-XXXXXX";
  assert(mkdtemp(path));
  struct Scratch {
    fs::path path;
    ~Scratch() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  } scratch{path};
  const auto &base = scratch.path;
  auto source=base/"source", store=base/"store";
  fs::create_directories(source/"dir");
  fs::create_directory(store);
  auto make = [&](const fs::path &path, const char *data) { std::ofstream(path) << data; };
  make(source/"dir/note", "original");
  make(source/"same", "original");
  fs::create_symlink("dir/note", source/"pointer");
  const auto attr = [](const fs::path &path, const char *name, uint32 type, const void *bytes, size_t count) {
    int fd=open(path.c_str(), O_RDONLY|O_NOTRAVERSE);
    assert(fd>=0);
    assert(fs_write_attr(fd, name, type, 0, bytes, count)==ssize_t(count));
    close(fd);
  };
  int32 answer=42;
  const unsigned char binary[]={0,255,1};
  attr(source/"dir/note","Count",B_INT32_TYPE,&answer,sizeof(answer));
  attr(source/"dir/note","Bytes",B_RAW_TYPE,binary,sizeof(binary));
  attr(source/"dir/note","BEOS:TYPE",B_MIME_STRING_TYPE,"text/plain",11);
  attr(source/"dir","Project",B_STRING_TYPE,"alpha",6);
  attr(source/"pointer","Status",B_STRING_TYPE,"link",5);
  attr(source, "Listi:RestoreIncomplete", B_STRING_TYPE, "user data", 10);
  HistoryStore history(source,store);
  std::atomic_bool cancel(false);
  auto count = [&] {
    BMessage ids=history.Points();
    type_code type;
    int32 n=0;
    ids.GetInfo("point", &type, &n);
    return n;
  };
  auto latest = [&]{ return std::string(history.Points().GetString("point","")); };
  auto objects = [&]{
    int n=0;
    for(auto &entry: fs::directory_iterator(store))
      n += entry.path().filename().string().find("object-")==0;
    return n;
  };
  auto checkAttr = [](const fs::path &path, const char *name, uint32 type, const void *expected, size_t length) {
    int fd=open(path.c_str(), O_RDONLY|O_NOTRAVERSE);
    assert(fd>=0);
    attr_info info{};
    assert(fs_stat_attr(fd,name,&info)==0 && info.type==type && info.size==ssize_t(length));
    char data[128];
    assert(fs_read_attr(fd,name,type,0,data,sizeof(data))==ssize_t(length));
    assert(memcmp(data,expected,length)==0);
    close(fd);
  };
  auto reject = [](auto callback, const char *label) {
    try { callback(); }
    catch(const std::exception &e) { std::cout << "PASS " << label << ": " << e.what() << std::endl; return; }
    assert(false);
  };
  assert(count()==0 && fs::is_empty(store));
  history.Save(cancel);
  std::string first=latest();
  assert(count()==1 && objects()==1);
  attr(source/"dir/note","Status",B_STRING_TYPE,"approved",9);
  history.Save(cancel);
  const auto metadata = latest();
  assert(count() == 2 && objects() == 1 && first != metadata);
  fs::rename(source/"dir/note",source/"dir/renamed");
  history.Save(cancel);
  const auto renamed = latest();
  assert(count() == 3 && objects() == 1 && renamed != metadata);
  make(source/"dir/renamed","changed");
  history.Save(cancel);
  const auto edited = latest();
  assert(count()==4 && objects()==2);
  fs::remove(source/"dir/renamed");
  history.Save(cancel);
  assert(count()==5 && objects()==2);
  std::cout << "PASS first/next, metadata/rename and identical contents share one object; changed bytes add one; deletion saved" << std::endl;
  std::string status;
  auto attrRows = Listi::DescribeVersion(history, metadata, first, status, cancel);
  assert(attrRows.at("dir/note").change == "Changed" && attrRows.at("same").change.empty());
  auto movedRows = Listi::DescribeVersion(history, renamed, metadata, status, cancel);
  assert(movedRows.at("dir/note").change == "Removed" && movedRows.at("dir/note").mode == 0);
  assert(movedRows.at("dir/renamed").change == "Added");
  auto editedRows = Listi::DescribeVersion(history, edited, renamed, status, cancel);
  assert(editedRows.at("dir/renamed").change == "Changed" && editedRows.at("same").change.empty());
  auto initialRows = Listi::DescribeVersion(history, first, "", status, cancel);
  assert(initialRows.at("dir/note").change.empty() && !initialRows.count("dir/renamed"));
  std::cout << "PASS timeline compares adjacent saved versions, including removed and changed nested files" << std::endl;
  history.Restore(first,base/"one",cancel);
  history.Restore(metadata,base/"two",cancel);
  history.Restore(renamed,base/"three",cancel);
  history.Restore(latest(),base/"last",cancel);
  assert(fs::exists(base/"one/dir/note") && fs::exists(base/"two/dir/note") && fs::exists(base/"three/dir/renamed"));
  assert(!fs::exists(base/"last/dir/renamed"));
  assert(fs::read_symlink(base/"one/pointer")=="dir/note");
  checkAttr(base/"one/dir/note","Count",B_INT32_TYPE,&answer,sizeof(answer));
  checkAttr(base/"one/dir/note","Bytes",B_RAW_TYPE,binary,sizeof(binary));
  checkAttr(base/"one/dir/note","BEOS:TYPE",B_MIME_STRING_TYPE,"text/plain",11);
  checkAttr(base/"one/dir","Project",B_STRING_TYPE,"alpha",6);
  checkAttr(base/"one/pointer","Status",B_STRING_TYPE,"link",5);
  checkAttr(base/"two/dir/note","Status",B_STRING_TYPE,"approved",9);
  std::cout << "PASS independent copies preserve BFS MIME, int32, raw bytes, strings, directory and symlink attributes" << std::endl;
  reject([&]{ history.Restore(first,base/"one",cancel); }, "existing destination not overwritten");
  fs::create_directory(base/"picked");
  history.Restore(first, base/"picked/note", cancel, "dir/note");
  std::string recovered;
  std::ifstream(base/"picked/note") >> recovered;
  assert(recovered == "original" && std::distance(fs::directory_iterator(base/"picked"), {}) == 1);
  checkAttr(base/"picked/note", "Count", B_INT32_TYPE, &answer, sizeof(answer));
  reject([&] { history.Restore(first, base/"picked/note", cancel, "dir/note"); }, "file destination exists");
  reject([&] { history.Restore(first, base/"invalid", cancel, "../same"); }, "invalid selected file");
  reject([&] { history.Restore(first, base/"invalid", cancel, "pointer"); }, "symlink not a regular file");
  assert(!fs::exists(base/"invalid"));
  std::cout << "PASS selected nested file restores old bytes and metadata, never siblings or overwrites" << std::endl;
  cancelAfterWrite = &cancel;
  reject([&]{ history.Restore(first,base/"partial",cancel); }, "canceled after a payload write");
  assert(cancel && !cancelAfterWrite && count() == 5);
  constexpr char incomplete[] = "Incomplete Listi restore. Do not use as a finished copy.";
  checkAttr(base/"partial", "Listi:RestoreIncomplete-1", B_STRING_TYPE,
            incomplete, sizeof(incomplete));
  checkAttr(base/"one", "Listi:RestoreIncomplete", B_STRING_TYPE, "user data", 10);
  {
    const int fd = open((base/"one").c_str(), O_RDONLY);
    attr_info marker{};
    assert(fd >= 0 && fs_stat_attr(fd, "Listi:RestoreIncomplete-1", &marker) != 0);
    close(fd);
  }
  reject([&]{ history.Save(cancel); }, "canceled save");
  assert(count()==5);
  cancel=false;
  fs::rename(source,base/"source-offline");
  reject([&]{ history.Save(cancel); }, "source missing");
  assert(count()==5);
  fs::rename(base/"source-offline",source);
  fs::rename(store,base/"old");
  reject([&]{ history.Save(cancel); }, "store missing");
  fs::create_directory(store);
  reject([&]{ history.Save(cancel); }, "store replaced");
  assert(fs::is_empty(store));
  fs::remove(store);
  fs::rename(base/"old",store);
  assert(count()==5);
  history.Restore(first,base/"after",cancel);
  std::cout << "PASS prior points available after cancellation, errors and replacement" << std::endl;

  const auto approved = history.FolderIdentity();
  assert(!history.SaveAutomatically(cancel, approved) && count() == 5);
  make(source/"future", "later");
  assert(history.SaveAutomatically(cancel, approved) && count() == 6);
  assert(!history.SaveAutomatically(cancel, approved) && count() == 6);
  history.Restore(latest(), base/"automatic", cancel);
  assert(fs::exists(base/"automatic/future"));
  std::cout << "PASS automatic saves only changed trees and uses the manual restore format" << std::endl;

  int otherWriter = open(store.c_str(), O_RDONLY | O_DIRECTORY);
  assert(otherWriter >= 0 && flock(otherWriter, LOCK_EX | LOCK_NB) == 0);
  reject([&]{ history.Save(cancel); }, "another writer holds the folder");
  reject([&]{ history.SaveAutomatically(cancel, approved); }, "automatic respects writer lock");
  close(otherWriter);
  assert(count() == 6);

  fs::rename(source, base/"formerly-approved");
  fs::create_directory(source);
  make(source/"private", "never approved");
  HistoryStore replacement(source,store);
  reject([&]{ replacement.SaveAutomatically(cancel, approved); }, "replacement source not approved");
  assert(count() == 6);

  fs::remove_all(source);
  fs::rename(base/"formerly-approved", source);

  const auto wide = source/"too-many-entries";
  fs::create_directory(wide);
  for (int i = 0; i < 2049; ++i)
    make(wide/std::to_string(i), "");
  const int beforeOversize = objects();
  reject([&]{ history.SaveAutomatically(cancel, approved); }, "automatic entry budget");
  assert(count() == 6 && objects() == beforeOversize);
  fs::remove_all(wide);

  const auto saved = store/latest();
  for (int i = 0; i < 18; ++i)
    fs::copy_file(saved, store/("point-1-" + std::to_string(i)));
  reject([&]{ history.SaveAutomatically(cancel, approved); }, "automatic point cap");
  assert(count() == 24);
  history.Save(cancel);
  assert(count() == 25);
  std::cout << "PASS automatic does not delete points or restrict explicit manual saves" << std::endl;
}
