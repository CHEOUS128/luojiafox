"""Read-only one-hour recorded-video audit; no executable, GPU or network access.

The result evaluates the declared offline integrity/resource checks. Frame rate
and no-new-image intervals are observations, not an invented performance gate.
No source gap or slow output is automatically removed from the valid windows.
Only the explicitly requested JSON report is written; source records stay intact.
"""
import argparse
import bisect
import hashlib
import json
import math
from pathlib import Path
import statistics

from analyze_video_performance import analyze, load_csv, number, update_gaps


MIB = 1048576
DEFAULT_LIMITS = dict(minimum_seconds=3600.0, memory_warmup_seconds=60.0,
    minimum_eligible_fraction=.75, maximum_unsampled_tail_seconds=2.0,
    maximum_sample_gap_seconds=5.0, minimum_association_fraction=.95,
    max_private_growth_mib=128.0, max_private_slope_mib_per_minute=4.0,
    max_handle_growth=32.0, max_resource_growth=0,
    packet_queue_capacity=256, mailbox_capacity=1, max_deferred=64,
    gap_threshold_ms=250.0,
    # Fixed before the 2026-11-16 run. These measure probe coverage, not vehicle
    # responsiveness. Video source-loop exclusions never stop the input clock.
    minimum_input_slot_coverage=.95, maximum_input_event_gap_seconds=2.0)


def eligible_fraction_argument(value):
    fraction=float(value)
    if not math.isfinite(fraction) or not 0<fraction<=1:
        raise argparse.ArgumentTypeError('minimum eligible fraction must be finite and 0 < value <= 1')
    return fraction


def linear_slope_per_minute(points):
    if len(points) < 2:
        return None
    mean_t = statistics.fmean(t for t, _ in points)
    mean_y = statistics.fmean(y for _, y in points)
    denominator = sum((t - mean_t) ** 2 for t, _ in points)
    if denominator <= 0:
        return None
    return sum((t - mean_t) * (y - mean_y) for t, y in points) / denominator * 60


def time_weighted_mean(points, begin, end):
    """Piecewise-linear mean in an actual time quarter, not a row-count quarter."""
    area = 0.0
    covered = 0.0
    for (t0, value0), (t1, value1) in zip(points, points[1:]):
        left, right = max(begin, t0), min(end, t1)
        if right <= left:
            continue
        value_left = value0 + (value1-value0)*(left-t0)/(t1-t0)
        value_right = value0 + (value1-value0)*(right-t0)/(t1-t0)
        area += (value_left+value_right)/2*(right-left)
        covered += right-left
    return area/covered if covered else None


def window_gaps(sorted_times, begin, end, threshold_ms):
    # Neighbor context preserves the physical gap at both edges without sorting
    # hundreds of thousands of full-run timestamps again for every sample.
    left=bisect.bisect_left(sorted_times,begin)
    right=bisect.bisect_right(sorted_times,end)
    context=sorted_times[max(0,left-1):min(len(sorted_times),right+1)]
    return update_gaps(context,[[begin,end]],threshold_ms)


def input_coverage_windows(elapsed, events):
    """Only planned window/fault exercises may leave the input coverage clock.

    In particular source-loop recovery and externally lost focus do not create
    an exclusion. Lost events remain losses even during a declared exercise.
    """
    exclusions = []
    minimized = fault = None
    for row in events:
        kind = row.get('event')
        if kind not in ('window_action', 'fault_begin', 'fault_clear'):
            continue
        at = number(row, 'seconds')
        if kind == 'window_action':
            action = int(row['detail'])
            exclusions.append((at, at+2.0))
            if action == 2:
                if minimized is None:
                    minimized = at
            elif minimized is not None:
                exclusions.append((minimized, at+2.0))
                minimized = None
        elif kind == 'fault_begin':
            fault = at
        elif fault is not None:
            exclusions.append((fault, at+2.0))
            fault = None
    if minimized is not None:
        exclusions.append((minimized, elapsed))
    if fault is not None:
        exclusions.append((fault, elapsed))
    windows = [[2.0, elapsed]] if elapsed > 2.0 else []
    for left, right in sorted(exclusions):
        reduced = []
        for begin, end in windows:
            if right <= begin or left >= end:
                reduced.append([begin, end])
            else:
                if left > begin:
                    reduced.append([begin, left])
                if right < end:
                    reduced.append([right, end])
        windows = reduced
    return windows


