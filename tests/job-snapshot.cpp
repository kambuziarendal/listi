// SPDX-License-Identifier: MIT
#include "JobSnapshot.h"
#include "VirtualFile.h"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
int main() {
  char tmp[]="/tmp/Listi-snapshot-XXXXXX";char *root=mkdtemp(tmp);if(!root)return 2;
  std::string base=root,src=base+"/file",dst=base+"/dest";mkdir(dst.c_str(),0755);
  int failures=0,checks=0;
  auto check=[&](bool ok,const char*text){++checks;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<text<<std::endl;};
  {
    const auto rename = Listi::EncodeJob(Listi::Job{Listi::Action::Rename, {src}, dst}, 'test');
    check(!Listi::DecodeSubmittedJob(rename, {Listi::Action::Trash, Listi::Action::Delete}),
          "a confirmation cannot submit a different command");
    check(!Listi::DecodeSubmittedJob(BMessage('test'), {Listi::Action::Copy}),
          "a missing action does not default to an approved copy");
    const auto accepted = Listi::DecodeSubmittedJob(rename, {Listi::Action::Rename});
    check(accepted && accepted->sources.front() == src && accepted->destination == dst,
          "a valid confirmation carries its original paths");
  }
  for (auto action : {Listi::Action::Rename, Listi::Action::Move, Listi::Action::Delete}) {
    const std::string path = "sftp://fixture/selected";
    Listi::Job selection{action, {path}, ""};
    auto &entry = selection.expectedRemoteSources[path];
    entry.link = true;
    entry.size = 42;
    entry.SetModification(123, 456000000);
    entry.providerIdentity = "object-7";
    auto captured = Listi::DecodeSubmittedJob(Listi::EncodeJob(selection, 'test'), {action});
    const auto &saved = captured->expectedRemoteSources.at(path);
    check(!saved.directory && saved.link && saved.sizeKnown && saved.size == entry.size &&
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
