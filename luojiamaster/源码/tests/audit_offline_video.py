"""Read-only CSV/JSON auditor; no GUI, networking, device or executable access.

The harness reports completion only. This independent auditor rejects late
freezes despite nonzero lifetime frame totals. Thresholds are explicit in its
result and can be changed by the reviewer rather than hidden in the app.
"""
import argparse
import csv
import json
import math
from pathlib import Path
import statistics


def finite(row, field):
    value = float(row[field])
    if not math.isfinite(value):
        raise ValueError('Non-finite field ' + field)
    return value


def slope_per_minute(points):
    if len(points) < 2:
        return None
    mx = statistics.fmean(t for t, value in points)
    my = statistics.fmean(value for t, value in points)
    denominator = sum((t - mx) ** 2 for t, value in points)
    if denominator == 0:
        return 0.0
    return sum((t - mx) * (value - my) for t, value in points) / denominator * 60


def audit(summary, rows, limits):
    failures = []
    eligible, excluded = [], []
    previous = None
    for index, row in enumerate(rows):
        t = finite(row, 'seconds')
        if previous and t <= finite(previous, 'seconds'):
            failures.append(dict(row=index, reason='sample time did not advance'))
        for key, cap in (('udp_packet_queue', 256), ('cbb_packet_queue', 256),
                         ('udp_mailbox_pending', 1), ('cbb_mailbox_pending', 1),
                         ('backend_deferred', limits['max_deferred'])):
            value = finite(row, key)
            if not 0 <= value <= cap:
                failures.append(dict(row=index, reason='queue/deferred bound', field=key, actual=value, maximum=cap))
        if previous:
            dt = t - finite(previous, 'seconds')
            for field in ('udp_presented', 'cbb_presented', 'successful_presents', 'udp_decoded', 'cbb_decoded'):
                if finite(row, field) < finite(previous, field):
                    failures.append(dict(row=index, reason='cumulative counter decreased', field=field))
            # Exclusions are explicit telemetry, not inferred from bad output.
            if row.get('excluded', '0') in ('1', 'true', 'True'):
                excluded.append(dict(seconds=t, reason=row.get('exclusion_reason', 'unspecified')))
            elif dt > 0:
                item = dict(seconds=t, udp_display_fps=(finite(row, 'udp_presented')-finite(previous, 'udp_presented'))/dt,
                            cbb_display_fps=(finite(row, 'cbb_presented')-finite(previous, 'cbb_presented'))/dt,
                            udp_age_ms=finite(row, 'udp_age_ms'), cbb_age_ms=finite(row, 'cbb_age_ms'))
                eligible.append(item)
                if 'minimap_ready' in row and finite(row, 'minimap_ready') != 1:
                    failures.append(dict(row=index, seconds=t, reason='embedded production map not ready during visible steady interval'))
                for lane in ('udp', 'cbb'):
                    if item[lane + '_display_fps'] < limits['min_' + lane + '_fps']:
                        failures.append(dict(row=index, seconds=t, reason='visible steady interval froze or updated too slowly', lane=lane, fps=item[lane + '_display_fps']))
                    if not 0 <= item[lane + '_age_ms'] <= limits['max_age_ms']:
                        failures.append(dict(row=index, seconds=t, reason='visible decoded-frame age', lane=lane, age_ms=item[lane + '_age_ms']))
        previous = row
    if not summary.get('completed'):
        failures.append(dict(reason='run did not complete requested duration'))
    if not summary.get('dual_stream_smoke_ok'):
        failures.append(dict(reason='both real decoded lanes were not presented'))
    if summary.get('worker_error'):
        failures.append(dict(reason='worker error', detail=summary['worker_error']))
    if summary.get('minimap_required') and (not summary.get('minimap_ready') or not summary.get('minimap_presented')):
        failures.append(dict(reason='embedded production map was not actually presented and ready at completion'))
    if summary.get('elapsed_seconds', 0) < limits['minimum_seconds']:
        failures.append(dict(reason='not long enough for this acceptance profile'))
    if summary.get('fault_stage') and not summary.get('fault_recovered'):
        failures.append(dict(reason='injected failure was not observed, cleared and recovered'))
    if len(eligible) < limits['minimum_eligible_intervals']:
        failures.append(dict(reason='insufficient visible steady intervals', count=len(eligible)))
    if rows and len(eligible) / len(rows) < limits['minimum_eligible_fraction']:
        failures.append(dict(reason='too much of the run was excluded', fraction=len(eligible)/len(rows)))
    memory_rows = [r for r in rows if finite(r, 'seconds') >= limits['memory_warmup_seconds']]
    memory = dict(evaluated=False)
    if len(memory_rows) >= 10:
        quarter = max(1, len(memory_rows)//4)
        first, last = memory_rows[:quarter], memory_rows[-quarter:]
        private_delta = statistics.fmean(finite(r, 'private_bytes')for r in last)-statistics.fmean(finite(r, 'private_bytes')for r in first)
        handle_delta = statistics.fmean(finite(r, 'handles')for r in last)-statistics.fmean(finite(r, 'handles')for r in first)
        tail = memory_rows[len(memory_rows)//2:]
        trend = slope_per_minute([(finite(r, 'seconds'), finite(r, 'private_bytes')/1048576)for r in tail])
        resources = finite(last[-1], 'backend_resources')-finite(first[0], 'backend_resources')
        presents = finite(last[-1], 'successful_presents')-finite(first[0], 'successful_presents')
        resource_ratio = resources/max(1, presents)
        memory = dict(evaluated=True, private_growth_mib=private_delta/1048576, handle_growth=handle_delta,
                      last_half_private_slope_mib_per_minute=trend, resources_per_present=resource_ratio)
        if private_delta/1048576 > limits['max_private_growth_mib']:
            failures.append(dict(reason='private memory growth', actual_mib=private_delta/1048576))
        if trend is not None and trend > limits['max_private_slope_mib_per_minute']:
            failures.append(dict(reason='private memory keeps growing in final half', actual_mib_per_minute=trend))
        if handle_delta > limits['max_handle_growth']:
            failures.append(dict(reason='handle growth', actual=handle_delta))
        if resources > 64 and resource_ratio > limits['max_resources_per_present']:
            failures.append(dict(reason='backend keeps creating resources per frame', actual_ratio=resource_ratio))
    else:
        failures.append(dict(reason='not enough post-warmup samples for memory/handle trend'))
    return dict(acceptance_passed=not failures, thresholds=limits, total_samples=len(rows),
                eligible_intervals=len(eligible), excluded_intervals=excluded, failures=failures,
                visible_steady=dict(udp_min_fps=min((r['udp_display_fps']for r in eligible),default=None),
                                    cbb_min_fps=min((r['cbb_display_fps']for r in eligible),default=None),
                                    udp_mean_fps=statistics.fmean(r['udp_display_fps']for r in eligible)if eligible else None,
                                    cbb_mean_fps=statistics.fmean(r['cbb_display_fps']for r in eligible)if eligible else None),
                memory=memory,
                scope='Recorded real encoded data + actual decoders + GPU uploads/presents. No camera end-to-end latency measurement.')


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report_dir',type=Path)
    parser.add_argument('--out',type=Path)
    parser.add_argument('--minimum-seconds',type=float,default=60)
    parser.add_argument('--min-udp-fps',type=float,default=1)
    parser.add_argument('--min-cbb-fps',type=float,default=1)
    parser.add_argument('--max-age-ms',type=float,default=2000)
    parser.add_argument('--memory-warmup-seconds',type=float,default=20)
    parser.add_argument('--max-private-growth-mib',type=float,default=128)
    parser.add_argument('--max-private-slope-mib-per-minute',type=float,default=4)
    parser.add_argument('--max-handle-growth',type=float,default=32)
    parser.add_argument('--max-resources-per-present',type=float,default=.02)
    parser.add_argument('--minimum-eligible-fraction',type=float,default=.5)
    args=parser.parse_args()
    limits={key:value for key,value in vars(args).items()if key not in ('report_dir','out')}
    limits.update(max_deferred=64,minimum_eligible_intervals=5)
    summary=json.loads((args.report_dir/'summary.json').read_text(encoding='utf-8-sig'))
    with (args.report_dir/'samples.csv').open(encoding='utf-8-sig',newline='')as source:rows=list(csv.DictReader(source))
    result=audit(summary,rows,limits)
    output=args.out or args.report_dir/'acceptance.json'
    output.write_text(json.dumps(result,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print('ACCEPTANCE '+('PASSED'if result['acceptance_passed']else'FAILED')+' eligible='+str(result['eligible_intervals'])+' failures='+str(len(result['failures'])))
    for failure in result['failures'][:8]:print(json.dumps(failure,ensure_ascii=True))
    return 0 if result['acceptance_passed']else 2


if __name__=='__main__':raise SystemExit(main())
