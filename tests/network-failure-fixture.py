# SPDX-License-Identifier: MIT
# Isolated host fixture, never point at production. Requires impacket, pyftpdlib, paramiko.
import os, pathlib, threading, socket, time, json, logging
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from impacket import smbserver, smb3structs as smb2
from impacket.ntlm import compute_lmhash, compute_nthash
from pyftpdlib.handlers import FTPHandler, DTPHandler
from pyftpdlib.servers import FTPServer
from pyftpdlib.authorizers import DummyAuthorizer
import paramiko
import argparse
parser=argparse.ArgumentParser();parser.add_argument('--directory',required=True);args=parser.parse_args()
base=pathlib.Path(args.directory).resolve();base.mkdir(exist_ok=True)
root=base/'share';root.mkdir(exist_ok=True)
state={'mode':'none','triggered':False};lock=threading.Lock()
def fire(mode):
 with lock:
  if state['mode']==mode and not state['triggered']:
   state['triggered']=True;print('INJECT',mode,flush=True);return True
 return False
def corrupt_stage():
 if state['mode']!='corrupt-stage':return
 parts=list(root.glob('*.part'))
 if len(parts)!=1 or not parts[0].stat().st_size:return
 if fire('corrupt-stage'):
  with parts[0].open('r+b') as stream:
   first=stream.read(1);stream.seek(0);stream.write(bytes([first[0]^1]))
def cut(sock):
 try:sock.shutdown(socket.SHUT_RDWR)
 except OSError:pass
 sock.close()
def reset(mode):
 for p in root.iterdir():
  if p.is_file():p.unlink()
 (root/'input').write_bytes(b'old contents')
 (root/'download').write_bytes(b'd'*2097152)
 (root/'hello').write_bytes(b'hello')
 with lock:state.update(mode=mode,triggered=False)
reset('none')
class Control(BaseHTTPRequestHandler):
 def do_GET(self):
  if self.path.startswith('/reset/'):
   reset(self.path.split('/')[-1])
  data=json.dumps(state).encode();self.send_response(200);self.end_headers();self.wfile.write(data)
 def log_message(self,*a):pass
threading.Thread(target=ThreadingHTTPServer(('127.0.0.1',23990),Control).serve_forever,daemon=True).start()
# SMB: close the real socket at a protocol boundary, including after a rename
# has changed server state but before its success response can be delivered.
sockets={}
class SMBHandler(smbserver.SMBSERVERHandler):
 def handle(self):
  sockets[threading.current_thread().name]=self.request
  try:super().handle()
  finally:sockets.pop(threading.current_thread().name,None)
class SMBServer(smbserver.SMBSERVER):
 def __init__(self,address,**kwargs):super().__init__(address,handler_class=SMBHandler,**kwargs)
user='listitest';password='fixture-only'
smb=smbserver.SimpleSMBServer(listenAddress='127.0.0.1',listenPort=1445,smbserverclass=SMBServer)
smb.addShare('files',str(root));smb.setSMB2Support(True);smb.addCredential(user,0,compute_lmhash(password),compute_nthash(password));smb.setLogFile(str(base/'smb.log'))
def disconnect_smb():
 cut(sockets[threading.current_thread().name]);raise ConnectionError('task-owned injected transport break')
server=smb.getServer()
def hook(command,fn):
 old=server.hookSmb2Command(command,lambda cid,srv,packet:fn(old,cid,srv,packet))
def io_hook(kind):
 def run(old,cid,srv,packet):
  request=(smb2.SMB2Read if kind=='download' else smb2.SMB2Write)(packet['Data'])
  if kind=='download':corrupt_stage()
  result=old(cid,srv,packet)
  if request['Offset']>=65536 and fire(kind):disconnect_smb()
  return result
 return run
hook(smb2.SMB2_READ,io_hook('download'));hook(smb2.SMB2_WRITE,io_hook('upload'))
def setinfo(old,cid,srv,packet):
 req=smb2.SMB2SetInfo(packet['Data']);conn=srv.getConnectionData(cid)
 fid=req['FileID'].getData()
 if fid==b'\xff'*16:fid=conn.get('LastRequest',{}).get('SMB2_CREATE',{}).get('FileID',fid)
 source=str(conn.get('OpenedFiles',{}).get(fid,{}).get('FileName',''))
 kind=req['FileInfoClass'];destination=''
 if kind==smb2.SMB2_FILE_RENAME_INFO:
  destination=smb2.FILE_RENAME_INFORMATION_TYPE_2(req['Buffer'])['FileName'].decode('utf-16le')
  if source.endswith('.part') and fire('publish-before'):disconnect_smb()
 if kind==smb2.SMB2_FILE_DISPOSITION_INFO and source.endswith('.previous') and fire('cleanup'):disconnect_smb()
 result=old(cid,srv,packet)
 if kind==smb2.SMB2_FILE_RENAME_INFO:
  if destination.endswith('.previous') and fire('backup-reply'):disconnect_smb()
  if source.endswith('.part') and fire('publish-reply'):disconnect_smb()
 return result
hook(smb2.SMB2_SET_INFO,setinfo)
def create_hook(old,cid,srv,packet):
 req=smb2.SMB2Create(packet['Data'])
 name=req['Buffer'][:req['NameLength']].decode('utf-16le')
 if name.endswith('.previous') and req['CreateOptions'] & 0x1000 and fire('cleanup'):disconnect_smb()
 return old(cid,srv,packet)
