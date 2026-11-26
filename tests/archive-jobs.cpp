// SPDX-License-Identifier: MIT
#include "ArchiveActions.h"
#include "ArchiveWindow.h"
#include "ArchiveJobs.h"
#include "ArchivePasswords.h"
#include "ArchiveSettings.h"
#include "MessageStore.h"
#include "RecoveryInventory.h"
#include "JobSnapshot.h"
#include "JobReportStore.h"
#include "VirtualFile.h"
#include <Application.h>
#include <FindDirectory.h>
#include <TextControl.h>
#include <StringView.h>
#include <Window.h>
#include <algorithm>
#include <cstring>
#include <Directory.h>
#include <Entry.h>
#include <Path.h>
#include <fstream>
#include <iostream>
#include <map>
#include <Node.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
using namespace Listi;
int passed=0;
void check(bool ok,const std::string &name){if(!ok)throw std::runtime_error("FAIL "+name);std::cout<<"PASS "<<++passed<<" "<<name<<std::endl;}
void write(const std::string &p,const std::string &s){std::ofstream(p)<<s;}
std::string read(const std::string &p){std::ifstream f(p);return std::string(std::istreambuf_iterator<char>(f),{});}
std::string vread(const std::string &p){std::string out;ReadFile(p,[&](const char *bytes,size_t size){out.append(bytes,size);});return out;}
bool exists(const std::string &p){struct stat s;return !lstat(p.c_str(),&s);}
ArchiveSecretPtr secret(const char *text="Listi-create-test-Æ"){return std::make_shared<ArchiveSecret>(text);}
void clean(const std::string &p){BDirectory d(p.c_str());BEntry e;while(d.GetNextEntry(&e,false)==B_OK){if(e.IsDirectory()) {BPath path;e.GetPath(&path);clean(path.Path());}e.Remove();}rmdir(p.c_str());}
// Check the ZIP headers directly: a same-library round trip can read raw
// UTF-8 even when independent readers are told to interpret it as CP437.
void checkZipName(const std::string &path) {
  const auto bytes = read(path);
  const std::string name = "folder/æ name ; $(no).txt";
  auto marked = [&](const char *signature, size_t header, size_t flagOffset) {
    for (size_t at = bytes.find(name); at != std::string::npos;
         at = bytes.find(name, at + name.size())) {
      if (at >= header && bytes.compare(at - header, 4, signature, 4) == 0)
        return (bytes[at - header + flagOffset + 1] & 0x08) != 0;
    }
    return false;
  };
  check(marked("PK\x03\x04", 30, 6) && marked("PK\x01\x02", 46, 8),
        "ZIP local and central headers declare UTF-8 for the exact member name");
}
void checkSuggestedNames(const std::string &root) {
  const std::string prefix(250, 'x');
  const auto source = root + "/" + prefix + "🍕z";
  write(source, "fixture");
  auto job = PrepareArchiveJob(false, {source});
  auto address = OpenArchiveWindow(job, root, {});
  BWindow *dialog = nullptr;
  for (int i = 0; i < 200; ++i) {
    if (!address.LockTarget()) throw std::runtime_error("cannot lock archive dialog");
    dialog = dynamic_cast<BWindow *>(address.Target(nullptr));
    auto *label = dynamic_cast<BStringView *>(dialog->FindView("archive chosen format"));
    if (label && std::string(label->Text()) != "Format: checking...")
      break;
    dialog->Unlock();
    dialog = nullptr;
    snooze(50000);
  }
  check(dialog != nullptr, "archive format discovery completed");
  auto *name = dynamic_cast<BTextControl *>(dialog->FindView("archive name"));
  auto select = [&](const char *id) {
    BMessage msg('arfm');
    msg.AddString("format", id);
    dialog->MessageReceived(&msg);
  };
  select("tgz");
  check(std::string(name->Text()) == prefix.substr(0, 248) + ".tar.gz",
        "long source name leaves room for the entire archive suffix");
  select("zip");
  check(std::string(name->Text()) == prefix + ".zip",
        "switching back restores the full available UTF-8 prefix");
  SetArchiveDestination(job, root, name->Text());
  name->SetText("Field notes.ZIP");
  select("tgz");
  check(std::string(name->Text()) == "Field notes.tar.gz",
        "switching format replaces a custom uppercase suffix exactly once");
  dialog->Quit();

  Job extract;
  extract.action = Action::ExtractArchive;
  extract.sources = {root + "/Album.TAR.XZ"};
  auto extraction = OpenArchiveWindow(extract, root, {});
  check(extraction.LockTarget(), "lock extraction dialog");
  dialog = dynamic_cast<BWindow *>(extraction.Target(nullptr));
  name = dynamic_cast<BTextControl *>(dialog->FindView("archive name"));
  check(std::string(name->Text()) == "Album extracted",
        "extract suggestion strips a mixed-case compound archive suffix");
  dialog->Quit();
}