def audit_local_input_v2(summary, inputs, elapsed, events=(), limits=None):
    """Every generated event terminates once; lost is never a latency sample."""
    limits = dict(DEFAULT_LIMITS, **(limits or {}))
    checks = []
    def check(name, passed, actual, limit):
        checks.append(dict(check=name, passed=bool(passed), actual=actual, limit=limit))
    fields = ('input_events_generated', 'input_events_matched', 'input_events_lost',
              'input_events_mismatched', 'input_events_scheduled', 'input_events_skipped_no_catchup')
    recorded = all(type(summary.get(field)) is int and summary[field] >= 0 for field in fields)
    check('local_input_counters_recorded', recorded,
          {field:summary.get(field) for field in fields}, 'explicit nonnegative integers')
    generated, matched, lost, mismatched, scheduled, skipped = (summary[field] if type(summary.get(field)) is int else 0 for field in fields)
    check('local_input_event_accounting', recorded and generated > 0 and generated == matched+lost,
          dict(generated=generated, matched=matched, lost=lost), 'generated = matched + lost > 0')
    check('all_generated_local_input_matched', recorded and generated > 0 and matched == generated and lost == 0 and mismatched == 0,
          dict(generated=generated, matched=matched, lost=lost, mismatched=mismatched),
          'normal profile: generated=matched>0; lost=0; mismatched=0')
    check('no_pending_input_before_finish', summary.get('input_events_pending_before_finish') is False,
          summary.get('input_events_pending_before_finish'), False)
    check('no_pending_input_after_finish', summary.get('input_events_pending_at_stop') is False,
          summary.get('input_events_pending_at_stop'), False)
    check('local_input_csv_count_matches', len(inputs) == generated,
          dict(csv=len(inputs), generated=generated), 'equal')
    bad, event_times = [], []
    csv_matched = csv_lost = 0
    loss_reasons = {}
    previous_enqueued = -1.0
    for index, row in enumerate(inputs):
        try:
            enqueued = number(row, 'event_enqueued_us')
            resolved = number(row, 'resolved_us')
            event_times.append(enqueued/1e6)
            valid = (int(row['event_id']) == index+1 and row['key_down'] in ('0','1')
                     and row['excluded'] in ('0','1') and enqueued >= previous_enqueued
                     and 0 <= enqueued <= resolved <= (elapsed+1.0)*1e6)
            previous_enqueued = enqueued
            sampled = number(row, 'sample_started_us') if row.get('sample_started_us') else None
            received = number(row, 'sink_received_us') if row.get('sink_received_us') else None
            if sampled is not None:
                valid = valid and enqueued <= sampled <= resolved
            if received is not None:
                valid = valid and sampled is not None and sampled <= received <= resolved
                valid = valid and abs(number(row, 'event_to_sink_us')-(received-enqueued)) <= 2.0
                valid = valid and abs(number(row, 'sample_to_sink_us')-(received-sampled)) <= 2.0
            if row['outcome'] == 'matched':
                csv_matched += 1
                valid = (valid and row['matched'] == '1' and row['lost'] == '0'
                         and not row['loss_reason'] and sampled is not None and received is not None
                         and int(row['keyboard_value']) == int(row['key_down']) and number(row, 'mouse_x') == 2)
            elif row['outcome'] == 'lost':
                csv_lost += 1
                reason = row['loss_reason']
                loss_reasons[reason] = loss_reasons.get(reason, 0)+1
                valid = (valid and row['matched'] == '0' and row['lost'] == '1'
                         and reason in ('focus_lost','timeout','invalid_payload','key_state_mismatch','payload_mismatch','run_stopped'))
                if received is None:
                    valid = valid and not row.get('event_to_sink_us') and not row.get('sample_to_sink_us')
            else:
                valid = False
            if not valid:
                bad.append(dict(row=index,event_id=row.get('event_id')))
        except (ValueError, TypeError, KeyError):
            bad.append(dict(row=index,event_id=row.get('event_id')))
    check('local_input_csv_payload_and_chronology', not bad, bad, [])
    check('local_input_csv_outcomes_match_counters', csv_matched == matched and csv_lost == lost,
          dict(csv_matched=csv_matched, csv_lost=csv_lost, matched=matched, lost=lost), 'equal by outcome')
    check('local_input_schedule_accounting', recorded and scheduled == generated+skipped,
          dict(scheduled=scheduled,generated=generated,skipped=skipped), 'scheduled = generated + skipped')
    windows = input_coverage_windows(elapsed, events)
    eligible_events = expected_slots = 0
    largest_gap = 0.0
    for begin, end in windows:
        times = sorted(time for time in event_times if begin <= time < end)
        eligible_events += len(times)
        # Fixed absolute 250ms slots beginning at t=2s; no replay-loop filter.
        first = max(0, math.ceil((begin-2.0)/.25-1e-9))
        stop = max(0, math.ceil((end-2.0)/.25-1e-9))
        expected_slots += max(0, stop-first)
        edges = [begin]+times+[end]
        largest_gap = max(largest_gap, max((b-a for a,b in zip(edges,edges[1:])), default=0.0))
    fraction = eligible_events/expected_slots if expected_slots else 0.0
    coverage = dict(windows=windows, eligible_generated=eligible_events, expected_fixed_slots=expected_slots,
        generated_slot_fraction=fraction, maximum_no_event_seconds=largest_gap,
        scheduled=scheduled, skipped=skipped, loss_reasons=loss_reasons,
        definition='250ms absolute slots after startup. Only declared window/fault exercises are separate; source-loop recovery and real focus loss are not excluded. Lost is never matched and normal-profile lost must be zero.')
    check('local_input_generation_coverage', expected_slots > 0 and fraction >= limits['minimum_input_slot_coverage'],
          dict(generated=eligible_events, expected_fixed_slots=expected_slots, fraction=fraction), limits['minimum_input_slot_coverage'])
    check('local_input_generation_continues', expected_slots > 0 and largest_gap <= limits['maximum_input_event_gap_seconds'],
          largest_gap, limits['maximum_input_event_gap_seconds'])
    return checks, coverage


