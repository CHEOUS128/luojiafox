"""Synthetic auditor self-tests, never used as measured performance evidence."""
import copy
import unittest
from analyze_video_performance import analyze, update_gaps


class AnalysisTests(unittest.TestCase):
    def fixture(self):
        summary = dict(performance_profile='test',elapsed_seconds=10,completed=True,raw_source_sha256='fixture',
            replay_window_seconds=35,private_bytes_start=100,private_bytes_peak=120,private_bytes_last=110,
            handles_start=20,handles_last=20,backend_resource_creations=19,backend_recoveries=0)
        samples=[dict(seconds=str(i),excluded='1' if i==1 else '0',private_bytes='110',source_loop='1')for i in range(1,11)]
        frames=[]
        for lane in ('udp','cbb'):
            for i in range(1,200):
                presented=i*50000
                row=dict(lane=lane,excluded='0',associated='1',source_loop='0',token=str(i),
                    present_us=str(presented),ingress_to_present_ms='5',conversion_total_us='100',convert_us='90',copy_us='10',
                    copied_bytes='1024',conversion_allocation='0',upload_us='20')
                for key,delta in [('first_ingress_us',6000),('complete_ingress_us',5000),('decode_submit_us',4500),('decoded_us',1000),('converted_us',900),('upload_begin_us',500),('upload_end_us',400)]:
                    row[key]=str(presented-delta)
                frames.append(row)
        return summary,frames,[],samples

    def test_stable(self):
        r=analyze(*self.fixture())
        self.assertFalse(r['integrity_failures'])
        self.assertTrue(r['lanes']['udp']['latency_ready'])
        self.assertEqual(r['lanes']['udp']['update_gaps']['count'],0)

    def test_tail_freeze_not_hidden_by_lifetime_total(self):
        summary,frames,inputs,samples=self.fixture()
        frames=[r for r in frames if float(r['present_us'])<3000000]
        r=analyze(summary,frames,inputs,samples)
        self.assertGreater(r['lanes']['udp']['update_gaps']['maximum_ms'],6000)

    def test_unknown_never_enters_latency_quantile(self):
        fixture=self.fixture()
        for row in fixture[1]: row['associated']='0'; row['ingress_to_present_ms']='-1'
        r=analyze(*fixture)
        self.assertIsNone(r['lanes']['udp']['ingress_to_present_ms']['p95'])
        self.assertFalse(r['lanes']['udp']['latency_ready'])
        self.assertEqual(r['lanes']['udp']['rgba_total_us']['p95'],100)

    def test_explicit_exclusion_splits_gap_windows(self):
        summary,frames,inputs,samples=self.fixture()
        for row in samples:
            if 3<=int(row['seconds'])<=6: row['excluded']='1'
        frames=[r for r in frames if not 2000000<float(r['present_us'])<6000000]
        r=analyze(summary,frames,inputs,samples)
        self.assertEqual(r['lanes']['udp']['update_gaps']['count'],0)

    def test_duplicate_pts_is_integrity_error(self):
        summary,frames,inputs,samples=self.fixture()
        target=next(r for r in frames if r['lane']=='udp'and float(r['present_us'])>2000000)
        following=frames[frames.index(target)+1]
        following['token']=target['token']
        r=analyze(summary,frames,inputs,samples)
        self.assertTrue(any('repeated' in x for x in r['integrity_failures']))

    def test_startup_denominator_never_includes_excluded_frames(self):
        summary,frames,inputs,samples=self.fixture()
        for row in frames:
            if float(row['present_us']) < 2000000:
                row['excluded']='1'
        r=analyze(summary,frames,inputs,samples)
        self.assertEqual(r['eligible_windows'],[[2.,10.]])
        self.assertEqual(r['eligible_seconds'],8.)
        self.assertEqual(r['excluded_seconds'],2.)
        self.assertEqual(r['lanes']['udp']['actual_new_fps'],20.)
        self.assertEqual(r['lanes']['udp']['new_presented'],160)

    def test_no_normal_endpoint_before_two_seconds_is_an_interval(self):
        summary,frames,inputs,samples=self.fixture()
        samples[0]['excluded']='0'
        r=analyze(summary,frames,inputs,samples)
        self.assertEqual(r['eligible_windows'],[[2.,10.]])

    def test_reset_does_not_join_two_source_loops(self):
        summary,frames,inputs,samples=self.fixture()
        for row in samples:
            if int(row['seconds']) >= 6: row['source_loop']='2'
        for row in frames:
            if float(row['present_us']) >= 6000000: row['source_loop']='1'
        r=analyze(summary,frames,inputs,samples)
        self.assertEqual(r['eligible_windows'],[[2.,5.],[6.,10.]])
        self.assertTrue(any(item['reason']=='source_loop_changed' for item in r['rejected_sample_intervals']))

    def test_excluded_frame_inside_normal_sample_endpoints_vetoes_interval(self):
        summary,frames,inputs,samples=self.fixture()
        next(row for row in frames if row['lane']=='udp' and row['present_us']=='4500000')['excluded']='1'
        r=analyze(summary,frames,inputs,samples)
        self.assertEqual(r['eligible_windows'],[[2.,4.],[5.,10.]])
        self.assertTrue(any(item['reason']=='excluded_frame_inside_interval' for item in r['rejected_sample_intervals']))

    def test_missing_source_frames_remain_a_measured_gap(self):
        summary,frames,inputs,samples=self.fixture()
        frames=[row for row in frames if not 4000000<float(row['present_us'])<6000000]
        r=analyze(summary,frames,inputs,samples)
        self.assertEqual(r['eligible_windows'],[[2.,10.]])
        self.assertEqual(r['lanes']['udp']['update_gaps']['count'],1)
        self.assertAlmostEqual(r['lanes']['udp']['update_gaps']['total_gap_ms'],2000.)

    def test_one_freeze_crossing_exclusion_is_not_counted_twice(self):
        r=update_gaps([1.,9.],[[2.,3.],[5.,8.]],250.)
        self.assertEqual(r['count'],1)
        self.assertEqual(r['eligible_segment_count'],2)
        self.assertEqual(r['total_gap_ms'],4000.)
        self.assertEqual(r['maximum_ms'],3000.)

    def test_empty_window_tail_is_one_gap_not_two(self):
        r=update_gaps([],[[2.,10.]],250.)
        self.assertEqual(r['count'],1)
        self.assertEqual(r['total_gap_ms'],8000.)

    def test_final_unsampled_tail_is_disclosed_not_counted_twice(self):
        summary,frames,inputs,samples=self.fixture()
        summary['elapsed_seconds']=11.
        frames=[row for row in frames if float(row['present_us'])<=7000000]
        r=analyze(summary,frames,inputs,samples)
        self.assertEqual(r['unassessed_tail_seconds'],1.)
        self.assertEqual(r['lanes']['udp']['update_gaps']['count'],1)
        self.assertEqual(r['lanes']['udp']['update_gaps']['maximum_ms'],3000.)

    def test_input_enqueue_and_sink_must_share_eligible_window(self):
        summary,frames,inputs,samples=self.fixture()
        for begin,end in [(1990000,2010000),(2100000,2110000)]:
            inputs.append(dict(excluded='0',matched='1',event_enqueued_us=str(begin),
                sample_started_us=str(end-10),sink_received_us=str(end),
                event_to_sink_us=str(end-begin),sample_to_sink_us='10'))
        r=analyze(summary,frames,inputs,samples)
        self.assertEqual(r['input']['eligible'],1)
        self.assertEqual(r['input']['outside_or_excluded_rows'],1)
        self.assertEqual(r['input']['event_to_local_sink_us']['p95'],10000.)

    def test_retention_is_separate_from_association_coverage(self):
        summary,frames,inputs,samples=self.fixture()
        summary.update(udp_decoded=400,udp_new_presented=200,packet_dropped=[2,0],mailbox_overwritten=[200,0])
        r=analyze(summary,frames,inputs,samples)
        self.assertEqual(r['lanes']['udp']['association_fraction'],1.)
        self.assertEqual(r['lanes']['udp']['full_run_decode_to_present_fraction'],.5)
        self.assertEqual(r['lanes']['udp']['full_run_packet_dropped'],2)
        self.assertEqual(r['lanes']['udp']['full_run_mailbox_overwritten'],200)


if __name__=='__main__': unittest.main()
