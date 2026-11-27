// SPDX-License-Identifier: MIT
#include "ArchiveBackend.h"
#include "ArchiveJobs.h"
#include "ArchivePasswords.h"
#include "ArchiveVolumes.h"
#include "JobSnapshot.h"
#include <Application.h>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
using namespace Listi;
namespace {
int checks=0;
void check(bool ok,const std::string &name){if(!ok)throw std::runtime_error("FAIL "+name);++checks;std::cout<<"PASS "<<name<<std::endl;}
std::string disk(const std::string &p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void write(const std::string &p,const std::string &bytes){std::ofstream out(p,std::ios::binary);out.write(bytes.data(),bytes.size());}
bool exists(const std::string &p){struct stat s;return lstat(p.c_str(),&s)==0;}
void clean(const std::string &p){BDirectory d(p.c_str());BEntry e;while(d.GetNextEntry(&e,false)==B_OK){BPath path;e.GetPath(&path);struct stat s;if(!lstat(path.Path(),&s)&&S_ISDIR(s.st_mode))clean(path.Path());e.Remove();}rmdir(p.c_str());}
std::string read(const std::string &p){std::string bytes;ReadFile(p,[&](const char *b,size_t n){bytes.append(b,n);});return bytes;}
void password(){ForgetArchivePasswords();SetArchivePasswordPrompt([](const std::string &,bool,const TransferControl &){return std::make_shared<ArchiveSecret>("Listi-test-only");});}
template<class F> bool fails(F f){try{f();return false;}catch(const std::exception &e){std::cout<<"EXPECTED "<<e.what()<<std::endl;return true;}}
size_t walk(const std::string &p){size_t total=0;for(auto &e:ListFiles(p)){if(e.directory)total+=walk(e.path);else{auto data=read(e.path);check(e.sizeKnown&&int64_t(data.size())==e.size,"nested multipart member exact declared size and CRC");total+=data.size();}}return total;}
void *run(void *){try{
 char cwd[4096];getcwd(cwd,sizeof(cwd));std::string base=std::string(cwd)+"/tests/fixtures/multipart/";
 char pattern[]="/tmp/Listi-multipart-XXXXXX";std::string root=mkdtemp(pattern);mkdir((root+"/out").c_str(),0700);
 int serial=0;
 auto clone=[&](const std::string &leaf){std::string folder=root+"/set"+std::to_string(++serial);mkdir(folder.c_str(),0700);for(auto &part:FindArchiveVolumes(base+leaf).files)write(folder+"/"+LocationLeaf(part.path),disk(part.path));return folder+"/"+leaf;};
 check(SevenZipAvailable(),"optional engine available");
 {
   const auto first = root + "/Trip.rar", second = root + "/trip.rar";
   write(first, disk(base + "../sevenzip/rar4-data.rar"));
   write(second, disk(base + "../sevenzip/rar5-mixed.rar"));
   for (const auto &path : {first, second}) {
     const auto volumes = FindArchiveVolumes(path);
     check(volumes.kind == ArchiveVolumes::Single && volumes.files.size() == 1 &&
           volumes.files.front().path == path && ParseLocation(OpenArchive(path)).archive == path,
           "case-distinct standalone RAR opens the selected root");
   }
   const auto numbered = root + "/trip.r00";
   write(numbered, disk(base + "legacy.r00"));
   for (const auto &path : {first, second, numbered})
     check(fails([&]{ FindArchiveVolumes(path); }),
           "numbered RAR parts still reject ambiguous roots");
 }
 for(const char *leaf:{"plain.7z.001","plain.7z.004","encrypted.7z.002","plain.part3.rar","encrypted.part4.rar"}){
  password();auto parts=FindArchiveVolumes(base+leaf);check(parts.files.size()==4,"four parts discovered from any selected part");auto url=OpenArchive(base+leaf);
  check(ParseLocation(url).archive==parts.files.front().path,"virtual location canonicalizes first part");
  check(read(ChildLocation(url,"note.txt"))==disk(base+"original/note.txt"),"multipart small member exact bytes");
  check(read(ChildLocation(url,"payload.bin"))==disk(base+"original/payload.bin"),"multipart spanning member exact bytes");
  auto job=PrepareArchiveJob(true,{base+leaf});check(job.expectedSources.size()==4,"job freezes all four parts");job=DecodeJob(EncodeJob(job,0));check(job.expectedArchiveSets.size()==1,"part-set fingerprint survives queued message");
  SetArchiveDestination(job,root+"/out",leaf);auto result=RunArchiveJob(job,{});if(!result.errors.empty())std::cerr<<result.errors[0]<<std::endl;
  check(result.errors.empty()&&result.completed==1&&disk(job.destination+"/payload.bin")==disk(base+"original/payload.bin"),"atomic whole-set extraction exact");
 }
 for (const char *leaf : {"plain.7z.001", "plain.part1.rar", "legacy.rar"}) {
   const auto fixture = FindArchiveVolumes(base + leaf);
   const bool legacyRar = std::string(leaf) == "legacy.rar";
   const std::string member = legacyRar ? "LibarchiveAddingTest.html" : "payload.bin";
   const auto expected = legacyRar ? read(ChildLocation(OpenArchive(base + leaf), member))
                                   : disk(base + "original/" + member);
   for (char separator : {'\n', '\r'}) {
     const auto folder = root + "/set" + std::to_string(++serial);
     mkdir(folder.c_str(), 0700);
     std::vector<std::string> paths;
     for (const auto &part : fixture.files) {
       paths.push_back(folder + "/line" + separator + LocationLeaf(part.path));
       write(paths.back(), disk(part.path));
     }
     const auto middle = paths[paths.size() / 2];
     for (const auto &selected : {paths.front(), middle}) {
       const auto set = FindArchiveVolumes(selected);
       check(set.files.size() == paths.size() && set.files.front().path == paths.front(),
             "multipart basename with line break resolves full set from first or middle");
       auto job = PrepareArchiveJob(true, {selected});
       SetArchiveDestination(job, root + "/out", "decoded" + std::to_string(++serial));
       const auto result = RunArchiveJob(job, {});
       check(result.completed == 1 && result.errors.empty() &&
                 disk(job.destination + "/" + member) == expected,
             "multipart basename with line break extracts exact content");
     }
     auto job = PrepareArchiveJob(true, {paths.front()});
     SetArchiveDestination(job, root + "/out", "stale" + std::to_string(serial));
     const auto replacement = folder + "/replacement";
     write(replacement, disk(middle));
     check(rename(replacement.c_str(), middle.c_str()) == 0, "install new middle-part identity");
     const auto result = RunArchiveJob(job, {});
     check(!result.errors.empty() && !result.completed && !exists(job.destination),
           "changed middle-part identity invalidates confirmed extraction");
   }
 }
 auto legacy=OpenArchive(base+"legacy.r01");auto a=read(ChildLocation(legacy,"LibarchiveAddingTest.html"));check(a.size()==20111&&a==read(ChildLocation(legacy,"LibarchiveAddingTest2.html")),"old RAR volumes decode both members identically");
 auto solid=OpenArchive(base+"test_read_format_rar5_multiarchive_solid.part03.rar");check(walk(solid)>0,"solid nested RAR5 set decodes");
 for(const char *leaf:{"plain.7z.001","plain.part1.rar","legacy.rar"}){
  for(int which:{0,1,-1}){auto selected=clone(leaf);auto set=FindArchiveVolumes(selected);auto victim=set.files[which<0?set.files.size()-1:which].path;unlink(victim.c_str());std::string surviving=set.files[which==0?1:0].path;
   check(fails([&]{auto url=OpenArchive(surviving);walk(url);}),"missing first/middle/last part rejected");
  }
  auto selected=clone(leaf);auto set=FindArchiveVolumes(selected);auto bytes=disk(set.files[1].path);bytes[bytes.size()/2]^=0x51;write(set.files[1].path,bytes);auto job=PrepareArchiveJob(true,{selected});SetArchiveDestination(job,root+"/out","damaged"+std::to_string(serial));auto result=RunArchiveJob(job,{});
  check(!result.errors.empty()&&!exists(job.destination),"corrupt middle part publishes no destination");
 }
 for(const char *leaf:{"plain.7z.001","plain.part1.rar"}){auto selected=clone(leaf);write(selected,"not an archive");check(fails([&]{OpenArchive(selected);}),"damaged first part never falls through to external opening");}
 auto selected=clone("plain.7z.001");auto set=FindArchiveVolumes(selected);unlink(set.files[1].path.c_str());symlink((base+"plain.7z.002").c_str(),set.files[1].path.c_str());check(fails([&]{FindArchiveVolumes(selected);}),"symlink part refused");
 selected=clone("plain.7z.001");write(selected.substr(0,selected.size()-3)+"005","extra");check(fails([&]{OpenArchive(selected);}),"extra trailing 7z part refused");
 selected=clone("plain.part1.rar");write(selected.substr(0,selected.size()-5)+"5.rar",disk(base+"plain.part4.rar"));check(fails([&]{OpenArchive(selected);}),"unconsumed RAR part refused");
 selected=clone("plain.7z.001");write(selected.substr(0,selected.size()-12)+"PLAIN.7Z.001",disk(selected));check(fails([&]{FindArchiveVolumes(selected);}),"ambiguous case-folded part refused");
 selected=clone("plain.7z.001");
 const auto alternate=selected.substr(0,selected.size()-3)+"0001";
 check(!rename(selected.c_str(),alternate.c_str()) &&
       fails([&]{FindArchiveVolumes(selected);}) &&
       FindArchiveVolumes(alternate).files.size()==4,
       "missing selected pathname cannot silently open an alternate part number spelling");
 for(const char *leaf:{"encrypted.7z.002","encrypted.part2.rar"}){
  password();int calls=0;SetArchivePasswordPrompt([&](const std::string &,bool,const TransferControl &){return std::make_shared<ArchiveSecret>(++calls==1?"wrong":"Listi-test-only");});check(read(ChildLocation(OpenArchive(base+leaf),"payload.bin"))==disk(base+"original/payload.bin")&&calls==2,"multipart wrong password retry");
  password();SetArchivePasswordPrompt([](const std::string &,bool,const TransferControl &){return ArchiveSecretPtr();});auto job=PrepareArchiveJob(true,{base+leaf});SetArchiveDestination(job,root+"/out",std::string("cancel-")+leaf);auto result=RunArchiveJob(job,{});check(result.cancelled&&!exists(job.destination),"multipart password cancel publishes nothing");
  selected=clone(leaf);set=FindArchiveVolumes(selected);password();SetArchivePasswordPrompt([&](const std::string &,bool,const TransferControl &){auto p=set.files[1].path;rename(p.c_str(),(p+".old").c_str());write(p,disk(p+".old"));return std::make_shared<ArchiveSecret>("Listi-test-only");});check(fails([&]{OpenArchive(selected);}),"nonfirst part replaced during password prompt refused");
 }
 password();selected=clone("plain.7z.001");auto job=PrepareArchiveJob(true,{selected});set=FindArchiveVolumes(selected);write(set.files[1].path,disk(set.files[1].path)+"change");check(fails([&]{SetArchiveDestination(job,root+"/out","changed");}),"part size change before confirmation refused");
 selected=clone("plain.7z.001");job=PrepareArchiveJob(true,{selected});SetArchiveDestination(job,root+"/out","added");write(selected.substr(0,selected.size()-3)+"005","new");check(!RunArchiveJob(job,{}).errors.empty()&&!exists(job.destination),"added part after confirmation refused");
 selected=clone("plain.part1.rar");job=PrepareArchiveJob(true,{selected});SetArchiveDestination(job,root+"/out","late-change");set=FindArchiveVolumes(selected);bool changed=false;Hooks mutate;mutate.progress=[&](auto &,uint64_t){if(!changed){changed=true;write(set.files.back().path,disk(set.files.back().path)+"late");}};auto result=RunArchiveJob(job,mutate);check(changed&&!result.errors.empty()&&!exists(job.destination),"part changed after first member cannot publish partial folder");
 job=PrepareArchiveJob(true,{base+"plain.7z.001"});SetArchiveDestination(job,root+"/out","cancel-progress");bool progressed=false;Hooks cancel;cancel.progress=[&](auto &,uint64_t){progressed=true;};cancel.proceed=[&]{return !progressed;};result=RunArchiveJob(job,cancel);check(result.cancelled&&!exists(job.destination),"cancel after decoded bytes cleans destination");
 BDirectory d((root+"/out").c_str());BEntry e;bool leftover=false;while(d.GetNextEntry(&e,false)==B_OK){char name[B_FILE_NAME_LENGTH];e.GetName(name);if(std::string(name).find(".Listi-archive-")==0)leftover=true;}check(!leftover,"no staging after failure or cancellation");
 StopArchivePasswords();clean(root);std::cout<<checks<<" multipart checks passed on 256 KiB stack"<<std::endl;return nullptr;
 }catch(const std::exception &e){std::cerr<<e.what()<<std::endl;return reinterpret_cast<void*>(1);}}
}
int main(){BApplication app("application/x-vnd.Listi-Multipart-Test");pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,256*1024);pthread_t t;pthread_create(&t,&attr,run,nullptr);pthread_attr_destroy(&attr);void *result;pthread_join(t,&result);return result?1:0;}