void *run(void *) {
 try{
  char pattern[]="/tmp/Listi-archive-jobs-XXXXXX";std::string root=mkdtemp(pattern);
  checkSuggestedNames(root);
  Job moveRequest;
  moveRequest.action = Action::MoveArchive;
  moveRequest.sources = {root + "/local.txt"};
  moveRequest.destination = root + "/target.zip";
  const auto movePrompt = ArchiveActions::ConfirmAddition(moveRequest, "test-lease");
  const auto acceptedMove = DecodeJob(movePrompt.accepted);
  check(movePrompt.accepted.what == kArchiveEditAccepted &&
            std::string(movePrompt.accepted.GetString("clipboardLease", "")) == "test-lease" &&
            acceptedMove.action == moveRequest.action &&
            acceptedMove.sources == moveRequest.sources &&
            acceptedMove.destination == moveRequest.destination &&
            std::string(movePrompt.button) == "Move entries",
        "archive move confirmation preserves the frozen request and clipboard lease");
  auto settings=LoadArchiveSettings(root+"/settings");
  check(settings.what!='none',"new archive settings are not the missing-file sentinel");
  settings.SetString("default","7z");settings.SetBool("ask",false);settings.SetBool("favorite:7z",true);settings.SetInt32("level:7z",3);
  SaveArchiveSettings(root+"/settings",settings);auto restored=LoadArchiveSettings(root+"/settings");
  check(std::string(restored.GetString("default",""))=="7z" && !restored.GetBool("ask",true) && restored.GetBool("favorite:7z",false) && restored.GetInt32("level:7z",-1)==3,"format preferences survive save and reload");
  restored.SetBool("ask",true);SaveArchiveSettings(root+"/settings",restored);check(LoadArchiveSettings(root+"/settings").GetBool("ask",false),"ask every time remains explicit with a saved default");
  auto originalSettings=read(root+"/settings");restored.SetInt32("version",99);bool invalid=false;
  try{SaveArchiveSettings(root+"/settings",restored);}catch(...){invalid=true;}
  check(invalid && read(root+"/settings")==originalSettings,"invalid settings cannot replace valid saved choices");
  write(root+"/bad-settings","broken");invalid=false;try{LoadArchiveSettings(root+"/bad-settings");}catch(...){invalid=true;}
  check(invalid && read(root+"/bad-settings")=="broken","corrupt settings report an error without overwriting the file");
  mkdir((root+"/in").c_str(),0700);mkdir((root+"/out").c_str(),0700);
  mkdir((root+"/.Listi-archive-stage-visible").c_str(),0700);
  auto visible=ListFiles(root);check(std::any_of(visible.begin(),visible.end(),
    [](const auto &entry){return entry.name==".Listi-archive-stage-visible";}),
    "unregistered archive staging names remain visible");
  std::string input=root+"/in",out=root+"/out";
  mkdir((input+"/folder").c_str(),0700);mkdir((input+"/folder/empty").c_str(),0700);
  write(input+"/folder/æ name ; $(no).txt","payload\n");write(input+"/empty","");
  auto formats=WritableArchiveFormats();check(!formats.empty(),"optional system writer available");
  std::string archive;std::map<std::string,std::string> madeArchives;
  for(const auto &f:formats){
    if (f.id == "cab") rmdir((input+"/folder/empty").c_str());
    const bool flat = f.id == "ar";
    const bool ascii = f.id == "arj";
    const std::string leaf = ascii ? "literal name.txt" : "æ name ; $(no).txt";
    if (ascii) rename((input+"/folder/æ name ; $(no).txt").c_str(),(input+"/folder/"+leaf).c_str());
    const auto payload = (flat ? "/" : "/folder/")+leaf;
    auto job=PrepareArchiveJob(false,{flat ? input+"/folder/"+leaf : input+"/folder",input+"/empty"});job.archiveFormat=f.id;
    SetArchiveDestination(job,out,"roundtrip"+f.extension);
    job=DecodeJob(EncodeJob(job,0));auto result=Run(job,{});
    if(!result.errors.empty())std::cerr<<result.errors.front()<<std::endl;
    check(result.completed==1 && result.errors.empty(),f.id+" creates archive");
    if (f.id == "zip") checkZipName(job.destination);
    auto extract=PrepareArchiveJob(true,{job.destination});SetArchiveDestination(extract,out,"extract-"+f.id);
    auto unpack=Run(extract,{});if(!unpack.errors.empty())std::cerr<<unpack.errors.front()<<std::endl;
    check(unpack.completed==1 && unpack.errors.empty(),f.id+" extracts transactionally");
    check(read(extract.destination+payload)=="payload\n",f.id+" member name and content round trip");
    check((flat || f.id == "cab" || exists(extract.destination+"/folder/empty")) && exists(extract.destination+"/empty"),f.id+" representable empty entries preserved");
    auto again=Run(job,{});check(!again.errors.empty() && read(extract.destination+payload)=="payload\n",f.id+" existing archive is not replaced");
    for(int level:{0,9}) {
      auto variant=job;variant.archiveLevel=level;SetArchiveDestination(variant,out,f.id+std::to_string(level)+f.extension);
      auto done=Run(variant,{});if(!done.errors.empty())std::cerr<<done.errors.front()<<std::endl;
      check(done.completed==1 && done.errors.empty(),f.id+" compression level "+std::to_string(level));
    }
    if (ascii) rename((input+"/folder/"+leaf).c_str(),(input+"/folder/æ name ; $(no).txt").c_str());
    if (f.id == "cab") mkdir((input+"/folder/empty").c_str(),0700);
    archive=job.destination;madeArchives[f.id]=job.destination;
  }
  // Exercise the public job path with material that actually benefits from
  // compression. A successful exit alone would not catch silent Store output.
  for (const auto &format : formats) {
    if ((format.id != "arj" && format.id != "cab") || !format.compression)
      continue;
    const std::string contents(128 * 1024, 'A');
    const auto member = input + "/compressible.txt";
    write(member, contents);
    auto job = PrepareArchiveJob(false, {member});
    job.archiveFormat = format.id;
    job.archiveLevel = 6;
    SetArchiveDestination(job, out, "compressed" + format.extension);
    auto result = Run(job, {});
    check(result.completed == 1 && result.errors.empty() &&
              read(job.destination).size() < contents.size() / 2,
          format.id + " job uses compression for repeated data");
    check(vread(ChildLocation(OpenArchive(job.destination), "compressible.txt")) == contents &&
              PrepareArchiveEditTarget(job.destination).archiveFormat == format.id,
          format.id + " compressed result is readable and editable");
  }
  if (madeArchives.count("zip")) {
    const auto original = madeArchives.at("zip");
    const auto originalBytes = read(original);
    const auto child = ChildLocation(OpenArchive(original), "cancelled-folder");
    auto edit = PrepareArchiveMutation(Job{Action::MakeDirectory, {}, child});
    BPath inventory;
    check(find_directory(B_USER_SETTINGS_DIRECTORY, &inventory) == B_OK &&
              inventory.Append("Listi-recovery") == B_OK,
          "locate recovery inventory for publication cancellation");
    // Observe only this fixture's journal. Request an ordinary cancellation
    // after the Publishing checkpoint, without changing any archive or stage.
    const std::string prefix = ".Listi-archive-stage-";
    std::string recordPath;
    bool sawPublishing = false;
    Hooks cancel;
    cancel.proceed = [&] {
      if (recordPath.empty()) {
        BDirectory folder(out.c_str());
        BEntry entry;
        char name[B_FILE_NAME_LENGTH];
        while (folder.GetNextEntry(&entry, false) == B_OK) {
          if (entry.GetName(name) == B_OK && std::string(name).find(prefix) == 0) {
            recordPath = std::string(inventory.Path()) + "/record-" +
                         std::string(name).substr(prefix.size());
            break;
          }
        }
      }
      if (!recordPath.empty()) {
        const auto record = LoadMessageStore(recordPath, true);
        sawPublishing = record.GetString("destination", "") == edit.destination &&
                        record.GetInt32("phase", -1) == int(RecoveryPhase::Publishing);
      }
      return !sawPublishing;
    };
    const auto stopped = RunArchiveJob(edit, cancel);
    check(sawPublishing && stopped.cancelled && stopped.errors.empty() &&
              stopped.completed == 0 && read(original) == originalBytes,
          "cancellation after the publication checkpoint preserves the archive");
  }
  if (madeArchives.count("iso")) {
    // Renamed images still have the same format; copying bytes without the
    // verification attribute must not grant permission to rebuild an image.
    const auto original = madeArchives.at("iso");
    const auto renamed = out + "/without-extension";
    check(rename(original.c_str(), renamed.c_str()) == 0 &&
              PrepareArchiveEditTarget(renamed).archiveFormat == "iso",
          "Listi data ISO remains editable when renamed");
    const auto unmarked = out + "/unmarked.iso";
    const auto image = read(renamed);
    write(unmarked, image);
    bool unmarkedRefused = false;
    try { PrepareArchiveEditTarget(unmarked); }
    catch (const std::exception &) { unmarkedRefused = true; }
    check(unmarkedRefused && !OpenArchive(unmarked).empty() &&
              read(unmarked) == image,
          "unmarked ISO can be read but is never rebuilt");
    const auto iso = OpenArchive(renamed);
    const auto added = ChildLocation(iso, "new-folder");
    const auto pending = PrepareArchiveMutation(
        Job{Action::MakeDirectory, {}, added});
    auto tampered = image;
    tampered[0] = 1; // System area: outside the visible filesystem.
    write(renamed, tampered);
    const auto refused = Run(pending, {});
    bool changedRefused = false;
    try { PrepareArchiveEditTarget(renamed); }
    catch (const std::exception &) { changedRefused = true; }
    check(changedRefused && !refused.errors.empty() &&
              read(renamed) == tampered && !OpenArchive(renamed).empty(),
          "changed ISO retains exact bytes and remains readable");
    write(renamed, image);
    auto edited = Run(PrepareArchiveMutation(
        Job{Action::MakeDirectory, {}, added}), {});
    FileEntry info;
    check(edited.completed == 1 && StatFile(added, info) && info.directory &&
              PrepareArchiveEditTarget(renamed).archiveFormat == "iso",
          "data ISO contents can be edited and reverified");
    check(rename(renamed.c_str(), original.c_str()) == 0, "restore renamed ISO fixture");
  }
  mkdir((input+"/edit").c_str(),0700);mkdir((input+"/edit/replacement").c_str(),0700);
  write(input+"/edit/replacement/empty","replacement\n");write(input+"/edit/new.txt","new\n");
  for(const auto &id:{std::string("zip"),std::string("7z")})if(madeArchives.count(id)){
    const auto original=madeArchives[id];auto rootURL=OpenArchive(original);
    auto removed=ChildLocation(ChildLocation(rootURL,"folder"),"æ name ; $(no).txt");
    BNode metadata(original.c_str());const char marker[]="kept";
    check(metadata.WriteAttr("Listi:edit-test",B_STRING_TYPE,0,marker,sizeof(marker))==sizeof(marker),id+" edit fixture metadata");
    auto selfTarget=PrepareArchiveEditTarget(original,"/");bool selfRefused=false;
    try{SetArchiveEditAdditions(selfTarget,{original});}catch(...){selfRefused=true;}
    check(selfRefused,id+" archive cannot add itself during editing");
    if (id == "zip") {
      const auto addition = input + "/edit/unsupported";
      mkdir(addition.c_str(), 0700);
      const auto unsupported = addition + "/pipe";
      check(mkfifo(unsupported.c_str(), 0600) == 0, "create unsupported addition fixture");
      const auto initial = read(original);
      Hooks queued;
      queued.report = std::make_shared<JobReportStore>();
      auto rejected = RunArchiveJob(PrepareArchiveEditJob(original, {addition}, {removed}), queued);
      check(!rejected.completed && !rejected.errors.empty() && read(original) == initial,
            "queued archive edit with unsupported addition retains every original byte");
      unlink(unsupported.c_str());
      rmdir(addition.c_str());
    }
    auto edit=PrepareArchiveEditJob(original,{input+"/edit/replacement/empty",input+"/edit/new.txt"},{removed},"/");
    edit.archiveLevel=4;auto encoded=DecodeJob(EncodeJob(edit,0));
    check(encoded.action==Action::EditArchive&&encoded.archiveRemove.size()==1&&encoded.archivePath.empty(),id+" edit snapshot round trip");
    std::vector<std::string> progressPaths,finishedPaths;
    std::vector<std::string> conflicts;
    Hooks replace;replace.conflict=[&](const std::string &path){conflicts.push_back(path);return Decision::Replace;};
    replace.progress=[&](const std::string &path,uint64_t){progressPaths.push_back(path);};
    replace.fileDone=[&](const std::string &path,const std::string &){finishedPaths.push_back(path);};
    auto updated=RunArchiveJob(encoded,replace);if(!updated.errors.empty())std::cerr<<updated.errors.front()<<std::endl;
    check(updated.completed==1&&updated.errors.empty(),id+" add replace and remove atomically");
    check(!progressPaths.empty()&&std::all_of(progressPaths.begin(),progressPaths.end(),
      [&](const auto &path){return path==original;})&&finishedPaths==std::vector<std::string>{original},
      id+" internal work reports only the archive target");
    check(conflicts.size()==1&&conflicts[0].find(original+" → /")==0&&
      conflicts[0].find(".Listi-archive-stage-")==std::string::npos,
      id+" conflict prompt hides private staging paths");
    rootURL=OpenArchive(original);FileEntry gone;
    check(!StatFile(ChildLocation(ChildLocation(rootURL,"folder"),"æ name ; $(no).txt"),gone),id+" removed member absent");
    check(vread(ChildLocation(rootURL,"empty"))=="replacement\n"&&vread(ChildLocation(rootURL,"new.txt"))=="new\n",id+" replacement and addition exact");
    BNode preserved(original.c_str());char attr[8]{};
    check(preserved.ReadAttr("Listi:edit-test",B_STRING_TYPE,0,attr,sizeof(attr))==sizeof(marker)&&!strcmp(attr,marker),id+" archive metadata preserved");
    auto before=read(original);auto cancelled=PrepareArchiveEditJob(original,{input+"/edit/new.txt"},{},"/");
    Hooks stop;stop.proceed=[] {return false;};auto stopped=RunArchiveJob(cancelled,stop);
    check(stopped.cancelled&&read(original)==before,id+" cancelled edit preserves original bytes");
    write(input+"/edit/replacement/new.txt","before");
    auto changed=PrepareArchiveEditJob(original,{input+"/edit/replacement/new.txt"},{},"/");
    bool touched=false;Hooks mutateAdd;mutateAdd.conflict=[&](const std::string &){
      touched=true;write(input+"/edit/replacement/new.txt","after");return Decision::Replace;};
    auto refused=RunArchiveJob(changed,mutateAdd);
    check(touched&&!refused.errors.empty()&&read(original)==before,id+" changed addition preserves original archive");
    auto frozen=PrepareArchiveEditTarget(original,"/");const auto frozenBytes=read(original);
    write(original,frozenBytes+"changed while picker was open");bool pickerRefused=false;
    try{SetArchiveEditAdditions(frozen,{input+"/edit/new.txt"});}catch(...){pickerRefused=true;}
    check(pickerRefused,id+" picker rejects an archive changed after selection");
    write(original,frozenBytes);
    auto single=PrepareArchiveJob(false,{input+"/edit/new.txt"});single.archiveFormat=id;
    const auto extension=id=="zip"?".zip":".7z";SetArchiveDestination(single,out,"empty-edit-"+id+extension);
    auto singleMade=RunArchiveJob(single,{});auto singleRoot=OpenArchive(single.destination);
    auto only=ChildLocation(singleRoot,"new.txt");auto emptyEdit=PrepareArchiveEditJob(single.destination,{},{only},"/");
    auto emptied=RunArchiveJob(emptyEdit,{});auto emptyRoot=OpenArchive(single.destination);
    check(singleMade.completed==1&&emptied.completed==1&&ListFiles(emptyRoot).empty(),id+" remove-only edit can produce an empty archive");
  }
  for(const auto &format:formats)if(format.encryption){
    const std::string password="Listi-create-test-Æ";
    auto encrypted=PrepareArchiveJob(false,{input+"/folder",input+"/empty"});
    encrypted.archiveFormat=format.id;encrypted.archiveLevel=7;
    encrypted.archiveEncryptNames=format.encryptedNames;
    encrypted.archivePasswordToken=HoldArchiveCreationPassword(secret(password.c_str()));
    SetArchiveDestination(encrypted,out,"encrypted-"+format.id+format.extension);
    auto message=EncodeJob(encrypted,0);std::vector<char> flattened(message.FlattenedSize());
    check(message.Flatten(flattened.data(),flattened.size())==B_OK &&
      std::search(flattened.begin(),flattened.end(),password.begin(),password.end())==flattened.end(),
      format.id+" job snapshot excludes password bytes");
    encrypted=DecodeJob(message);
    auto made=RunArchiveJob(encrypted,{});
    if(!made.errors.empty())std::cerr<<made.errors.front()<<std::endl;
    check(made.completed==1 && made.errors.empty(),format.id+" AES-256 archive creation");
    if (format.id == "zip") checkZipName(encrypted.destination);
    int prompts=0;ForgetArchivePasswords();
    SetArchivePasswordPrompt([&](const std::string &,bool,const TransferControl &){++prompts;return secret(password.c_str());});
    auto encryptedURL=OpenArchive(encrypted.destination);
    auto top=ListFiles(encryptedURL);
    auto privateEntry=ChildLocation(ChildLocation(encryptedURL,"folder"),"æ name ; $(no).txt");
    check(top.size()==2 && vread(privateEntry)=="payload\n" && prompts==1,
      format.id+" encrypted archive requires and accepts password");
    auto unpack=PrepareArchiveJob(true,{encrypted.destination});
    SetArchiveDestination(unpack,out,"decrypted-"+format.id);
    auto extracted=RunArchiveJob(unpack,{});
    check(extracted.completed==1 && extracted.errors.empty() &&
      read(unpack.destination+"/folder/æ name ; $(no).txt")=="payload\n",
      format.id+" encrypted round trip exact");
    write(input+"/edit/encrypted-add.txt","private addition\n");
    auto encryptedEdit=PrepareArchiveEditJob(encrypted.destination,
        {input+"/edit/encrypted-add.txt",input+"/empty"},{},"/");
    bool evicted = false;
    Hooks evictOnConflict;
    evictOnConflict.conflict = [&](const std::string &) {
      const auto generation = ArchivePasswordGeneration();
      for (int i = 0; i < 17; ++i)
        RememberArchivePassword("unrelated-" + std::to_string(i), secret(), generation);
      evicted = true;
      return Decision::Replace;
    };
    auto encryptedUpdated=RunArchiveJob(encryptedEdit,evictOnConflict);
    check(evicted, format.id+" edit experienced password-cache eviction");
    if(!encryptedUpdated.errors.empty())std::cerr<<encryptedUpdated.errors.front()<<std::endl;
    check(encryptedUpdated.completed==1&&encryptedUpdated.errors.empty(),format.id+" encrypted archive edit");
    if (format.id == "zip") checkZipName(encrypted.destination);
    ForgetArchivePasswords();int editPrompts=0;
    SetArchivePasswordPrompt([&](const std::string &,bool,const TransferControl &){++editPrompts;return secret(password.c_str());});
    auto editedURL=OpenArchive(encrypted.destination);
    const auto editedContent=vread(ChildLocation(editedURL,"encrypted-add.txt"));
    if(editedContent!="private addition\n"||editPrompts!=1)
      std::cerr<<format.id<<" edited prompt count "<<editPrompts<<", content bytes "<<editedContent.size()<<std::endl;
    check(editedContent=="private addition\n"&&editPrompts==1,
      format.id+" edited archive keeps password protection");
    auto missing=PrepareArchiveJob(false,{input+"/empty"});missing.archiveFormat=format.id;
    missing.archivePasswordToken=encrypted.archivePasswordToken;
    SetArchiveDestination(missing,out,"missing-secret-"+format.id+format.extension);
    auto unavailable=RunArchiveJob(missing,{});
    check(unavailable.cancelled && !exists(missing.destination),format.id+" consumed secret token cannot be reused");
    auto stopped=PrepareArchiveJob(false,{input+"/folder"});stopped.archiveFormat=format.id;
    stopped.archivePasswordToken=HoldArchiveCreationPassword(secret(password.c_str()));
    stopped.archiveEncryptNames=format.encryptedNames;
    SetArchiveDestination(stopped,out,"cancel-encrypted-"+format.id+format.extension);
    bool wrote=false;Hooks stopAfterBytes;
    stopAfterBytes.progress=[&](const std::string &,uint64_t){wrote=true;};
    stopAfterBytes.proceed=[&]{return !wrote;};
    auto cancelledArchive=RunArchiveJob(stopped,stopAfterBytes);
    check(cancelledArchive.cancelled && !exists(stopped.destination),
      format.id+" encrypted cancel publishes no partial archive");
    auto changedEncrypted=PrepareArchiveJob(false,{input+"/folder"});changedEncrypted.archiveFormat=format.id;
    changedEncrypted.archivePasswordToken=HoldArchiveCreationPassword(secret(password.c_str()));
    changedEncrypted.archiveEncryptNames=format.encryptedNames;
    SetArchiveDestination(changedEncrypted,out,"changed-encrypted-"+format.id+format.extension);
    bool edited=false;Hooks editDuringRead;
    editDuringRead.progress=[&](const std::string &,uint64_t){if(!edited){edited=true;write(input+"/folder/æ name ; $(no).txt","changed");}};
    auto changedResult=RunArchiveJob(changedEncrypted,editDuringRead);
    check(!changedResult.errors.empty() && !exists(changedEncrypted.destination),
      format.id+" encrypted source change publishes no archive");
    write(input+"/folder/æ name ; $(no).txt","payload\n");
  }
  ForgetArchivePasswords();
  auto url=OpenArchive(archive);
  auto subset=PrepareArchiveJob(true,{url+"folder",url+"empty"});SetArchiveDestination(subset,out,"selected-members");
  auto subsetResult=Run(subset,{});
  check(subsetResult.completed==1 && read(subset.destination+"/folder/æ name ; $(no).txt")=="payload\n" && exists(subset.destination+"/empty"),"multiple selected archive entries extract together");
  auto late=PrepareArchiveJob(true,{archive});SetArchiveDestination(late,out,"late-directory");Hooks lateHook;bool lateMade=false;
  lateHook.progress=[&](auto &,uint64_t){if(!lateMade){lateMade=true;mkdir(late.destination.c_str(),0700);write(late.destination+"/existing","preserve");}};
  check(!Run(late,lateHook).errors.empty() && read(late.destination+"/existing")=="preserve","late extraction destination is preserved");
  write(input+"/C:unsafe","payload");auto drive=PrepareArchiveJob(false,{input+"/C:unsafe"});SetArchiveDestination(drive,out,"drive.zip");
  check(!Run(drive,{}).errors.empty()&&!exists(drive.destination),"unsafe drive-style archive names fail explicitly");unlink((input+"/C:unsafe").c_str());
  auto create=[&](std::string name){auto j=PrepareArchiveJob(false,{input+"/folder"});SetArchiveDestination(j,out,name);return j;};
  auto j=create("cancel.zip");Hooks cancel;cancel.proceed=[] {return false;};
  check(Run(j,cancel).cancelled&&!exists(j.destination),"cancel creation leaves no output");
  auto ex=PrepareArchiveJob(true,{archive});SetArchiveDestination(ex,out,"cancel-extract");
  check(Run(ex,cancel).cancelled&&!exists(ex.destination),"cancel extraction leaves no output");
  Hooks midCancel;bool progressed=false;midCancel.progress=[&](auto &,uint64_t){progressed=true;};midCancel.proceed=[&]{return !progressed;};
  j=create("mid-cancel.zip");check(Run(j,midCancel).cancelled&&!exists(j.destination),"cancel after bytes written removes staging");
  progressed=false;SetArchiveDestination(ex,out,"mid-extract");check(Run(ex,midCancel).cancelled&&!exists(ex.destination),"cancel after extraction bytes removes staging");
  auto source=PrepareArchiveJob(false,{input+"/empty"});rename((input+"/empty").c_str(),(input+"/original").c_str());write(input+"/empty","replacement");
  bool denied=false;try{SetArchiveDestination(source,out,"replaced.zip");}catch(...){denied=true;}
  check(denied,"replaced selected file rejected before queueing");
  j=create("replaced-dest.zip");rename(out.c_str(),(root+"/old-out").c_str());mkdir(out.c_str(),0700);
  check(!Run(j,{}).errors.empty()&&!exists(j.destination),"replaced destination rejected");
  rmdir(out.c_str());rename((root+"/old-out").c_str(),out.c_str());
  j=create("concurrent.zip");bool changed=false;Hooks conflict;conflict.progress=[&](auto &,uint64_t){if(!changed){changed=true;write(j.destination,"keep me");}};
  check(!Run(j,conflict).errors.empty()&&read(j.destination)=="keep me","late target creation is never overwritten");
  j=create("changed.zip");Hooks mutate;changed=false;mutate.progress=[&](auto &,uint64_t){if(!changed){changed=true;write(input+"/folder/æ name ; $(no).txt","changed during read");}};
  check(!Run(j,mutate).errors.empty()&&!exists(j.destination),"changed source aborts publication");
  j=create("inside.zip");SetArchiveDestination(j,input+"/folder","inside.zip");
  check(!Run(j,{}).errors.empty()&&!exists(j.destination),"output inside selected tree rejected");
  symlink("/boot/home",(input+"/folder/link").c_str());j=create("links.zip");
  check(!Run(j,{}).errors.empty()&&!exists(j.destination),"links are not followed or silently omitted");unlink((input+"/folder/link").c_str());
  write(out+"/broken.zip","PK broken");ex=PrepareArchiveJob(true,{out+"/broken.zip"});SetArchiveDestination(ex,out,"broken-extract");
  check(!Run(ex,{}).errors.empty()&&!exists(ex.destination),"damaged archive does not publish partial folder");
  BDirectory d(out.c_str());BEntry e;bool leftover=false;while(d.GetNextEntry(&e,false)==B_OK){char n[B_FILE_NAME_LENGTH];e.GetName(n);if(std::string(n).find(".Listi-archive-")==0)leftover=true;}
  check(!leftover,"staging removed after success, error and cancel");
  clean(root);std::cout<<"ARCHIVE JOBS COMPLETE "<<passed<<std::endl;return nullptr;
 }catch(const std::exception &e){std::cerr<<e.what()<<std::endl;return reinterpret_cast<void*>(1);}
}
int main(){BApplication app("application/x-vnd.Listi-ArchiveJobsTest");pthread_attr_t attr;pthread_attr_init(&attr);pthread_attr_setstacksize(&attr,256*1024);pthread_t thread;pthread_create(&thread,&attr,run,nullptr);void *result;pthread_join(thread,&result);pthread_attr_destroy(&attr);return result?1:0;}
