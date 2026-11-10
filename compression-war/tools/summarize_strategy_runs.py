"""Aggregate acceptance without hiding failed repeats. Reads completed evidence only."""
from pathlib import Path
import argparse,json,csv,statistics
from strategy_compare import settings,rows
p=argparse.ArgumentParser();p.add_argument('root',type=Path);a=p.parse_args();items=[]
for d in sorted(a.root.glob('*')):
    if not (d/'summary.txt').exists():continue
    s=settings(d/'summary.txt');tx=settings(d/'wire-verification.txt');rx=settings(d/'rx-replay-verification.txt')
    frames=rows(d/'display.csv');gaps=[float(b['present_ms'])-float(a['present_ms']) for a,b in zip(frames,frames[1:])]
    items.append(dict(run=d.name,profile=settings(d/'strategy.ini')['name'],input=int(s['source_frames']),decoded=int(s['decoded']),displayed=int(s['displayed_or_headless_consumed']),
        min_fps=int(s['minimum_stable_display_fps']),windows=int(s['stable_display_windows']),failed_windows=int(s['display_windows_below_15']),acceptance=s['display_acceptance'],
        median_ms=float(s['latency_median_ms']),p95_ms=float(s['latency_p95_ms']),max_display_gap_ms=max(gaps,default=0),
        max_bytes=int(s['max_sent_frame_h264_bytes']),packets=int(s['packets_sent']),oversize=int(s['oversize']),
        min_gap_ms=tx.get('min_packet_gap_ms'),max_window_packets=tx.get('max_packets_rolling_second'),
        max_send_queue=int(s['max_send_queue_frames']),max_rx_queue=int(s['max_rx_queue_packets']),max_trace_records=int(s['max_trace_records']),
        wire=tx.get('pixel_sha256_match'),reconstruction=rx.get('processed_sha256_match')))
(a.root/'acceptance.json').write_text(json.dumps(items,ensure_ascii=False,indent=2),encoding='utf-8')
if items:
    with (a.root/'acceptance.csv').open('w',encoding='utf-8-sig',newline='') as f:
        w=csv.DictWriter(f,fieldnames=items[0]);w.writeheader();w.writerows(items)
for r in items:print(f"{r['run']}: {r['acceptance']} min={r['min_fps']} fps P50/P95={r['median_ms']}/{r['p95_ms']}ms max_gap={r['max_display_gap_ms']:.1f}ms wire/post={r['wire']}/{r['reconstruction']}")
