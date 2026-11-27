// SPDX-License-Identifier: MIT
#include "ArchiveJobs.h"
#include "ArchiveVolumes.h"
#include "ArchivePasswords.h"
#include "FileClipboard.h"
#include "JobSnapshot.h"
#include "VirtualFile.h"
#include <Application.h>
#include <Node.h>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
using namespace Listi;
namespace {
int checks=0, failures=0;
void check(bool ok,const std::string &label){++checks;failures+=!ok;std::cout<<(ok?"PASS ":"FAIL ")<<label<<std::endl;}
std::string content(const std::string &p){std::string s;ReadFile(p,[&](const char*b,size_t n){s.append(b,n);});return s;}
bool exists(const std::string &p){FileEntry e;return StatFile(p,e);}
void clean(const std::string &p){BDirectory d(p.c_str());BEntry e;while(d.GetNextEntry(&e,false)==B_OK){struct stat s;e.GetStat(&s);if(S_ISDIR(s.st_mode)){BPath path;e.GetPath(&path);clean(path.Path());}e.Remove();}rmdir(p.c_str());}
std::string root;int serial=0;
std::string archive(const std::string &format,const std::vector<std::string> &sources, bool encrypted=false){
 auto create=PrepareArchiveJob(false,sources);create.archiveFormat=format;SetArchiveDestination(create,root,"a"+std::to_string(++serial));
 if(encrypted){create.archiveEncryptNames=format=="7z";create.archivePasswordToken=HoldArchiveCreationPassword(std::make_shared<ArchiveSecret>("Listi-move-test"));}
 auto result=Run(create,{});if(result.completed!=1||!result.errors.empty())throw std::runtime_error("Create fixture: "+(result.errors.empty()?"no completion":result.errors.front()));
 return OpenArchive(create.destination);
}
Job move(const std::vector<std::string>&sources,const std::string&dest){return PrepareArchiveMove(Job{Action::Move,sources,dest});}
Result workerRun(const Job &job) {
 struct Work {const Job &job;Result result;} work{job,{}};
 pthread_attr_t attributes;pthread_attr_init(&attributes);pthread_attr_setstacksize(&attributes,262144);
 pthread_t thread;int status=pthread_create(&thread,&attributes,[](void *p)->void*{auto &w=*static_cast<Work*>(p);w.result=Run(w.job,{});return nullptr;},&work);
 pthread_attr_destroy(&attributes);if(status)throw std::runtime_error("Cannot start native worker test");pthread_join(thread,nullptr);return work.result;
}
std::string fingerprint(const std::string &url){return FindArchiveVolumes(ParseLocation(url).archive).Fingerprint();}
template<class F>bool rejects(F f){try{f();return false;}catch(const std::exception&){return true;}}
}
int main(){
 BApplication app("application/x-vnd.Listi-archive-local-move-test");char pattern[]="/tmp/Listi-localmove-XXXXXX";auto made=mkdtemp(pattern);if(!made)return 2;root=made;
 mkdir((root+"/seed").c_str(),0700);std::ofstream(root+"/seed/keep")<<"retained";
 auto seed=root+"/seed/keep";
 auto local=[&](bool empty = true){auto p=root+"/local"+std::to_string(++serial);mkdir(p.c_str(),0700);mkdir((p+"/folder").c_str(),0700);if (empty) mkdir((p+"/folder/empty").c_str(),0700);std::ofstream(p+"/item")<<"payload";std::ofstream(p+"/folder/nested")<<std::string(160000,'x');return p;};
 auto output=[&]{auto p=root+"/out"+std::to_string(++serial);mkdir(p.c_str(),0700);return p;};
 try {
  for(const auto &format:WritableArchiveFormats()){
   const bool flat = format.id == "ar";
   const bool hasEmpty = !flat && format.id != "cab";
   auto input=local(hasEmpty),target=archive(format.id,{seed}),out=output();
   if (format.id == "arj" || format.id == "cab") {
     // DOS-style archives cannot represent a private directory mode.
     if (chmod((input+"/folder").c_str(),0755) ||
         (hasEmpty && chmod((input+"/folder/empty").c_str(),0755))) return 2;
   }
   if (format.id == "cab") {
     // libarchive decodes writable Cabinet files with mode 0666.
     if (chmod((input+"/item").c_str(),0666) || chmod((input+"/folder/nested").c_str(),0666)) return 2;
   }
   const std::string secondInput = input + (flat ? "/folder/nested" : "/folder");
   const auto name = Leaf(secondInput);
   auto item = ChildLocation(target,"item"), second = ChildLocation(target,name);
   auto result=workerRun(DecodeJob(EncodeJob(move({input+"/item",secondInput},target),0)));
   if(!result.errors.empty())std::cerr<<result.errors.front()<<std::endl;
   check(result.completed==2&&result.errors.empty()&&!result.cancelled,format.id+" local-to-archive completes on worker stack");
   check(!exists(input+"/item")&&!exists(secondInput),format.id+" verified sources removed");
   check(content(item)=="payload" && content(flat ? second : ChildLocation(second,"nested"))==std::string(160000,'x'),format.id+" archive bytes preserved");
   if (hasEmpty) check(exists(ChildLocation(second,"empty")),format.id+" empty directory archived");
   result=workerRun(move({item,second},out));
   if(!result.errors.empty())std::cerr<<result.errors.front()<<std::endl;
   check(result.completed==2&&result.errors.empty()&&!result.cancelled,format.id+" archive-to-local completes on worker stack");
   check(content(out+"/item")=="payload" && content(out+(flat ? "/nested" : "/folder/nested"))==std::string(160000,'x'),format.id+" local bytes preserved");
   if (hasEmpty) check(exists(out+"/folder/empty"),format.id+" empty directory restored");
   check(!exists(item)&&!exists(second)&&content(ChildLocation(target,"keep"))=="retained",format.id+" removes only selected archive members");
   check(PrepareArchiveEditTarget(ParseLocation(target).archive).archiveFormat==format.id,format.id+" archive format retained after both moves");
  }
  {
   auto input=local(), target=archive("zip",{seed});
   const auto item=input+"/item";
   const timespec stamp[2]={{1700000000,125000000},{1700000000,125000000}};
   if (utimensat(AT_FDCWD,item.c_str(),stamp,0)) return 2;
   auto result=workerRun(move({item},target));
   check(result.completed==1 && !exists(item) && result.copyLimitations && !result.outcomes.empty(),
         "local-to-ZIP records reduced timestamp precision in the job receipt");
  }
  {
   auto input=local(), target=archive("arj",{seed});
   const auto item=input+"/item";
   if (chmod(item.c_str(),0600)) return 2;
   auto result=workerRun(move({item},target));
   check(!result.completed && !result.errors.empty() && content(item)=="payload",
         "DOS format cannot discard the private local mode and remove the source");
  }
  for(const auto &format:WritableArchiveFormats())if(format.encryption){
   SetArchivePasswordPrompt([](const std::string&,bool,const TransferControl&){return std::make_shared<ArchiveSecret>("Listi-move-test");});
   auto input=local(),target=archive(format.id,{seed},true),out=output();auto j=move({input+"/item"},target);auto fp=fingerprint(target);
   ForgetArchivePasswords();SetArchivePasswordPrompt([](const std::string&,bool,const TransferControl&){return ArchiveSecretPtr{};});
   auto r=Run(j,{});check(r.cancelled&&!r.completed&&content(input+"/item")=="payload"&&fingerprint(target)==fp,format.id+" cancelled password retains local source");
   SetArchivePasswordPrompt([](const std::string&,bool,const TransferControl&){return std::make_shared<ArchiveSecret>("Listi-move-test");});
   r=workerRun(j);check(r.completed==1&&r.errors.empty()&&!exists(input+"/item"),format.id+" local move into encrypted archive");
   ForgetArchivePasswords();r=workerRun(move({ChildLocation(target,"item")},out));check(r.completed==1&&r.errors.empty()&&content(out+"/item")=="payload",format.id+" encrypted member moves to local");
   ForgetArchivePasswords();check(content(ChildLocation(target,"keep"))=="retained",format.id+" source archive still protected and readable");
   ForgetArchivePasswords();SetArchivePasswordPrompt({});
  }
  auto input=local(),target=archive("zip",{seed}),out=output();auto j=move({input+"/item"},target);auto fp=fingerprint(target);
  Hooks cancel;cancel.proceed=[] {return false;};auto r=Run(j,cancel);check(r.cancelled&&!r.completed&&exists(input+"/item")&&fingerprint(target)==fp,"early cancel preserves both sides");
  bool changed=false;Hooks during;during.progress=[&](const std::string&,uint64_t n){if(n&&!changed){changed=true;std::ofstream(input+"/item")<<"newdata";}};
  r=Run(j,during);check(!r.errors.empty()&&!r.completed&&content(input+"/item")=="newdata"&&fingerprint(target)==fp,"local edit during snapshot prevents publication and removal");
  std::ofstream(input+"/item")<<"payload";j=move({input+"/item"},target);fp=fingerprint(target);
  Hooks after;after.proceed=[&]{return fingerprint(target)==fp;};r=Run(j,after);
  check(r.cancelled&&!r.completed&&content(input+"/item")=="payload"&&content(ChildLocation(target,"item"))=="payload","cancel after archive commit retains both copies");
  Hooks skip;skip.conflict=[](const std::string&){return Decision::Skip;};r=Run(move({input+"/item"},target),skip);check(!r.completed&&!r.errors.empty()&&exists(input+"/item"),"skip archive conflict retains local source");
  Hooks replace;replace.conflict=[](const std::string&){return Decision::Replace;};
  changed=false;fp=fingerprint(target);Hooks race=replace;race.proceed=[&]{if(!changed&&fingerprint(target)!=fp){changed=true;std::ofstream(input+"/item")<<"changed";}return true;};
  r=Run(move({input+"/item"},target),race);check(changed&&!r.completed&&!r.errors.empty()&&content(input+"/item")=="changed","local change after archive commit blocks cleanup");
  std::ofstream(out+"/item")<<"old";auto member=ChildLocation(target,"item");r=Run(move({member},out),skip);check(!r.completed&&!r.errors.empty()&&exists(member)&&content(out+"/item")=="old","skip local conflict retains archive member");
  Hooks abort;abort.conflict=[](const std::string&){return Decision::Abort;};r=Run(move({member},out),abort);check(r.cancelled&&!r.completed&&exists(member)&&content(out+"/item")=="old","abort local conflict preserves both sides");
  r=Run(move({member},out),replace);check(r.completed==1&&r.errors.empty()&&!exists(member)&&content(out+"/item")=="payload","explicit local replace verifies then removes member");
  // Local destination changes during source rebuild must block archive commit.
  target=archive("tgz",{input+"/item",seed});member=ChildLocation(target,"item");out=output();changed=false;
  Hooks mutate;mutate.progress=[&](const std::string &path,uint64_t){if(!changed&&path==ParseLocation(target).archive&&exists(out+"/item")){changed=true;std::ofstream(out+"/item")<<"damaged";}};
  r=Run(move({member},out),mutate);check(changed&&!r.completed&&!r.errors.empty()&&exists(member),"local destination changed during cleanup retains source archive");
  // Source preflight refuses metadata and special nodes that cannot survive packing.
  input=local();target=archive("zip",{seed});auto before=fingerprint(target);
  symlink("item",(input+"/link").c_str());r=Run(move({input+"/link"},target),{});check(!r.completed&&!r.errors.empty()&&fingerprint(target)==before,"symlink source refused without mutation");
  if(!link((input+"/item").c_str(),(input+"/hard").c_str())) {r=Run(move({input+"/item"},target),{});check(!r.completed&&!r.errors.empty()&&exists(input+"/item"),"hard-linked source refused");unlink((input+"/hard").c_str());}
  else std::cout<<"NOTE native volume does not support hard links; refusal branch unqualified on this volume"<<std::endl;
  BNode node((input+"/item").c_str());node.WriteAttr("test:valuable",B_STRING_TYPE,0,"kept",5);r=Run(move({input+"/item"},target),{});check(!r.completed&&!r.errors.empty()&&fingerprint(target)==before&&exists(input+"/item"),"BFS attributes never discarded by move");node.RemoveAttr("test:valuable");
  check(rejects([&]{move({root},target);}),"source cannot contain destination archive");
  std::ofstream(input+"/self")<<"self-contained";auto self=PrepareArchiveJob(false,{input+"/self"});SetArchiveDestination(self,root,"self");check(Run(self,{}).completed==1&&rejects([&]{move({ChildLocation(OpenArchive(self.destination),"self")},root);}),"archive cannot overwrite itself with a member");
  check(rejects([&]{move({input+"/folder",input+"/folder/nested"},target);}),"overlapping local selections rejected");
  // New children must not be discovered and deleted during cleanup.
  auto tree=local(),treeArchive=archive("zip",{seed});auto treeBefore=fingerprint(treeArchive);bool added=false;
  Hooks addChild;addChild.proceed=[&]{if(!added&&fingerprint(treeArchive)!=treeBefore){added=true;std::ofstream(tree+"/folder/new")<<"new";}return true;};
  r=Run(move({tree+"/folder"},treeArchive),addChild);check(added&&!r.completed&&!r.errors.empty()&&content(tree+"/folder/new")=="new"&&exists(tree+"/folder/nested"),"new local child after copy blocks all source cleanup");
  auto queued=local();auto stale=move({queued+"/item"},target);rename((queued+"/item").c_str(),(queued+"/old").c_str());std::ofstream(queued+"/item")<<"replacement";
  r=Run(stale,{});check(!r.completed&&!r.errors.empty()&&content(queued+"/item")=="replacement","queued local source replacement is never removed");
  auto mergeInput=local();auto mergeArchive=archive("zip",{mergeInput+"/folder",mergeInput+"/item",seed});auto mergeOut=output();mkdir((mergeOut+"/folder").c_str(),0700);std::ofstream(mergeOut+"/folder/extra")<<"extra";
  r=Run(move({ChildLocation(mergeArchive,"folder")},mergeOut),{});check(r.completed==1&&r.errors.empty()&&content(mergeOut+"/folder/extra")=="extra"&&exists(mergeOut+"/folder/empty"),"local directory merge preserves extra destination entries");
  std::ofstream(mergeOut+"/keep")<<"old";r=Run(move({ChildLocation(mergeArchive,"item"),ChildLocation(mergeArchive,"keep")},mergeOut),skip);
  check(!r.completed&&!r.errors.empty()&&exists(ChildLocation(mergeArchive,"item"))&&exists(ChildLocation(mergeArchive,"keep"))&&content(mergeOut+"/item")=="payload","partial local copy with skip retains every archive source");
  // Both clipboard directions use the existing lease and completion rules.
  BClipboard clipboard("Listi-local-archive-cut-test",false);PutFileClipboard({input+"/item"},true,&clipboard);auto paste=PrepareFilePaste(target,&clipboard);
  check(!paste.lease.empty()&&!paste.archiveCut&&paste.jobs.size()==1&&paste.jobs[0].action==Action::MoveArchive,"local cut into archive reserves clipboard");
  check(rejects([&]{PrepareFilePaste(target,&clipboard);}),"pending local archive paste cannot duplicate");FinishFilePaste(paste,{},&clipboard);paste=PrepareFilePaste(target,&clipboard);
  r=Run(paste.jobs[0],{});FinishFilePaste(paste,r.completed==1?std::vector<std::string>{input+"/item"}:std::vector<std::string>{},&clipboard);
  clipboard.Lock(); BMessage empty(*clipboard.Data()); clipboard.Unlock();
  check(r.completed==1&&rejects([&]{ReadFileClipboard(empty);}),"successful local move consumes cut reference");
  member=ChildLocation(target,"item");PutFileClipboard({member},true,&clipboard);out=output();
  struct stat shown;
  check(stat(out.c_str(), &shown) == 0, "observe clipboard destination folder");
  const NodeIdentity shownFolder{int32_t(shown.st_dev), int64_t(shown.st_ino)};
  const auto held = out + "-held";
  check(rename(out.c_str(), held.c_str()) == 0 && mkdir(out.c_str(),0700) == 0 &&
        rejects([&]{PrepareFilePaste(out,&clipboard,{},shownFolder);}) &&
        exists(member) && !exists(out+"/item") && !exists(held+"/item"),
        "archive Cut refuses stale destination before reserving clipboard or moving bytes");
  paste=PrepareFilePaste(out,&clipboard);check(paste.archiveCut&&!paste.lease.empty(),"archive cut into local folder reserves clipboard");FinishFilePaste(paste,{},&clipboard);paste=PrepareFilePaste(out,&clipboard);
  r=Run(paste.jobs[0],{});FinishFilePaste(paste,{member},&clipboard);check(r.completed==1&&content(out+"/item")=="payload"&&rejects([&]{PrepareFilePaste(out,&clipboard);}),"successful archive-to-local move consumes archive selection");
 }catch(const std::exception&e){check(false,std::string("unexpected: ")+e.what());}
 clean(root);std::cout<<checks<<" checks, "<<failures<<" failed"<<std::endl;return failures?1:0;
}
