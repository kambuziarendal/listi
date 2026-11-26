// SPDX-License-Identifier: MIT
#include "FileProperties.h"
#include <Application.h>
#include <File.h>
#include <Message.h>
#include <fstream>
#include <iostream>
#include <cstring>
#include <fcntl.h>
#include <fs_attr.h>
#include <unistd.h>
using namespace Listi;
static int fault=0,passed=0;
extern "C" ssize_t RealWrite(BNode*,const char*,type_code,off_t,const void*,size_t) asm("__real__ZN5BNode9WriteAttrEPKcjlPKvm");
extern "C" ssize_t WrappedWrite(BNode*,const char*,type_code,off_t,const void*,size_t) asm("__wrap__ZN5BNode9WriteAttrEPKcjlPKvm");
extern "C" ssize_t WrappedWrite(BNode *n,const char *k,type_code t,off_t p,const void *b,size_t s){if(fault==1){fault=0;return RealWrite(n,k,t,p,b,s-1);}return RealWrite(n,k,t,p,b,s);}
extern "C" status_t RealSync(BNode*) asm("__real__ZN5BNode4SyncEv");
extern "C" status_t WrappedSync(BNode*) asm("__wrap__ZN5BNode4SyncEv");
extern "C" status_t WrappedSync(BNode *n){if(fault==2){fault=0;return B_IO_ERROR;}return RealSync(n);}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){struct stat st;
 if (fault == 3 || (fault == 4 && !fstat(fd, &st) && S_ISDIR(st.st_mode))) {
   fault=0;errno=EIO;return -1;
 }
 return __real_fsync(fd);}
extern "C" status_t RealAddString(BMessage *, const char *, const char *)
    asm("__real__ZN8BMessage9AddStringEPKcS1_");
extern "C" status_t WrappedAddString(BMessage *, const char *, const char *)
    asm("__wrap__ZN8BMessage9AddStringEPKcS1_");
extern "C" status_t WrappedAddString(BMessage *message, const char *key,
                                     const char *value) {
  if (fault == 5 && strcmp(key, "attribute") == 0) {
    fault = 0;
    return B_NO_MEMORY;
  }
  return RealAddString(message, key, value);
}
void Check(bool ok,const char *s){if(!ok)throw std::runtime_error(s);std::cout<<"PASS "<<s<<std::endl;++passed;}
FileAttribute Get(PropertySession &s, const char *name = "test:original"){for(auto a:s.Read().attributes)if(a.name==name)return a;throw std::runtime_error("missing");}
int main(){BApplication app("application/x-vnd.Kambuzi-PropertiesFaults");try{
 char p[]="/boot/home/listi-properties-faults-XXXXXX";int fd=mkstemp(p);if(fd<0)return 1;write(fd,"payload",7);close(fd);
 PropertySession session(p);
 for(int injected=1;injected<=4;++injected){
   BNode raw(p);raw.WriteAttr("test:original",B_STRING_TYPE,0,"original metadata",18);auto before=Get(session);fault=injected;
   std::string error;try{session.Save(&before,before.name,before.type,"new metadata");}catch(const std::exception &e){error=e.what();}
   Check(!error.empty() && fault==0,"injected metadata failure reported");
   auto pos=error.find("/boot/home/");Check(pos!=std::string::npos,"failure identifies retained recovery record");
   std::string recovery=error.substr(pos);BFile backup(recovery.c_str(),B_READ_ONLY);BMessage record;
   if(injected<3){Check(record.Unflatten(&backup)==B_OK,"recovery record readable after mutation failure");const void *data;ssize_t size;Check(record.FindData("bytes",B_RAW_TYPE,&data,&size)==B_OK && size==18 && memcmp(data,"original metadata",18)==0,"recovery retains exact original typed bytes");Check(record.GetInt32("type",0)==B_STRING_TYPE && record.GetBool("existed",false),"recovery retains original type and existence");}
   else Check(Get(session).bytes==before.bytes,"failed recovery flush prevents attribute mutation");
   unlink(recovery.c_str());
 }
 auto before = Get(session);
 fault = 5;
 std::string error;
 try {
   session.Save(&before, before.name, before.type, "not applied");
 } catch (const std::exception &e) {
   error = e.what();
 }
 Check(fault == 0 && error.find("Record attribute name") != std::string::npos &&
           Get(session).bytes == before.bytes,
       "a failed recovery message field prevents attribute mutation");
 constexpr const char *emptyName = "test:zero-length";
 BNode raw(p);
 const std::string large(150000, 'x');
 auto createEmpty = [&] {
   if (raw.WriteAttr(emptyName, B_STRING_TYPE, 0, large.data(), large.size()) !=
       ssize_t(large.size()))
     return false;
   const int fd = fs_open_attr(p, emptyName, B_STRING_TYPE,
                               O_WRONLY | O_TRUNC | O_CREAT);
   return fd >= 0 && fs_close_attr(fd) == 0;
 };
 Check(createEmpty(), "create a native zero-length string attribute");
 auto empty = Get(session, emptyName);
 Check(empty.editable && empty.size == 0 && empty.bytes.empty(),
       "zero-length string remains an editable existing value");
 fault = 2;
 error.clear();
 try {
   session.Remove(empty);
 } catch (const std::exception &e) {
   error = e.what();
 }
 auto position = error.find("/boot/home/");
 Check(fault == 0 && position != std::string::npos,
       "zero-length attribute removal retains a recovery record on sync failure");
 const auto recovery = error.substr(position);
 BFile backup(recovery.c_str(), B_READ_ONLY);
 BMessage record;
 Check(record.Unflatten(&backup) == B_OK &&
           record.GetBool("existed", false) &&
           record.GetInt32("type", 0) == B_STRING_TYPE &&
           record.GetInt64("size", -1) == 0 &&
           !record.HasData("bytes", B_RAW_TYPE),
       "recovery record represents an existing, typed, zero-byte value");
 unlink(recovery.c_str());
 Check(createEmpty(), "recreate the zero-length attribute");
 empty = Get(session, emptyName);
 session.Save(&empty, empty.name, empty.type, "restored text");
 const auto repaired = Get(session, emptyName);
 Check(repaired.text == "restored text", "edit an existing zero-length string");
 Check(createEmpty(), "recreate the zero-length attribute for removal");
 session.Remove(Get(session, emptyName));
 attr_info info;
 Check(raw.GetAttrInfo(emptyName, &info) == B_ENTRY_NOT_FOUND,
       "remove an existing zero-length string");
 std::ifstream f(p);std::string content((std::istreambuf_iterator<char>(f)),{});Check(content=="payload","all injected failures preserve file contents");unlink(p);
 std::cout<<"TOTAL "<<passed<<" passed"<<std::endl;return 0;
 }catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}}
