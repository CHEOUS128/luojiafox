"""Test the auditor itself using explicit synthetic telemetry, never images."""
from audit_offline_video import audit

limits=dict(max_deferred=64,min_udp_fps=1,min_cbb_fps=1,max_age_ms=2000,minimum_seconds=60,
    minimum_eligible_intervals=5,minimum_eligible_fraction=.5,memory_warmup_seconds=20,
    max_private_growth_mib=128,max_private_slope_mib_per_minute=4,max_handle_growth=32,max_resources_per_present=.02)
summary=dict(completed=True,dual_stream_smoke_ok=True,elapsed_seconds=60,worker_error='',fault_stage='')
rows=[]
for t in range(1,61):
    rows.append(dict(seconds=str(t),udp_presented=str(30*t),cbb_presented=str(20*t),successful_presents=str(60*t),
        udp_decoded=str(30*t),cbb_decoded=str(20*t),udp_packet_queue='0',cbb_packet_queue='0',udp_mailbox_pending='0',cbb_mailbox_pending='1',
        backend_deferred='4',excluded='1'if t<=2 else'0',exclusion_reason='startup'if t<=2 else'none',
        udp_age_ms='20',cbb_age_ms='30',private_bytes='300000000',handles='123',backend_resources='16'))
assert audit(summary,rows,limits)['acceptance_passed']
print('PASS stable dual-lane telemetry accepted')
frozen=[dict(r)for r in rows]
for r in frozen[30:]:
    r['udp_presented']='900';r['cbb_presented']='600';r['udp_age_ms']=str((float(r['seconds'])-30)*1000);r['cbb_age_ms']=r['udp_age_ms']
result=audit(summary,frozen,limits)
assert not result['acceptance_passed'] and any('froze'in f['reason']for f in result['failures'])
print('PASS later freeze rejected despite nonzero lifetime frame totals')
growth=[dict(r)for r in rows]
for r in growth:
    r['handles']=str(100+10*int(r['seconds']));r['private_bytes']=str(300000000+10*1048576*int(r['seconds']))
assert not audit(summary,growth,limits)['acceptance_passed']
print('PASS memory/handle linear growth rejected')
bad=[dict(r)for r in rows];bad[5]['udp_mailbox_pending']='2'
assert not audit(summary,bad,limits)['acceptance_passed']
print('PASS capacity-one violation rejected')
excluded=[dict(r,excluded='1',exclusion_reason='incorrectly exclude everything')for r in rows]
assert not audit(summary,excluded,limits)['acceptance_passed']
print('PASS hiding every sample behind exclusions rejected')
