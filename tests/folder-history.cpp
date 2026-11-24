// SPDX-License-Identifier: MIT
// Native BFS regression; run with make audit TESTS=folder-history.
#include "FolderHistory.h"
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <unistd.h>
#include <fcntl.h>
#include <fs_attr.h>
#include <sys/stat.h>
namespace fs=std::filesystem;
using Listi::HistoryStore;
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
  assert(count()==4 && objects()==2);
  fs::remove(source/"dir/renamed");
  history.Save(cancel);
  assert(count()==5 && objects()==2);
  std::cout << "PASS first/next, metadata/rename and identical contents share one object; changed bytes add one; deletion saved" << std::endl;
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
  cancel=true;
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
}
