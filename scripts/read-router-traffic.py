"""Read the BX54 mobile status page; never change router settings or reuse browser cookies."""
import argparse,json,re,time,urllib.request,urllib.error,datetime

def read_traffic(base='http://192.168.124.1',timeout=2.0):
    url=base.rstrip('/')+'/mobile.asp'
    req=urllib.request.Request(url,headers={'User-Agent':'GSO-STATUS-traffic-reader/0.1','Cache-Control':'no-cache'})
    opener=urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(req,timeout=timeout) as response:
        raw=response.read(600000)
        if len(raw)>=600000:raise ValueError('Unexpectedly large status page')
        page=raw.decode('gb18030',errors='replace')
        match=re.search(r'\bvar\s+wan_rate\s*=\s*[\"\x27](\d+);(\d+)[\"\x27]',page)
        if not match:raise ValueError('WAN rate field missing; login expired or firmware format changed')
        return {'source':'H3C BX54 WAN','observed_at':datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=8))).isoformat(),'upload_Bps':int(match[1]),'download_Bps':int(match[2]),'quality':'ok','transport':'read-only HTTP status page','source_updated_at':None}

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--samples',type=int,default=1);args=parser.parse_args()
    for i in range(max(1,args.samples)):
        start=time.monotonic()
        try:result=read_traffic()
        except Exception as ex:result={'source':'H3C BX54 WAN','quality':'unavailable','error':str(ex),'upload_Bps':None,'download_Bps':None}
        print(json.dumps(result,ensure_ascii=True),flush=True)
        if i+1<args.samples:time.sleep(max(0,1-(time.monotonic()-start)))
