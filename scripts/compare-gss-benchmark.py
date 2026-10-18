"""Compare paired read-only navigation runs. Panel scan rate is not UI FPS."""
import json,re,hashlib
from pathlib import Path

root=Path(__file__).resolve().parents[1]
work=root/'work/release-0.7.2'
result={'version':'0.7.2-gss','scenario':'WIN/NET/HOME/GAME open/close, three cycles, 24 transitions, 800ms spacing',
        'baseline':'same 0.7.2 sources; unchanged-label, visibility and interaction optimizations disabled',
        'optimized':'changed labels, visible widgets, interaction-priority telemetry updates',
        'limitations':['Live GSN snapshots, not frozen input; one paired run with three cycles.',
                      'Programmatic navigation to first completed flush, not physical touch latency.',
                      'Active-window FPS counts completed frames within 350ms of each event.',
                      'Full-workflow FPS includes idle gaps; panel scan timing unchanged.']}
for name in ('baseline','optimized'):
    log=(work/name/'runtime.log').read_text(encoding='utf-8',errors='replace')
    metrics={}
    for tag in ('BENCH','BENCH2'):
        row=re.search(r'\b'+tag+r' (.+)',log)
        if not row:raise SystemExit(f'Missing {tag}: {name}')
        metrics.update({key:int(value) for key,value in re.findall(r'(\w+)=(\d+)',row.group(1))})
    assert metrics['event_count']==24
    metrics['active_window_fps']=round(metrics['active_frames']*1000/metrics['active_window_ms'],3)
    metrics['workflow_fps']=round(metrics['frames']*1000/metrics['elapsed_ms'],3)
    metrics['processing_throughput_fps']=round(1000000/metrics['total_avg_us'],3)
    metrics['binary_sha256']=hashlib.sha256((work/(name+'.bin')).read_bytes()).hexdigest()
    result[name+'_metrics']=metrics
b=result['baseline_metrics'];o=result['optimized_metrics']
result['changes_percent']={key:round((o[key]/b[key]-1)*100,2) for key in ('active_window_fps','workflow_fps','processing_throughput_fps','event_avg_us','p95_us','render_avg_us','total_avg_us')}
out=root/'artifacts/verification/performance-0.7.2.json'
out.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result,ensure_ascii=False,indent=2))
