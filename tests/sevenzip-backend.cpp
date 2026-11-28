// SPDX-License-Identifier: MIT
#include "ArchiveBackend.h"
#include "ArchiveJobs.h"
#include "ArchivePasswords.h"
#include <Application.h>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
using namespace Listi;
namespace {
int checks = 0;
void check(bool ok, const char *name) {
  if (!ok) {
    fprintf(stderr, "FAIL %s\n", name);
    exit(1);
  }
  ++checks;
  printf("PASS %s\n", name);
}
ArchiveSecretPtr secret(const char *s) {
  return std::make_shared<ArchiveSecret>(s);
}
std::string read(const std::string &p, const TransferControl &c = {}) {
  std::string out;
  ReadFile(p, [&](const char *b, size_t n) { out.append(b, n); }, c);
  return out;
}
void password(const char *p) {
  ForgetArchivePasswords();
  SetArchivePasswordPrompt([p](const std::string &, bool,
                               const TransferControl &) { return secret(p); });
}
void copy(const std::string &from, const std::string &to) {
  std::ifstream in(from, std::ios::binary);
  std::ofstream out(to, std::ios::binary);
  out << in.rdbuf();
}
void extractedTimes(const std::string &root, const std::string &archive) {
  const timespec expected[] = {{-1, 0}, {0, 0}, {1700000000, 125000000}};
  const char *names[] = {"before", "epoch", "fraction"};
  auto extract = PrepareArchiveJob(true, {archive});
  SetArchiveDestination(extract, root, "extracted-times");
  check(RunArchiveJob(extract, {}).completed == 1, "extract timestamped 7z");
  const auto url = OpenArchive(archive), reference = root + "/reference-time";
  std::ofstream(reference) << "reference";
  for (int i = 0; i < 3; ++i) {
    FileEntry entry;
    check(StatFile(ChildLocation(url, names[i]), entry) && entry.modifiedKnown &&
              entry.modified == expected[i].tv_sec &&
              entry.ModifiedNanoseconds() == expected[i].tv_nsec, "exact p7zip member time");
    // Measure this volume's precision independently of the archive adapter.
    timespec times[2] = {expected[i], expected[i]};
    struct stat stored, actual;
    const auto path = extract.destination + "/" + names[i];
    check(!utimensat(AT_FDCWD, reference.c_str(), times, 0) && !stat(reference.c_str(), &stored) &&
              !stat(path.c_str(), &actual) && actual.st_mtim.tv_sec == stored.st_mtim.tv_sec &&
              actual.st_mtim.tv_nsec == stored.st_mtim.tv_nsec, "extracted time matches filesystem precision");
  }
}
void *run(void *) {
  try {
    char cwd[4096];
    getcwd(cwd, sizeof(cwd));
    std::string base = std::string(cwd) + "/tests/fixtures/sevenzip/";
    char temp[] = "/boot/home/listi-sevenzip-test-XXXXXX";
    auto root = mkdtemp(temp);
    check(root, "test root");
    check(SevenZipAvailable(), "optional native engine available");
    extractedTimes(root, base + "times.7z");
    for (const char *name : {"data.7z", "headers.7z"}) {
      password("Listi-test-only");
      std::string file = base + name;
      auto url = OpenArchive(file);
      auto entries = ListFiles(url);
      check(entries.size() == 2, "7z immediate children");
      check(read(ChildLocation(url, "alpha.txt")) == "engine alpha\n",
            "7z content exact");
      auto sub = ChildLocation(url, "sub");
      auto nested = ListFiles(sub);
      check(nested.size() == 1 && nested[0].name == "blå.txt",
            "7z Unicode nested name");
      check(read(nested[0].path) == "engine unicode\n",
            "7z Unicode content exact");
      auto job = PrepareArchiveJob(true, {file});
      SetArchiveDestination(job, root, name);
      auto result = RunArchiveJob(job, {});
      check(result.errors.empty() && result.completed == 1,
            "encrypted 7z atomic extraction");
      check(read(std::string(root) + "/" + name + "/sub/blå.txt") ==
                "engine unicode\n",
            "extracted nested content");
    }
    for (const char *name :
         {"rar4-data.rar", "rar4-headers.rar", "rar5-headers.rar"}) {
      password(std::string(name).find("rar5") == 0 ? "password" : "12345678");
      auto entries = ListFiles(OpenArchive(base + name));
      check(entries.size() == (std::string(name).find("rar5") == 0 ? 4 : 2),
            "RAR encrypted names listed");
      for (const auto &entry : entries)
        check(read(entry.path) == (std::string(name).find("rar5") == 0
                                       ? "This is from " + entry.name
                                       : "data of " + entry.name + "\n"),
              "RAR decrypted member exact content");
      auto job = PrepareArchiveJob(true, {base + name});
      SetArchiveDestination(job, root, name);
      auto result = RunArchiveJob(job, {});
      check(result.errors.empty() && result.completed == 1,
            "encrypted RAR atomic extraction");
    }
    password("password");
    auto mixed = OpenArchive(base + "rar5-mixed.rar");
    check(read(ChildLocation(mixed, "b.txt")).size() == 18,
          "RAR5 first member password");
    int calls = 0;
    bool retrySeen = false;
    SetArchivePasswordPrompt(
        [&](const std::string &, bool retry, const TransferControl &) {
          ++calls;
          retrySeen |= retry;
          return secret("password2");
        });
    check(read(ChildLocation(mixed, "d.txt")).size() == 18 && calls == 1 &&
              retrySeen,
          "RAR5 different member password retries cached value");
    password("Listi-test-only");
    auto url = OpenArchive(base + "data.7z");
    auto target = ChildLocation(url, "alpha.txt");
    ForgetArchivePasswords();
    calls = 0;
    retrySeen = false;
    SetArchivePasswordPrompt(
        [&](const std::string &, bool retry, const TransferControl &) {
          retrySeen |= retry;
          return secret(++calls == 1 ? "wrong" : "Listi-test-only");
        });
    check(read(target) == "engine alpha\n" && calls == 2 && retrySeen,
          "wrong then correct produces no partial bytes");
    ForgetArchivePasswords();
    calls = 0;
    SetArchivePasswordPrompt(
        [&](const std::string &, bool, const TransferControl &) {
          ++calls;
          return secret("wrong");
        });
    std::string output;
    bool failed = false;
    try {
      ReadFile(target, [&](const char *b, size_t n) { output.append(b, n); });
    } catch (const std::exception &) {
      failed = true;
    }
    check(failed && calls == 4 && output.empty(),
          "bounded wrong attempts publish no data");
    ForgetArchivePasswords();
    SetArchivePasswordPrompt(
        [](const std::string &, bool, const TransferControl &) {
          return ArchiveSecretPtr();
        });
    bool cancelled = false;
    try {
      read(target);
    } catch (const Cancelled &) {
      cancelled = true;
    }
    check(cancelled, "password cancel propagates");
    auto job = PrepareArchiveJob(true, {base + "headers.7z"});
    SetArchiveDestination(job, root, "cancelled");
    auto result = RunArchiveJob(job, {});
    check(result.cancelled &&
              access((std::string(root) + "/cancelled").c_str(), F_OK) != 0,
          "header password cancel publishes no destination");
    password("Listi-test-only");
    SetArchivePasswordPrompt(
        [](const std::string &, bool, const TransferControl &) {
          ForgetArchivePasswords();
          return secret("Listi-test-only");
        });
    cancelled = false;
    try {
      read(target);
    } catch (const Cancelled &) {
      cancelled = true;
    }
    check(cancelled, "forget rejects outstanding engine prompt");
    std::string changed = std::string(root) + "/changed.7z";
    copy(base + "headers.7z", changed);
    ForgetArchivePasswords();
    SetArchivePasswordPrompt(
        [&](const std::string &, bool, const TransferControl &) {
          rename(changed.c_str(), (changed + ".old").c_str());
          copy(base + "headers.7z", changed);
          return secret("Listi-test-only");
        });
    failed = false;
    try {
      OpenArchive(changed);
    } catch (const std::exception &e) {
      failed =
          std::string(e.what()).find("Archive changed") != std::string::npos;
    }
    check(failed, "archive replacement during header prompt rejected");
    password("Listi-test-only");
    TransferControl control;
    int polls = 0;
    control.proceed = [&] { return ++polls < 8; };
    cancelled = false;
    try {
      read(target, control);
    } catch (const Cancelled &) {
      cancelled = true;
    }
    check(cancelled, "native decoder observes cancellation");
    password("unused");
    auto links = ListFiles(OpenArchive(base + "links.7z"));
    check(links.size() == 1 && links[0].link, "7z symlink recognized");
    failed = false;
    try {
      read(links[0].path);
    } catch (const std::exception &) {
      failed = true;
    }
    check(failed, "7z symlink never decoded as ordinary file");
    std::string damaged = std::string(root) + "/damaged.7z";
    copy(base + "data.7z", damaged);
    truncate(damaged.c_str(), 40);
    failed = false;
    try {
      OpenArchive(damaged);
    } catch (const std::exception &) {
      failed = true;
    }
    check(failed, "truncated archive rejected");
    std::string renamed = std::string(root) + "/not-an-extension";
    copy(base + "headers.7z", renamed);
    password("Listi-test-only");
    check(read(ChildLocation(OpenArchive(renamed), "alpha.txt")) ==
              "engine alpha\n",
          "magic routes renamed encrypted archive");
    for (const char *path : {"../escape", "/absolute", "dir/../../escape",
                             "C:/drive", "dir\\escape"}) {
      failed = false;
      try {
        SafeArchivePath(path);
      } catch (const std::exception &) {
        failed = true;
      }
      check(failed, "unsafe archive path rejected");
    }
    StopArchivePasswords();
    printf("%d sevenzip checks passed on 256 KiB stack\n", checks);
  } catch (const std::exception &e) {
    fprintf(stderr, "UNEXPECTED: %s\n", e.what());
    exit(2);
  }
  return nullptr;
}
} // namespace
int main() {
  BApplication app("application/x-vnd.Listi-SevenZip-Test");
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 256 * 1024);
  pthread_t thread;
  check(!pthread_create(&thread, &attr, run, nullptr), "native worker started");
  pthread_attr_destroy(&attr);
  pthread_join(thread, nullptr);
}
