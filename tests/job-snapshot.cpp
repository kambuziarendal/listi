// SPDX-License-Identifier: MIT
#include "JobSnapshot.h"
#include "VirtualFile.h"
#include <cstring>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
namespace {
const char *failedValue = nullptr;
bool injected = false;
std::string Read(const std::string &path) {
  std::ifstream stream(path);
  return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
}
extern "C" status_t RealAddString(BMessage *, const char *, const char *)
    asm("__real__ZN8BMessage9AddStringEPKcS1_");
extern "C" status_t WrappedAddString(BMessage *, const char *, const char *)
    asm("__wrap__ZN8BMessage9AddStringEPKcS1_");
extern "C" status_t WrappedAddString(BMessage *message, const char *key, const char *value) {
  if (failedValue && !injected && !strcmp(key, "source") && !strcmp(value, failedValue)) {
    injected = true;
    return B_NO_MEMORY;
  }
  return RealAddString(message, key, value);
}
int main() {
  char tmp[]="/tmp/Listi-snapshot-XXXXXX";char *root=mkdtemp(tmp);if(!root)return 2;
  std::string base=root,src=base+"/file",dst=base+"/dest";mkdir(dst.c_str(),0755);
  int failures=0,checks=0;
  auto check=[&](bool ok,const char*text){++checks;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<text<<std::endl;};
  {
    std::ofstream(src) << "first";
    Listi::Job selection{Listi::Action::Rename, {src}, dst};
    Listi::FreezeLocalJob(selection, dst);
    const auto rename = Listi::EncodeJob(selection, 'test');
    check(!Listi::DecodeSubmittedJob(rename, {Listi::Action::Trash, Listi::Action::Delete}),
          "a confirmation cannot submit a different command");
    check(!Listi::DecodeSubmittedJob(BMessage('test'), {Listi::Action::Copy}),
          "a missing action does not default to an approved copy");
    const auto accepted = Listi::DecodeSubmittedJob(rename, {Listi::Action::Rename});
    check(accepted && accepted->sources.front() == src && accepted->destination == dst,
          "a valid confirmation carries its original paths");

    Listi::Job network{Listi::Action::Copy, {src}, "smb://fixture/share"};
    Listi::FreezeLocalJob(network);
    for (const char *id : {"first-view", ""}) {
      network.displayedRemoteDestination = id;
      const auto queued = Listi::DecodeSubmittedJob(Listi::EncodeJob(network, 'test'), {Listi::Action::Copy});
      check(queued && queued->displayedRemoteDestination == network.displayedRemoteDestination,
            "confirmed network transfer retains the displayed destination identity");
    }

    const auto second = base + "/second";
    std::ofstream(second) << "second";
    std::ofstream(dst + "/keep") << "untouched";
    selection.action = Listi::Action::Copy;
    selection.sources.push_back(second);
    Listi::FreezeLocalJob(selection, dst);
    failedValue = second.c_str();
    bool refused = false;
    try {
      const auto accepted = Listi::EncodeJob(selection, 'test');
      if (auto job = Listi::DecodeSubmittedJob(accepted, {Listi::Action::Copy}))
        Listi::Run(*job, {});
    } catch (const std::runtime_error &) {
      refused = true;
    }
    failedValue = nullptr;
    check(injected && refused && access((dst + "/file").c_str(), F_OK) != 0 &&
              access((dst + "/second").c_str(), F_OK) != 0 && Read(dst + "/keep") == "untouched" &&
              Read(src) == "first" && Read(second) == "second",
          "failed second source append cannot submit a partial copy or change either side");
    auto complete = Listi::EncodeJob(selection, 'test');
    auto shortened = complete;
    shortened.RemoveData("source", 1);
    shortened.RemoveData("identity", 1);
    check(!Listi::DecodeSubmittedJob(shortened, {Listi::Action::Copy}),
          "even matching truncated lists cannot satisfy the recorded selection count");
    auto legacy = complete;
    legacy.RemoveName("sourceCount");
    shortened = legacy;
    shortened.RemoveData("source", 1);
    check(!Listi::DecodeSubmittedJob(shortened, {Listi::Action::Copy}),
          "legacy confirmation rejects an extra identity for an unselected source");
    shortened = legacy;
    shortened.RemoveName("source");
    check(!Listi::DecodeSubmittedJob(shortened, {Listi::Action::Copy}),
          "an empty selection cannot confirm a file copy");
    shortened = legacy;
    shortened.RemoveData("identity", 1);
    check(!Listi::DecodeSubmittedJob(shortened, {Listi::Action::Copy}),
          "every local source requires its original identity");
    BMessage malformed;
    malformed.AddString("path", second.c_str());
    malformed.AddInt32("device", 0);
    shortened.AddMessage("identity", &malformed);
    check(!Listi::DecodeSubmittedJob(shortened, {Listi::Action::Copy}),
          "an incomplete nested identity cannot be substituted for the missing guard");
    auto mismatched = selection;
    const auto secondNode = mismatched.expectedSources.at(second);
    mismatched.expectedSources.erase(second);
    mismatched.expectedSources.emplace(dst + "/keep", secondNode);
    check(!Listi::DecodeSubmittedJob(Listi::EncodeJob(mismatched, 'test'), {Listi::Action::Copy}),
          "equal counts cannot hide an identity for a different path");
    auto restored = Listi::DecodeSubmittedJob(legacy, {Listi::Action::Copy});
    Listi::Result result;
    if (restored)
      result = Listi::Run(*restored, {});
    check(restored && result.completed == 2 && result.errors.empty() && Read(dst + "/file") == "first" &&
              Read(dst + "/second") == "second" && Read(dst + "/keep") == "untouched" &&
              Read(src) == "first" && Read(second) == "second",
          "a complete legacy confirmation copies the entire selection and preserves bystanders");
    Listi::Job creation{Listi::Action::MakeDirectory, {}, dst};
    Listi::FreezeLocalJob(creation, dst);
    check(bool(Listi::DecodeSubmittedJob(Listi::EncodeJob(creation, 'test'), {Listi::Action::MakeDirectory})),
          "intentional no-source create confirmation retains its captured parent");
    for (const auto &path : {src, second, dst + "/file", dst + "/second", dst + "/keep"})
      unlink(path.c_str());
  }
  for (auto action : {Listi::Action::Rename, Listi::Action::Move, Listi::Action::Delete}) {
    const std::string path = "sftp://fixture/selected";
    Listi::Job selection{action, {path}, ""};
    auto &entry = selection.expectedRemoteSources[path];
    entry.link = true;
    entry.permissionsKnown = action != Listi::Action::Delete;
    entry.permissions = 0600;
    entry.size = 42;
    entry.SetModification(123, 456000000);
    entry.providerIdentity = "object-7";
    auto captured = Listi::DecodeSubmittedJob(Listi::EncodeJob(selection, 'test'), {action});
    if (!captured) {
      check(false, "complete remote selection remains valid");
      continue;
    }
    const auto &saved = captured->expectedRemoteSources.at(path);
    check(!saved.directory && saved.link && saved.sizeKnown && saved.size == entry.size &&
              saved.permissionsKnown == entry.permissionsKnown &&
              (!saved.permissionsKnown || saved.permissions == entry.permissions) &&
              saved.SameModification(entry) && saved.providerIdentity == entry.providerIdentity,
          "network mutation confirmation retains the selected type and revision");
  }
  for(auto action:{Listi::Action::Trash,Listi::Action::Delete,Listi::Action::Rename,Listi::Action::Copy,Listi::Action::Move}) {
    {std::ofstream f(src);f<<"original";}
    Listi::Job job{action,{src},dst};
    Listi::FreezeLocalJob(job,action==Listi::Action::Rename?base:action==Listi::Action::Copy||action==Listi::Action::Move?dst:"");
    auto captured=Listi::DecodeJob(Listi::EncodeJob(job,'test'));
    if(action==Listi::Action::Rename)captured.destination=base+"/renamed";
    rename(src.c_str(),(src+".original").c_str());{std::ofstream f(src);f<<"replacement";}
    auto result=Listi::Run(captured,{});
    struct stat st;
    check(!result.errors.empty() && !result.completed && !lstat(src.c_str(),&st),"delayed action rejects replaced source after message roundtrip");
    unlink(src.c_str());unlink((src+".original").c_str());
  }
  for(auto action:{Listi::Action::MakeDirectory,Listi::Action::Rename}) {
    {std::ofstream f(src);f<<"payload";}
    Listi::Job job{action,action==Listi::Action::Rename?std::vector<std::string>{src}:std::vector<std::string>{},dst};
    Listi::FreezeLocalJob(job,dst);auto captured=Listi::DecodeJob(Listi::EncodeJob(job,'test'));
    captured.destination=dst+"/new";
    rename(dst.c_str(),(dst+".original").c_str());mkdir(dst.c_str(),0755);
    auto result=Listi::Run(captured,{});struct stat st;
    check(!result.errors.empty() && !result.completed && lstat(captured.destination.c_str(),&st)!=0,"named operation rejects replaced parent folder");
    rmdir(dst.c_str());rename((dst+".original").c_str(),dst.c_str());unlink(src.c_str());
  }
  Listi::Job valid{Listi::Action::MakeDirectory,{},dst};Listi::FreezeLocalJob(valid,dst);valid.destination=dst+"/new";
  check(Listi::Run(valid,{}).completed==1,"unchanged captured parent permits new folder");rmdir(valid.destination.c_str());
  std::vector<std::string> deep{base+"/deep"};mkdir(deep.back().c_str(),0755);
  for(int i=0;i<66;++i){deep.push_back(deep.back()+"/d");mkdir(deep.back().c_str(),0755);}
  {std::ofstream f(deep.back()+"/payload");f<<"preserve";}
  Listi::Job removal{Listi::Action::Delete,{deep.front()},""};
  auto refused=Listi::Run(removal,{});struct stat deepStat;
  check(!refused.errors.empty() && !refused.completed && !stat((deep.back()+"/payload").c_str(),&deepStat),"deep deletion refused before removing any entry");
  Listi::Job copying{Listi::Action::Copy,{deep.front()},dst};
  auto bounded=Listi::Run(copying,{});
  check(!bounded.errors.empty() && !bounded.completed && !stat((deep.back()+"/payload").c_str(),&deepStat),"deep copy reports bounded failure and preserves source");
  unlink((deep.back()+"/payload").c_str());
  for(auto i=deep.rbegin();i!=deep.rend();++i){rmdir((dst+i->substr(base.size())).c_str());rmdir(i->c_str());}
  rmdir(dst.c_str());rmdir(base.c_str());
  std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