def audit_final_render_counters(summary, samples):
    pairs = dict(backend_uploads='backend_uploads_succeeded', backend_upload_failures='backend_upload_failures',
                 backend_recoveries='backend_recoveries', backend_resources='backend_resource_creations',
                 backend_wait_timeouts='backend_wait_timeouts')
    final = {key:summary.get(key) for key in pairs.values()}
    before = summary.get('backend_wait_timeouts_before_shutdown')
    valid = all(type(value) is int and value >= 0 for value in final.values()) and type(before) is int and before >= 0
    recorded = dict(check='final_backend_counters_recorded', passed=valid, actual=dict(final=final,before_shutdown_wait_timeouts=before), limit='explicit nonnegative integers')
    chronological = (valid and summary.get('backend_diagnostics_phase') == 'after_shutdown'
                     and final['backend_wait_timeouts'] >= before)
    if samples and valid:
        chronological = chronological and all(final[end] >= number(samples[-1], sample) for sample,end in pairs.items())
    covered = dict(check='final_backend_counters_cover_shutdown_and_samples', passed=bool(chronological),
        actual=dict(phase=summary.get('backend_diagnostics_phase'),final=final,
                    before_shutdown_wait_timeouts=before),limit='after_shutdown; final >= before shutdown and last sample')
    return [recorded, covered]


