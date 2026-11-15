"""Read only offline measurements; no GUI, sockets, capture replay or vehicle access.

Different profiles share one executable/instrumentation. AU latency is reported
only for byte-verified FFmpeg-PTS associations, never decoded-output order.
"""
import argparse
import bisect
import csv
import json
import math
from pathlib import Path


def number(row, key):
    value = float(row[key])
    if not math.isfinite(value):
        raise ValueError('Non-finite ' + key)
    return value


def distribution(values):
    values = sorted(values)
    def percentile(p):
        if not values:
            return None
        index = (len(values) - 1) * p
        lo, hi = math.floor(index), math.ceil(index)
        return values[lo] + (values[hi] - values[lo]) * (index - lo)
    return dict(count=len(values), p50=percentile(.5), p95=percentile(.95),
                p99=percentile(.99), maximum=values[-1] if values else None)


def eligible_intervals(samples, frames=()):
    """Conservative, sample-confirmed intervals, never a backwards point mask.

    A sample's excluded flag describes that instant, not the preceding second.
    Both endpoints must therefore be normal and belong to the same replay loop.
    Explicitly excluded frame records also veto an interval whose endpoints
    missed a short transition. Missing source frames never veto an interval.
    """
    result, rejected = [], []
    frame_rows = sorted(((number(r, 'present_us') / 1e6, r) for r in frames), key=lambda item: item[0])
    frame_times = [time for time, _ in frame_rows]
    previous, previous_time = None, 0.0
    for row in samples:
        end = number(row, 'seconds')
        if end <= previous_time:
            raise ValueError('Sample time must increase')
        reason = None
        if previous is None:
            reason = 'no_previous_sample'
        elif previous_time < 2.0:
            reason = 'startup_before_two_seconds'
        elif previous['excluded'] != '0' or row['excluded'] != '0':
            reason = 'excluded_sample_endpoint'
        elif previous.get('source_loop') != row.get('source_loop'):
            reason = 'source_loop_changed'
        else:
            # Samples use a 1-based replay-loop count; frame origins are 0-based.
            expected_loop = int(row['source_loop']) - 1 if 'source_loop' in row else None
            left = bisect.bisect_left(frame_times, previous_time)
            right = bisect.bisect_right(frame_times, end)
            for _, frame in frame_rows[left:right]:
                if frame['excluded'] != '0':
                    reason = 'excluded_frame_inside_interval'
                    break
                if (expected_loop is not None and frame['associated'] == '1'
                        and int(frame['source_loop']) != expected_loop):
                    reason = 'frame_source_loop_mismatch'
                    break
        if reason:
            rejected.append(dict(begin=previous_time, end=end, reason=reason))
        else:
            loop = row.get('source_loop')
            if result and result[-1]['end'] == previous_time and result[-1]['source_loop'] == loop:
                result[-1]['end'] = end
            else:
                result.append(dict(begin=previous_time, end=end, source_loop=loop))
        previous, previous_time = row, end
    return result, rejected


def eligible_windows(samples, frames=()):
    intervals, _ = eligible_intervals(samples, frames)
    return [[item['begin'], item['end']] for item in intervals]


def inside(time, windows):
    return any(a <= time <= b for a, b in windows)


def update_gaps(times, windows, threshold_ms):
    # One physical no-new-frame gap can intersect two eligible windows. Clip its
    # measured duration to their disjoint union and count the underlying gap only
    # once; do not join excluded time into it or count overlapping tails twice.
    times = sorted(set(times))
    gaps = []
    for begin, end in windows:
        left, right = bisect.bisect_left(times, begin), bisect.bisect_right(times, end)
        selected = times[left:right]
        for offset, (a, b) in enumerate(zip([begin] + selected, selected + [end])):
            milliseconds = (b - a) * 1000
            if milliseconds > threshold_ms:
                gaps.append(dict(begin=a, end=b, gap_ms=milliseconds,
                    preceding_present_index=left + offset - 1,
                    following_present_index=left + offset))
    unique_gaps = {(g['preceding_present_index'], g['following_present_index']) for g in gaps}
    return dict(count=len(unique_gaps), eligible_segment_count=len(gaps), total_gap_ms=sum(g['gap_ms'] for g in gaps),
                excess_over_threshold_ms=sum(g['gap_ms'] - threshold_ms for g in gaps),
                maximum_ms=max((g['gap_ms'] for g in gaps), default=0), intervals=gaps)


