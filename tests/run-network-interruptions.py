# SPDX-License-Identifier: MIT
# Report directory contains ssh wrapper and network/share from the owned fixture.
from pathlib import Path
import os,shlex,subprocess,urllib.request,json,hashlib,sys,time
if len(sys.argv)<2:raise SystemExit('Usage: run-network-interruptions.py REPORT [smb ftp sftp]')
r=Path(sys.argv[1]).resolve();share=r/'network/share'
protocols=sys.argv[2:] or ['smb','ftp','sftp']
if any(protocol not in ('smb','ftp','sftp') for protocol in protocols):
 raise SystemExit('Unknown network protocol')
guest_binary=os.environ.get('LISTI_NETWORK_TEST_BINARY','/boot/home/listi-024-final/build/native-audit-tests/network-interruptions')
guest_known_hosts=os.environ.get('LISTI_NETWORK_KNOWN_HOSTS','/boot/home/listi-network-known-hosts')
results=[]
for protocol in protocols:
 for mode in ['none','corrupt-stage','download','upload','publish-before','backup-reply','publish-reply','cleanup','cancel-download']:
  if os.getenv('NET_CASES') and protocol+':'+mode not in os.environ['NET_CASES'].split():continue
  urllib.request.urlopen('http://127.0.0.1:23990/reset/'+('none' if mode=='cancel-download' else mode)).read()
  cmd=shlex.join([guest_binary,protocol,mode,guest_known_hosts])
  start=time.monotonic()
  try:p=subprocess.run([str(r/'ssh'),cmd],text=True,capture_output=True,timeout=80)
  except subprocess.TimeoutExpired as e:
   (r/(protocol+'-'+mode+os.getenv('NET_RUN','')+'.log')).write_text(str(e));raise
  (r/(protocol+'-'+mode+os.getenv('NET_RUN','')+'.log')).write_text(p.stdout+p.stderr)
  state=json.load(urllib.request.urlopen('http://127.0.0.1:23990/'))
  files={x.name:{'size':x.stat().st_size,'sha256':hashlib.sha256(x.read_bytes()).hexdigest()} for x in share.iterdir() if x.is_file()}
  wanted=b'n'*2097152 if mode in ('none','publish-reply','cleanup') else b'old contents'
  actual=(share/'input').read_bytes() if (share/'input').exists() else None
  previous=[x.read_bytes() for x in share.glob('*.previous')]
  ok=p.returncode==0 and (mode in ('none','cancel-download') or state['triggered']) and actual==wanted
  if mode=='corrupt-stage':ok=ok and set(files)=={'input','hello','download'} and 'Uploaded content differs' in p.stdout
  if mode in ('publish-reply','cleanup'):ok=ok and previous==[b'old contents'] and '.previous' in p.stdout
  result=dict(protocol=protocol,mode=mode,ok=ok,exit=p.returncode,fixture=state,files=files,seconds=round(time.monotonic()-start,3));results.append(result)
  (r/('network-results'+os.getenv('NET_RUN','')+'.json')).write_text(json.dumps(results,indent=2))
  print(('PASS ' if ok else 'FAIL ')+protocol+' '+mode+' '+str(result['seconds'])+'s',flush=True)
  if not ok:print(p.stdout+p.stderr,flush=True)
print(sum(x['ok'] for x in results),'/',len(results),'real protocol cases passed',flush=True)
sys.exit(0 if all(x['ok'] for x in results) else 1)