def audit_long(summary, frames, inputs, samples, events=(), source_inventory=None, limits=None):
    limits = dict(DEFAULT_LIMITS, **(limits or {}))
    checks, warnings = [], []

    def check(name, passed, actual=None, limit=None):
        checks.append(dict(check=name, passed=bool(passed), actual=actual, limit=limit))

    # The underlying analyzer is shared with the short-run comparison. It checks
    # frame chronology/PTS uniqueness and conservatively selects eligible time.
    performance = analyze(summary, frames, inputs, samples, limits['gap_threshold_ms'])
    elapsed = number(summary,'elapsed_seconds')
    check('completed_requested_run', summary.get('completed') is True, summary.get('completed'), True)
    check('minimum_elapsed_seconds', elapsed >= limits['minimum_seconds'], elapsed, limits['minimum_seconds'])
    check('offline_no_network_entry', summary.get('offline_no_network') is True,
          summary.get('offline_no_network'), True)
    check('no_worker_error', not summary.get('worker_error'), summary.get('worker_error', ''), '')
    check('normal_profile_without_fault_injection', not summary.get('fault_stage'),
          summary.get('fault_stage', ''), '')
    # This is intentionally the zero-injected-fault profile. A fault_stage label
    # never excuses unrelated errors; the separate recovery matrix handles faults.
    for field in ('backend_upload_failures', 'backend_wait_timeouts', 'backend_recoveries', 'first_hresult'):
        check('zero_' + field, summary.get(field) == 0, summary.get(field), 0)
    check('no_first_gpu_failure_stage', not summary.get('first_failure_stage'),
          summary.get('first_failure_stage', ''), '')
    if summary.get('input_probe_schema_version', 1) >= 2:
        checks.extend(audit_final_render_counters(summary, samples))
    check('performance_trace_integrity', not performance['integrity_failures'],
          performance['integrity_failures'], [])

    sample_end = performance['observed_sample_seconds']
    sample_times = [number(row, 'seconds') for row in samples]
    maximum_sample_gap = max((b-a for a,b in zip([0.0]+sample_times, sample_times)), default=elapsed)
    check('samples_cover_elapsed_time',
          0 <= elapsed-sample_end <= limits['maximum_unsampled_tail_seconds'],
          elapsed-sample_end, limits['maximum_unsampled_tail_seconds'])
    check('sample_telemetry_gap', maximum_sample_gap <= limits['maximum_sample_gap_seconds'],
          maximum_sample_gap, limits['maximum_sample_gap_seconds'])
    eligible_fraction = performance['eligible_seconds']/elapsed if elapsed > 0 else 0.0
    check('eligible_time_coverage', eligible_fraction >= limits['minimum_eligible_fraction'],
          eligible_fraction, limits['minimum_eligible_fraction'])

    bound_violations, counter_violations, gpu_violations = [], [], []
    bounds = dict(udp_packet_queue=limits['packet_queue_capacity'], cbb_packet_queue=limits['packet_queue_capacity'],
        udp_mailbox_pending=limits['mailbox_capacity'], cbb_mailbox_pending=limits['mailbox_capacity'],
        backend_deferred=limits['max_deferred'])
    cumulative = ('udp_decoded','cbb_decoded','udp_presented','cbb_presented','successful_presents',
        'udp_packet_dropped','cbb_packet_dropped','udp_overwritten','cbb_overwritten',
        'backend_uploads','backend_upload_failures','backend_recoveries','backend_resources','backend_wait_timeouts')
    for index,row in enumerate(samples):
        for field,cap in bounds.items():
            value = number(row,field)
            if value != int(value) or not 0 <= value <= cap:
                bound_violations.append(dict(row=index, seconds=sample_times[index], field=field, actual=value, maximum=cap))
        for field in cumulative:
            value = number(row,field)
            if value < 0 or value != int(value) or (index and value < number(samples[index-1],field)):
                counter_violations.append(dict(row=index,field=field,actual=value))
        for field in ('backend_upload_failures','backend_recoveries','backend_wait_timeouts'):
            if number(row,field) != 0:
                gpu_violations.append(dict(row=index,field=field,actual=number(row,field)))
    check('sample_queue_and_mailbox_bounds', not bound_violations, bound_violations, bounds)
    check('sample_cumulative_counters', not counter_violations, counter_violations, 'nonnegative integers, monotonic')
    check('zero_gpu_errors_in_all_samples', not gpu_violations, gpu_violations, [])
    for field,cap in (('packet_queue_capacity_each',limits['packet_queue_capacity']),
                      ('mailbox_capacity_each',limits['mailbox_capacity'])):
        value = summary.get(field)
        check('declared_' + field, isinstance(value,int) and 0 < value <= cap, value, cap)
    for field,cap in (('packet_queue_max',limits['packet_queue_capacity']),
                      ('mailbox_pending_at_stop',limits['mailbox_capacity'])):
        values = summary.get(field)
        valid = isinstance(values,list) and len(values)==2 and all(isinstance(v,int) and 0<=v<=cap for v in values)
        check('summary_' + field + '_bounded', valid, values, cap)
    for field in ('packet_dropped','mailbox_overwritten'):
        values=summary.get(field)
        check('summary_' + field + '_recorded',isinstance(values,list) and len(values)==2
            and all(isinstance(value,int) and value>=0 for value in values),values,'two nonnegative counters')

    lane_frames = {lane:[r for r in frames if r['lane']==lane] for lane in ('udp','cbb')}
    lane_times = {lane:sorted(number(r,'present_us')/1e6 for r in rows) for lane,rows in lane_frames.items()}
    full_run = {}
    for lane,rows in lane_frames.items():
        item = performance['lanes'][lane]
        decoded, presented = summary.get(lane+'_decoded',0), summary.get(lane+'_new_presented',0)
        known = [r for r in rows if r['associated']=='1']
        fraction = len(known)/len(rows) if rows else 0.0
        check(lane+'_final_present_count_matches_csv', presented == len(rows),
              dict(summary=presented,csv=len(rows)), 'equal')
        check(lane+'_decode_present_counts', decoded >= presented > 0,
              dict(decoded=decoded,presented=presented), '0 < presented <= decoded')
        check(lane+'_eligible_association', item['association_fraction'] >= limits['minimum_association_fraction'],
              item['association_fraction'], limits['minimum_association_fraction'])
        check(lane+'_full_run_association', fraction >= limits['minimum_association_fraction'],
              fraction, limits['minimum_association_fraction'])
        unique = {(r['source_loop'],r['token']) for r in known}
        check(lane+'_full_run_no_duplicate_tokens', len(unique)==len(known),
              len(known)-len(unique), 0)
        index = 0 if lane=='udp' else 1
        final_since_present_ms = (elapsed-lane_times[lane][-1])*1000 if rows else None
        full_run[lane] = dict(decoded=decoded,presented=presented,associated_presented=len(known),
            association_fraction=fraction, decode_to_present_fraction=presented/decoded if decoded else None,
            packet_dropped=summary.get('packet_dropped',[None,None])[index],
            mailbox_overwritten=summary.get('mailbox_overwritten',[None,None])[index],
            final_since_last_new_present_ms=final_since_present_ms,
            final_decoded_frame_age_ms=summary.get(lane+'_final_age_ms'),
            decoded_dimensions=summary.get(lane+'_decoded_dimensions'),
            displayed_dimensions=summary.get(lane+'_displayed_dimensions'))
        if full_run[lane]['packet_dropped'] or full_run[lane]['mailbox_overwritten']:
            warnings.append(lane+': full-run input drops/mailbox overwrites are nonzero; inspect retained frame counts, not latency alone.')
        if presented < decoded:
            warnings.append(lane+': decoded/presented totals differ; disclose the retention ratio when interpreting low latency.')
        if samples:
            for sample_field,summary_value in ((lane+'_decoded',decoded),(lane+'_presented',presented)):
                check(lane+'_final_'+sample_field+'_covers_last_sample',
                      summary_value >= number(samples[-1],sample_field),
                      dict(summary=summary_value,last_sample=number(samples[-1],sample_field)), 'summary >= last sample')

    # Per eligible window AND per retained adjacent-sample interval. No frame-rate
    # or freshness value is used to select any interval or decide resource checks.
    windows = []
    per_sample = []
    for interval in performance['eligible_intervals']:
        begin,end = interval['begin'],interval['end']
        row = dict(begin=begin,end=end,seconds=end-begin,source_loop=interval['source_loop'],lanes={})
        for lane,times in lane_times.items():
            left,right = bisect.bisect_left(times,begin),bisect.bisect_right(times,end)
            row['lanes'][lane] = dict(new_presented=right-left,actual_new_fps=(right-left)/(end-begin),
                gaps_over_250ms=window_gaps(times,begin,end,limits['gap_threshold_ms']))
        windows.append(row)
        for previous,current in zip(samples,samples[1:]):
            a,b=number(previous,'seconds'),number(current,'seconds')
            if begin <= a < b <= end:
                item=dict(begin=a,end=b,seconds=b-a,source_loop=interval['source_loop'],lanes={})
                for lane,times in lane_times.items():
                    left,right=bisect.bisect_right(times,a),bisect.bisect_right(times,b)
                    item['lanes'][lane]=dict(new_presented=right-left,actual_new_fps=(right-left)/(b-a),
                        gaps_over_250ms=window_gaps(times,a,b,limits['gap_threshold_ms']))
                per_sample.append(item)
    no_image_windows=[dict(begin=row['begin'],end=row['end'],lane=lane)
        for row in windows for lane,item in row['lanes'].items() if not item['new_presented']]
    # This is a separate continuity observation, not a client-bug diagnosis. A
    # source outage can cause exactly the same visible symptom.
    continuity = dict(evaluated=True, no_image_windows=no_image_windows,
        no_eligible_gaps_over_threshold=all(performance['lanes'][lane]['update_gaps']['count']==0 for lane in lane_frames),
        final_new_present_within_threshold=all(full_run[lane]['final_since_last_new_present_ms'] is not None
            and 0<=full_run[lane]['final_since_last_new_present_ms']<=limits['gap_threshold_ms'] for lane in lane_frames),
        definition='Observed output continuity only. Source availability and reset/minimize periods remain separate; not proof of a client-caused freeze or physical display timing.')
    if no_image_windows:
        warnings.append('At least one eligible window has no new image; resource/integrity success must not be described as continuous playback.')
    if not continuity['no_eligible_gaps_over_threshold']:
        warnings.append('Eligible no-new-image gaps exceed the declared threshold. They remain in the report, including any source outage; review before a smoothness claim.')

    if summary.get('input_probe_schema_version', 1) >= 2:
        check('local_input_probe_enabled', summary.get('input_probe_enabled') is True, summary.get('input_probe_enabled'), True)
        input_checks, input_coverage = audit_local_input_v2(summary, inputs, elapsed, events, limits)
        checks.extend(input_checks)
        performance['input']['coverage'] = input_coverage
        if summary.get('input_events_skipped_no_catchup'):
            warnings.append('Fixed input slots were skipped without catch-up; their count and independent wall-time coverage are audited. Losses are never counted as matches.')
    else:
        # Preserve the original audit of historical captures, including the old
        # 1993/1992 input failure and absent final GPU count. Do not repair data.
        generated=summary.get('input_events_generated',0)
        matched=summary.get('input_events_matched',0)
        mismatched=summary.get('input_events_mismatched',0)
        check('local_input_probe_enabled', summary.get('input_probe_enabled') is True,summary.get('input_probe_enabled'),True)
        check('all_generated_local_input_matched', generated>0 and matched==generated and mismatched==0,
              dict(generated=generated,matched=matched,mismatched=mismatched), 'generated=matched>0; mismatched=0')
        check('local_input_csv_count_matches', len(inputs)==generated,dict(csv=len(inputs),generated=generated),'equal')
        input_bad=[]
        seen_inputs=set()
        for index,row in enumerate(inputs):
            times=[number(row,key) for key in ('event_enqueued_us','sample_started_us','sink_received_us')]
            event=row['event_id']
            if (event in seen_inputs or row['matched']!='1' or times!=sorted(times)
                    or int(row['keyboard_value']) != int(row['key_down']) or number(row,'mouse_x')!=2):
                input_bad.append(dict(row=index,event_id=event))
            seen_inputs.add(event)
        check('local_input_csv_payload_and_chronology',not input_bad,input_bad,[])
        scheduled,skipped=summary.get('input_events_scheduled',0),summary.get('input_events_skipped_no_catchup',0)
        check('local_input_schedule_accounting', scheduled==generated+skipped,
              dict(scheduled=scheduled,generated=generated,skipped=skipped),'scheduled = generated + skipped')
        if skipped:
            warnings.append('Some fixed input slots were skipped without catch-up; matched/generated excludes those slots, which are explicitly disclosed.')

    if summary.get('minimap_required'):
        check('minimap_ready_and_presented',summary.get('minimap_ready') and summary.get('minimap_presented',0)>0,
              dict(ready=summary.get('minimap_ready'),presented=summary.get('minimap_presented')),True)
        map_bad=[number(row,'seconds') for row in samples if row['excluded']=='0' and row.get('minimap_ready')!='1']
        check('minimap_ready_during_normal_samples',not map_bad,map_bad,[])

    # ALL post-warmup samples, including explicitly excluded replay/window
    # recovery, contribute to resource trends. Exclusions cannot hide leaks.
    memory_rows=[row for row in samples if number(row,'seconds')>=limits['memory_warmup_seconds']]
    memory=dict(evaluated=False,samples=len(memory_rows),all_exclusions_included=True)
    enough=len(memory_rows)>=10
    check('enough_post_warmup_samples',enough,len(memory_rows),10)
    if enough:
        first_t,last_t=number(memory_rows[0],'seconds'),number(memory_rows[-1],'seconds')
        span=last_t-first_t
        first_end,last_begin=first_t+span/4,last_t-span/4
        private=[(number(row,'seconds'),number(row,'private_bytes')/MIB) for row in memory_rows]
        handles=[(number(row,'seconds'),number(row,'handles')) for row in memory_rows]
        private_first=time_weighted_mean(private,first_t,first_end)
        private_last=time_weighted_mean(private,last_begin,last_t)
        handles_first=time_weighted_mean(handles,first_t,first_end)
        handles_last=time_weighted_mean(handles,last_begin,last_t)
        private_growth=private_last-private_first
        handle_growth=handles_last-handles_first
        tail=[point for point in private if point[0]>=first_t+span/2]
        slope=linear_slope_per_minute(tail)
        resource_start=number(memory_rows[0],'backend_resources')
        resource_end=max(number(memory_rows[-1],'backend_resources'),float(summary['backend_resource_creations']))
        resource_growth=resource_end-resource_start
        presents=max(0,float(summary['successful_present_calls'])-number(memory_rows[0],'successful_presents'))
        memory.update(evaluated=True,begin=first_t,end=last_t,first_quarter=[first_t,first_end],
            last_quarter=[last_begin,last_t],private_first_quarter_mean_mib=private_first,
            private_last_quarter_mean_mib=private_last,private_growth_mib=private_growth,
            final_half_slope_mib_per_minute=slope,handle_first_quarter_mean=handles_first,
            handle_last_quarter_mean=handles_last,handle_growth=handle_growth,
            handle_postwarmup_first=handles[0][1],handle_postwarmup_last=handles[-1][1],
            handle_postwarmup_peak=max(y for _,y in handles),private_postwarmup_peak_mib=max(y for _,y in private),
            private_postwarmup_last_mib=private[-1][1],
            platform_resources_postwarmup_start=resource_start,platform_resources_final=resource_end,
            platform_resource_growth=resource_growth,resources_per_present=resource_growth/max(1,presents))
        check('private_memory_quarter_mean_growth',private_growth<=limits['max_private_growth_mib'],private_growth,limits['max_private_growth_mib'])
        check('private_memory_final_half_slope',slope is not None and slope<=limits['max_private_slope_mib_per_minute'],slope,limits['max_private_slope_mib_per_minute'])
        check('handle_quarter_mean_growth',handle_growth<=limits['max_handle_growth'],handle_growth,limits['max_handle_growth'])
        check('no_repeated_platform_resource_creation',0<=resource_growth<=limits['max_resource_growth'],resource_growth,limits['max_resource_growth'])

    source = dict(raw_source_sha256=summary['raw_source_sha256'],replay_window_seconds=summary['replay_window_seconds'],
        selected_source_begin_us=summary.get('selected_source_begin_us'),selected_source_end_us=summary.get('selected_source_end_us'),
        selected_source_packets=summary.get('selected_source_packets'),replay_period_us=summary.get('replay_period_us'),
        source_loops=summary.get('source_loops'),input_lateness_max_us=summary.get('input_lateness_max_us'),
        holes_policy='No source-supply or low-FPS filtering. All source holes that fall in eligible time remain measured no-new-image gaps. An output gap alone cannot locate its cause.',
        availability_inventory_supplied=source_inventory is not None,listed_source_gaps=[])
    if source_inventory is not None:
        matching=source_inventory.get('sha256')==summary['raw_source_sha256']
        check('source_inventory_hash_matches',matching,source_inventory.get('sha256'),summary['raw_source_sha256'])
        if matching:
            begin=summary.get('selected_source_begin_us',0)/1e6
            end=summary.get('selected_source_end_us',summary.get('source_original_span_us',0))/1e6
            for lane in source_inventory.get('lanes',[]):
                for seconds,a,b in lane.get('largest_gaps_seconds',[]):
                    if seconds*1000>limits['gap_threshold_ms']:
                        source['listed_source_gaps'].append(dict(lane=lane['name'],seconds=seconds,begin=a,end=b,
                            intersects_selected_window=max(begin,a)<min(end,b)))
            source['inventory_limit']='Inventory lists selected largest raw-RX gaps, not necessarily every gap or AU completion; never used to remove measured time.'
    else:
        warnings.append('No raw-source availability inventory supplied. Output gaps are reported but not attributed to the client.')

    window_events=[row for row in events if row.get('event')=='window_action']
    check('window_action_count_matches_event_log',len(window_events)==summary.get('window_actions',0),
          dict(summary=summary.get('window_actions',0),events=len(window_events)),'equal')
    failures=[row for row in checks if not row['passed']]
    return dict(acceptance_passed=not failures,
        acceptance_scope='Offline one-hour completion, recorded-stream association, local-input integrity and declared resource/queue limits. FPS/smoothness and vehicle acceptance are separate.',
        performance_acceptance_evaluated=False,thresholds=limits,checks=checks,failures=failures,warnings=warnings,
        measured_seconds=elapsed,sample_count=len(samples),maximum_sample_gap_seconds=maximum_sample_gap,
        eligible_fraction=eligible_fraction,full_run=full_run,input=performance['input'],memory=memory,
        source=source,window_actions=dict(count=summary.get('window_actions',0),events=window_events,
            menu_exercises=summary.get('menu_exercises',0)),
        output_continuity=continuity,performance=performance,visible_windows=windows,visible_sample_intervals=per_sample,
        definitions=dict(memory='All samples after 60-second warmup, including exclusions; time-weighted first/last time-quarter means. Final-half OLS uses actual seconds. These engineering limits are not proof of zero leak.',
            gap_count='The whole-run analyzer counts an underlying gap once. Per-window/sample gap entries are clipped observations and must not be added as event counts across windows.',
            present='Actual new decoded source images submitted by successful Present return, not UI draws or repeated old pictures.',
            input='Synthetic local ImGui/raw events to real sampling, serialization and local protobuf parse; no MQTT, vehicle or physical-key latency.',
            continuity='No fixed FPS pass mark. A checked resource/integrity result must not be reported as smooth continuous playback without separately examining all frame/gap observations.'))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run',type=Path)
    parser.add_argument('--out',type=Path,required=True)
    parser.add_argument('--source-inventory',type=Path)
    parser.add_argument('--minimum-seconds',type=float,default=DEFAULT_LIMITS['minimum_seconds'])
    parser.add_argument('--minimum-eligible-fraction',type=eligible_fraction_argument,
                        default=DEFAULT_LIMITS['minimum_eligible_fraction'])
    parser.add_argument('--max-resource-growth',type=int,default=DEFAULT_LIMITS['max_resource_growth'])
    args=parser.parse_args()
    # Protect the four owning records even if the caller accidentally names one
    # as output. The tool never writes into an existing capture/CSV/summary file.
    output=args.out.resolve()
    owning=[(args.run/name).resolve() for name in ('summary.json','frames.csv','input.csv','samples.csv','events.csv')]
    if output in owning or (args.source_inventory and output==args.source_inventory.resolve()):
        parser.error('Output must be a new audit report, never an input record')
    try:
        summary=json.loads((args.run/'summary.json').read_text(encoding='utf-8-sig'))
        frames=load_csv(args.run/'frames.csv')
        inputs=load_csv(args.run/'input.csv')
        samples=load_csv(args.run/'samples.csv')
        events=load_csv(args.run/'events.csv')
        inventory=json.loads(args.source_inventory.read_text(encoding='utf-8-sig')) if args.source_inventory else None
        limits=dict(minimum_seconds=args.minimum_seconds,max_resource_growth=args.max_resource_growth,
                    minimum_eligible_fraction=args.minimum_eligible_fraction)
        if not math.isfinite(args.minimum_seconds) or args.minimum_seconds<=0 or args.max_resource_growth<0:
            raise ValueError('Limits must be finite and positive (resource growth may be zero)')
        result=audit_long(summary,frames,inputs,samples,events,inventory,limits)
        result['run']=str(args.run.resolve())
        result['auditor_sha256']=hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
        result['performance_analyzer_sha256']=hashlib.sha256(Path(__file__).with_name('analyze_video_performance.py').read_bytes()).hexdigest()
        result['input_files']={path.name:dict(bytes=path.stat().st_size,mtime_ns=path.stat().st_mtime_ns) for path in owning}
    except (OSError,ValueError,KeyError,TypeError,IndexError) as error:
        result=dict(acceptance_passed=False,input_validation_error=str(error),run=str(args.run.resolve()),
                    acceptance_scope='No acceptance conclusion; input records could not be validated')
    args.out.parent.mkdir(parents=True,exist_ok=True)
    args.out.write_text(json.dumps(result,ensure_ascii=False,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print('OFFLINE LONG-RUN INTEGRITY/RESOURCE CHECK: '+('PASSED' if result['acceptance_passed'] else 'FAILED'))
    print('FPS, visible gaps and source availability remain separate observations; this is not vehicle acceptance.')
    for failure in result.get('failures',[])[:12]:
        print(json.dumps(failure,ensure_ascii=True))
    if result.get('input_validation_error'):
        print(result['input_validation_error'])
    if 'performance' in result:
        for lane,item in result['performance']['lanes'].items():
            print(lane,'eligible_fps=%.3f'%item['actual_new_fps'],'association=%.6f'%item['association_fraction'],
                'gaps_over_250ms=',item['update_gaps']['count'],'decoded/presented=',
                (result['full_run'][lane]['decoded'],result['full_run'][lane]['presented']))
        print('private_growth_mib=',result['memory'].get('private_growth_mib'),
              'tail_slope_mib_per_min=',result['memory'].get('final_half_slope_mib_per_minute'),
              'handles_growth=',result['memory'].get('handle_growth'))
    return 0 if result['acceptance_passed'] else 2


if __name__=='__main__':
    raise SystemExit(main())