def analyze(summary, frames, inputs, samples, threshold_ms=250):
    if not math.isfinite(threshold_ms) or threshold_ms <= 0:
        raise ValueError('Gap threshold must be positive and finite')
    intervals, rejected = eligible_intervals(samples, frames)
    windows = [[item['begin'], item['end']] for item in intervals]
    duration = sum(b-a for a, b in windows)
    observed_end = number(samples[-1], 'seconds') if samples else 0.0
    result = dict(profile=summary['performance_profile'], elapsed_seconds=summary['elapsed_seconds'],
        completed=summary['completed'], source_sha256=summary['raw_source_sha256'],
        source_window_seconds=summary['replay_window_seconds'],
        controlled_ui_work_ms=summary.get('controlled_ui_work_ms', 0), eligible_seconds=duration,
        excluded_seconds=observed_end-duration, observed_sample_seconds=observed_end,
        unassessed_tail_seconds=max(0.0, summary['elapsed_seconds']-observed_end),
        eligible_windows=windows, eligible_intervals=intervals,
        rejected_sample_intervals=rejected, lanes={})
    failures = []
    for lane in ('udp','cbb'):
        rows = [r for r in frames if r['lane'] == lane]
        presented = [r for r in rows if r['excluded'] == '0' and inside(number(r,'present_us')/1e6, windows)]
        known = [r for r in presented if r['associated'] == '1']
        timed = [r for r in known if any(a <= number(r,'first_ingress_us')/1e6
            <= number(r,'present_us')/1e6 <= b for a,b in windows)]
        times = [number(r,'present_us')/1e6 for r in rows]
        if any(b < a for a,b in zip(times,times[1:])):
            failures.append(lane + ': present time moved backwards')
        seen = set()
        for r in known:
            key = (int(r['source_loop']), int(r['token']))
            if key in seen:
                failures.append(lane + ': repeated decoded token counted as a new source frame')
            seen.add(key)
            stamps = [number(r,key) for key in ('first_ingress_us','complete_ingress_us','decode_submit_us','decoded_us','converted_us','upload_begin_us','upload_end_us','present_us')]
            if any(b < a for a,b in zip(stamps,stamps[1:])):
                failures.append(lane + ': frame chronology invalid')
        full_decoded = summary.get(lane + '_decoded')
        full_presented = summary.get(lane + '_new_presented')
        lane_index = 0 if lane == 'udp' else 1
        item = dict(new_presented=len(presented), associated_presented=len(known),
            unknown_presented=len(presented)-len(known),
            association_fraction=len(known)/len(presented) if presented else 0,
            latency_window_presented=len(timed), latency_boundary_excluded=len(known)-len(timed),
            latency_window_fraction=len(timed)/len(presented) if presented else 0,
            actual_new_fps=len(presented)/duration if duration else 0,
            ingress_to_present_ms=distribution([number(r,'ingress_to_present_ms') for r in timed]),
            update_gaps=update_gaps(times,windows,threshold_ms),
            latency_ready=bool(timed) and len(timed)/max(1,len(presented)) >= .95,
            full_run_decoded=full_decoded, full_run_new_presented=full_presented,
            full_run_decode_to_present_fraction=full_presented/full_decoded if full_decoded and full_presented is not None else None,
            full_run_unknown_decoded=summary.get('trace_unknown_decoded', [None,None])[lane_index],
            full_run_packet_dropped=summary.get('packet_dropped', [None,None])[lane_index],
            full_run_mailbox_overwritten=summary.get('mailbox_overwritten', [None,None])[lane_index])
        if lane == 'udp':
            item['rgba_total_us'] = distribution([number(r,'conversion_total_us') for r in presented])
            item['rgba_scale_us'] = distribution([number(r,'convert_us') for r in presented])
            item['rgba_copy_us'] = distribution([number(r,'copy_us') for r in presented])
            item['rgba_copied_bytes_presented'] = sum(int(r['copied_bytes']) for r in presented)
            item['rgba_allocations_presented'] = sum(int(r['conversion_allocation']) for r in presented)
            item['upload_us'] = distribution([number(r,'upload_us') for r in presented])
        result['lanes'][lane] = item
    eligible_inputs = [r for r in inputs if r['excluded']=='0' and r['matched']=='1'
        and any(a <= number(r,'event_enqueued_us')/1e6
                <= number(r,'sample_started_us')/1e6
                <= number(r,'sink_received_us')/1e6 <= b for a,b in windows)]
    result['input'] = dict(generated=summary.get('input_events_generated',0),
        scheduled=summary.get('input_events_scheduled',0),skipped=summary.get('input_events_skipped_no_catchup',0),
        sampling_placement=summary.get('input_sampling_placement','unspecified'),
        matched=summary.get('input_events_matched',0), mismatched=summary.get('input_events_mismatched',0),
        outside_or_excluded_rows=len(inputs)-len(eligible_inputs),
        eligible=len(eligible_inputs), event_to_local_sink_us=distribution([number(r,'event_to_sink_us') for r in eligible_inputs]),
        sampling_and_serialization_us=distribution([number(r,'sample_to_sink_us') for r in eligible_inputs]),
        definition='Synthetic ImGui event enqueue to real sampling/serialization/local protobuf parse; excludes MQTT and vehicle.')
    memory = [r for r in samples if number(r,'seconds')>=20]
    result['resources'] = dict(private_start=summary['private_bytes_start'],private_peak=summary['private_bytes_peak'],
        private_last=summary['private_bytes_last'], handles_start=summary['handles_start'], handles_last=summary['handles_last'],
        private_after_warmup_delta=int(memory[-1]['private_bytes'])-int(memory[0]['private_bytes']) if memory else None,
        backend_resources=summary['backend_resource_creations'],backend_recoveries=summary['backend_recoveries'])
    result['integrity_failures'] = failures
    result['definitions'] = dict(latency='All actual byte-verified AU data ingress -> successful Present return; not physical scanout or vehicle end-to-end.',
        eligibility='Adjacent sample endpoints both normal, same source loop, start >=2s, no explicitly excluded/mismatched-loop frame inside. No source-supply or latency-value filter. Latency additionally requires first ingress and Present in the same interval; boundary exclusions are counted. Final unsampled tail is disclosed separately.',
        gap='No new Present longer than threshold, clipped to eligible intervals. A gap crossing excluded intervals is counted once; durations sum disjoint eligible pieces only. Source holes remain gaps, not automatic exclusions.',
        gap_threshold_ms=threshold_ms, coverage='Only actual successful new-frame Present rows are measured; unknown associations excluded from latency, disclosed in coverage.',
        retention='Decode-to-Present counts, queue drops and mailbox overwrites cover the full run including excluded periods; association fraction alone does not prove frame retention.',
        input_eligibility='Event enqueue, sampling and local sink must all lie in the same eligible interval.',
        version='One instrumented executable with baseline/optimized runtime choices; not the historical 009 executable.')
    return result