hook(smb2.SMB2_CREATE,create_hook)
threading.Thread(target=smb.start,daemon=True).start()
# FTP control reply loss and data-channel interruption.
auth=DummyAuthorizer();auth.add_user(user,password,str(root),perm='elradfmwMT')
class DTP(DTPHandler):
 def handle_read(self):
  super().handle_read()
  if self.tot_bytes_received>=65536 and fire('upload'):
   self.cmd_channel.close();self.close()
 handle_read_event=handle_read
 def send(self,data):
  result=super().send(data)
  if self.tot_bytes_sent>=65536 and fire('download'):
   self.cmd_channel.close();self.close()
  return result
class FTP(FTPHandler):
 dtp_handler=DTP;use_sendfile=False
 def ftp_RETR(self,path):
  if path.endswith('.part'):corrupt_stage()
  return super().ftp_RETR(path)
 def ftp_RNTO(self,path):
  source=self._rnfr or ''
  if source.endswith('.part') and fire('publish-before'):self.close();return
  if (path.endswith('.previous') and state['mode']=='backup-reply') or (source.endswith('.part') and state['mode']=='publish-reply'):
   mode=state['mode']
   if fire(mode):
    self._rnfr=None;self.run_as_current_user(self.fs.rename,source,path);self.close();return
  return super().ftp_RNTO(path)
 def ftp_DELE(self,path):
  if path.endswith('.previous') and fire('cleanup'):self.close();return
  return super().ftp_DELE(path)
FTP.authorizer=auth;FTP.passive_ports=range(31000,31020)
ftp=FTPServer(('127.0.0.1',2121),FTP);threading.Thread(target=ftp.serve_forever,daemon=True).start()
keypath=base/'sftp-key'
if keypath.exists():key=paramiko.RSAKey.from_private_key_file(str(keypath))
else:key=paramiko.RSAKey.generate(2048);key.write_private_key_file(str(keypath));os.chmod(keypath,0o600)
(base/'known_hosts').write_text('[127.0.0.1]:2222 '+key.get_name()+' '+key.get_base64()+'\n')
class Auth(paramiko.ServerInterface):
 def check_auth_password(self,u,p):return paramiko.AUTH_SUCCESSFUL if u==user and p==password else paramiko.AUTH_FAILED
 def get_allowed_auths(self,u):return 'password'
 def check_channel_request(self,kind,id):return paramiko.OPEN_SUCCEEDED if kind=='session' else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED
class SFTP(paramiko.SFTPServerInterface):
 def __init__(self,server,*args,**kwargs):super().__init__(server,*args,**kwargs);self.transport=server.transport
 def local(self,path):
  p=(root/path.lstrip('/')).resolve()
  if p!=root and root not in p.parents:raise OSError(13,'outside fixture')
  return str(p)
 def list_folder(self,path):
  try:
   out=[]
   for e in os.scandir(self.local(path)):
    a=paramiko.SFTPAttributes.from_stat(e.stat(follow_symlinks=False));a.filename=e.name;out.append(a)
   return out
  except OSError as e:return paramiko.SFTPServer.convert_errno(e.errno)
 def stat(self,path):
  try:return paramiko.SFTPAttributes.from_stat(os.stat(self.local(path)))
  except OSError as e:return paramiko.SFTPServer.convert_errno(e.errno)
 lstat=stat
 def open(self,path,flags,attr):
  try:
   fd=os.open(self.local(path),flags,0o600);f=os.fdopen(fd,'r+b' if flags&os.O_RDWR else 'wb' if flags&os.O_WRONLY else 'rb');h=paramiko.SFTPHandle(flags);h.readfile=f;h.writefile=f
   read=h.read;write=h.write
   def interrupted_read(offset,length):
    if path.endswith('.part'):corrupt_stage()
    if offset>=65536 and fire('download'):self.transport.close();return paramiko.SFTP_FAILURE
    return read(offset,min(length,65536))
   def interrupted_write(offset,data):
    result=write(offset,data)
    if offset>=65536 and fire('upload'):self.transport.close();return paramiko.SFTP_FAILURE
    return result
   h.read=interrupted_read;h.write=interrupted_write;return h
  except OSError as e:return paramiko.SFTPServer.convert_errno(e.errno)
 def action(self,fn,*args):
  try:fn(*[self.local(x) for x in args]);return paramiko.SFTP_OK
  except OSError as e:return paramiko.SFTPServer.convert_errno(e.errno)
 def mkdir(self,path,attr):return self.action(os.mkdir,path)
 def remove(self,path):
  if path.endswith('.previous') and fire('cleanup'):self.transport.close();return paramiko.SFTP_FAILURE
  return self.action(os.unlink,path)
 def rmdir(self,path):return self.action(os.rmdir,path)
 def rename(self,a,b):
  if a.endswith('.part') and fire('publish-before'):self.transport.close();return paramiko.SFTP_FAILURE
  result=self.action(os.rename,a,b)
  if result==paramiko.SFTP_OK and ((b.endswith('.previous') and fire('backup-reply')) or (a.endswith('.part') and fire('publish-reply'))):self.transport.close();return paramiko.SFTP_FAILURE
  return result
 def chattr(self,path,attr):return paramiko.SFTP_OK
listener=socket.socket();listener.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);listener.bind(('127.0.0.1',2222));listener.listen(10)
def client(sock):
 try:
  t=paramiko.Transport(sock);t.add_server_key(key);t.set_subsystem_handler('sftp',paramiko.SFTPServer,SFTP);auth=Auth();auth.transport=t;t.start_server(server=auth)
  while t.is_active():time.sleep(.1)
 except Exception:logging.exception('fixture client')
 finally:sock.close()
print('READY task-owned SMB/FTP/SFTP and control',flush=True)
while True:
 sock,_=listener.accept();threading.Thread(target=client,args=(sock,),daemon=True).start()