def load_csv(path):
    with path.open(encoding='utf-8-sig',newline='') as stream:
        return list(csv.DictReader(stream))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs',nargs='+',type=Path)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--gap-ms',type=float,default=250)
    args = parser.parse_args()
    results = []
    for run in args.runs:
        summary = json.loads((run/'summary.json').read_text(encoding='utf-8-sig'))
        result = analyze(summary,load_csv(run/'frames.csv'),load_csv(run/'input.csv'),load_csv(run/'samples.csv'),args.gap_ms)
        result['run'] = str(run.resolve())
        results.append(result)
    keys = {(r['source_sha256'],r['source_window_seconds'],r['controlled_ui_work_ms']) for r in results}
    report = dict(comparable_source_and_load=len(keys)==1, runs=results)
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    for r in results:
        print(r['profile'], 'eligible_seconds=%.1f'%r['eligible_seconds'],
              'udp_rgba_p95_us=',r['lanes']['udp']['rgba_total_us']['p95'],
              'udp_association=',r['lanes']['udp']['association_fraction'],
              'cbb_association=',r['lanes']['cbb']['association_fraction'],
              'input_event_p95_us=',r['input']['event_to_local_sink_us']['p95'],
              'integrity_failures=',len(r['integrity_failures']))
    print('comparable_source_and_load=',report['comparable_source_and_load'])
    return 0 if len(keys)==1 and not any(r['integrity_failures'] for r in results) else 2


if __name__=='__main__':
    raise SystemExit(main())
