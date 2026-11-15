"""Synthetic auditor checks only; these are not video/client measurements."""
import copy
import argparse
import unittest

from audit_long_video_performance import audit_long, time_weighted_mean, eligible_fraction_argument


def fixture(seconds=180):
    summary=dict(offline_no_network=True,completed=True,dual_stream_smoke_ok=True,
        elapsed_seconds=seconds,performance_profile='synthetic',raw_source_sha256='synthetic-not-video',
        replay_window_seconds=35,controlled_ui_work_ms=0,worker_error='',fault_stage='',
        backend_upload_failures=0,backend_wait_timeouts=0,backend_recoveries=0,first_hresult=0,first_failure_stage='',
        packet_queue_capacity_each=256,mailbox_capacity_each=1,packet_queue_max=[2,1],mailbox_pending_at_stop=[0,0],
        udp_decoded=seconds,cbb_decoded=seconds,udp_new_presented=seconds,cbb_new_presented=seconds,
        trace_unknown_decoded=[0,0],packet_dropped=[0,0],mailbox_overwritten=[0,0],
        input_probe_enabled=True,input_events_generated=1,input_events_matched=1,input_events_mismatched=0,
        input_events_scheduled=1,input_events_skipped_no_catchup=0,input_sampling_placement='early',
        private_bytes_start=300*1048576,private_bytes_peak=300*1048576,private_bytes_last=300*1048576,
        handles_start=200,handles_last=200,backend_resource_creations=19,successful_present_calls=seconds*60,
        source_loops=1,selected_source_begin_us=0,selected_source_end_us=35000000,selected_source_packets=100,
        replay_period_us=35050000,input_lateness_max_us=0,window_actions=0,menu_exercises=0,
        udp_final_age_ms=250,cbb_final_age_ms=250)
    samples=[]
    frames=[]
    for second in range(1,seconds+1):
        row=dict(seconds=str(second),excluded='1' if second<2 else '0',source_loop='1',
            exclusion_reason='startup' if second<2 else 'none',private_bytes=str(300*1048576),handles='200',
            udp_decoded=str(second),cbb_decoded=str(second),udp_presented=str(second),cbb_presented=str(second),
            successful_presents=str(second*60),udp_packet_queue='0',cbb_packet_queue='0',
            udp_mailbox_pending='0',cbb_mailbox_pending='0',backend_deferred='0',
            udp_packet_dropped='0',cbb_packet_dropped='0',udp_overwritten='0',cbb_overwritten='0',
            backend_uploads=str(second*2),backend_upload_failures='0',backend_recoveries='0',
            backend_resources='19',backend_wait_timeouts='0',udp_age_ms='250',cbb_age_ms='250')
        samples.append(row)
        present=second*1000000-250000
        for lane in ('udp','cbb'):
            frame=dict(lane=lane,excluded='1' if present<2000000 else '0',associated='1',source_loop='0',token=str(second),
                present_us=str(present),ingress_to_present_ms='5',conversion_total_us='100',convert_us='90',copy_us='10',
                copied_bytes='1024',conversion_allocation='0',upload_us='20')
            for key,delta in [('first_ingress_us',6000),('complete_ingress_us',5000),('decode_submit_us',4500),
                              ('decoded_us',1000),('converted_us',900),('upload_begin_us',500),('upload_end_us',400)]:
                frame[key]=str(present-delta)
            frames.append(frame)
    inputs=[dict(event_id='1',key_down='1',event_enqueued_us='2100000',sample_started_us='2101000',
        sink_received_us='2102000',event_to_sink_us='2000',sample_to_sink_us='1000',mouse_x='2',keyboard_value='1',
        matched='1',excluded='0')]
    return summary,frames,inputs,samples


class LongAuditTests(unittest.TestCase):
    def run_fixture(self,data=None,**limits):
        return audit_long(*(data or fixture()),limits=dict(minimum_seconds=180,**limits))

    def failed(self,result,name):
        return any(row['check']==name for row in result['failures'])

    def test_default_profile_requires_full_hour(self):
        result=audit_long(*fixture())
        self.assertFalse(result['acceptance_passed'])
        self.assertTrue(self.failed(result,'minimum_elapsed_seconds'))

    def test_one_hour_resource_integrity_profile_has_no_invented_fps_gate(self):
        result=audit_long(*fixture(3600))
        self.assertTrue(result['acceptance_passed'],result['failures'])
        self.assertFalse(result['performance_acceptance_evaluated'])
        self.assertFalse(result['output_continuity']['no_eligible_gaps_over_threshold'])
        self.assertAlmostEqual(result['performance']['lanes']['udp']['actual_new_fps'],1.)

    def test_incomplete_and_worker_error_rejected(self):
        data=fixture();data[0].update(completed=False,worker_error='fixture failure')
        result=self.run_fixture(data)
        self.assertTrue(self.failed(result,'completed_requested_run'))
        self.assertTrue(self.failed(result,'no_worker_error'))

    def test_gpu_error_never_excused_by_fault_label(self):
        data=fixture();data[0].update(fault_stage='present',backend_recoveries=1,first_hresult=-1)
        result=self.run_fixture(data)
        self.assertTrue(self.failed(result,'normal_profile_without_fault_injection'))
        self.assertTrue(self.failed(result,'zero_backend_recoveries'))
        self.assertTrue(self.failed(result,'zero_first_hresult'))

    def test_sample_only_gpu_error_rejected(self):
        data=fixture()
        for row in data[3][30:]:row['backend_wait_timeouts']='1'
        self.assertTrue(self.failed(self.run_fixture(data),'zero_gpu_errors_in_all_samples'))

    def test_queue_bound_and_transient_summary_peak_rejected(self):
        data=fixture();data[3][15]['udp_packet_queue']='257';data[0]['packet_queue_max']=[257,0]
        result=self.run_fixture(data)
        self.assertTrue(self.failed(result,'sample_queue_and_mailbox_bounds'))
        self.assertTrue(self.failed(result,'summary_packet_queue_max_bounded'))

    def test_capacity_one_mailbox_violation_rejected(self):
        data=fixture();data[3][15]['udp_mailbox_pending']='2'
        self.assertTrue(self.failed(self.run_fixture(data),'sample_queue_and_mailbox_bounds'))

    def test_hidden_telemetry_tail_rejected(self):
        data=fixture();del data[3][-10:]
        self.assertTrue(self.failed(self.run_fixture(data),'samples_cover_elapsed_time'))

    def test_excluding_most_time_cannot_pass(self):
        data=fixture()
        for row in data[3][20:150]:row['excluded']='1'
        self.assertTrue(self.failed(self.run_fixture(data),'eligible_time_coverage'))

    def test_memory_and_handle_growth_rejected_even_during_excluded_samples(self):
        data=fixture()
        for row in data[3]:
            second=float(row['seconds'])
            row['private_bytes']=str(int((300+2*second)*1048576))
            row['handles']=str(int(200+second))
            if second>100:row['excluded']='1'
        result=self.run_fixture(data)
        self.assertTrue(self.failed(result,'private_memory_quarter_mean_growth'))
        self.assertTrue(self.failed(result,'private_memory_final_half_slope'))
        self.assertTrue(self.failed(result,'handle_quarter_mean_growth'))
        self.assertTrue(result['memory']['all_exclusions_included'])

    def test_every_hundred_frames_resource_growth_not_hidden_by_old_ratio_guard(self):
        data=fixture()
        for row in data[3]:row['backend_resources']=str(19+int(row['seconds'])//100)
        data[0]['backend_resource_creations']=20
        self.assertTrue(self.failed(self.run_fixture(data),'no_repeated_platform_resource_creation'))

    def test_time_quarter_mean_uses_actual_time_not_row_density(self):
        self.assertAlmostEqual(time_weighted_mean([(0.,0.),(1.,10.),(10.,10.)],0.,10.),9.5)

    def test_real_source_hole_remains_in_output_observations(self):
        data=fixture()
        data[1][:]=[row for row in data[1] if not 10000000<float(row['present_us'])<20000000]
        for lane in ('udp','cbb'):data[0][lane+'_new_presented']=170
        for row in data[3]:
            second=int(row['seconds'])
            for lane in ('udp','cbb'):row[lane+'_presented']=str(second-min(10,max(0,second-10)))
        result=self.run_fixture(data)
        self.assertTrue(result['acceptance_passed'],result['failures'])
        self.assertEqual(result['performance']['eligible_windows'],[[2.,180.]])
        self.assertGreater(result['performance']['lanes']['udp']['update_gaps']['maximum_ms'],10000.)
        self.assertFalse(result['output_continuity']['no_eligible_gaps_over_threshold'])

    def test_all_unknown_cannot_create_low_latency_success(self):
        data=fixture()
        for row in data[1]:row['associated']='0';row['ingress_to_present_ms']='-1'
        result=self.run_fixture(data)
        self.assertTrue(self.failed(result,'udp_eligible_association'))
        self.assertIsNone(result['performance']['lanes']['udp']['ingress_to_present_ms']['p95'])

    def test_input_payload_mismatch_rejected(self):
        data=fixture();data[2][0]['mouse_x']='0'
        self.assertTrue(self.failed(self.run_fixture(data),'local_input_csv_payload_and_chronology'))

    def test_skipped_input_slots_are_disclosed_not_called_matches(self):
        data=fixture();data[0].update(input_events_scheduled=5,input_events_skipped_no_catchup=4)
        result=self.run_fixture(data)
        self.assertTrue(result['acceptance_passed'],result['failures'])
        self.assertEqual(result['input']['generated'],1)
        self.assertEqual(result['input']['skipped'],4)
        self.assertTrue(any('skipped' in item for item in result['warnings']))

    def test_summary_present_count_must_match_actual_csv(self):
        data=fixture();data[0]['udp_new_presented']=181
        self.assertTrue(self.failed(self.run_fixture(data),'udp_final_present_count_matches_csv'))

    def test_wrong_source_inventory_hash_rejected(self):
        result=audit_long(*fixture(),source_inventory=dict(sha256='wrong'),limits=dict(minimum_seconds=180))
        self.assertTrue(self.failed(result,'source_inventory_hash_matches'))

    def test_source_inventory_keeps_outside_window_holes_explicit(self):
        data=fixture()
        inventory=dict(sha256='synthetic-not-video',lanes=[dict(name='cbb',largest_gaps_seconds=[[.66,40.2,40.86]])])
        result=audit_long(*data,source_inventory=inventory,limits=dict(minimum_seconds=180))
        self.assertTrue(result['acceptance_passed'],result['failures'])
        self.assertFalse(result['source']['listed_source_gaps'][0]['intersects_selected_window'])

    def planned_actions_fixture(self):
        data=fixture()
        # Synthetic explicit recovery masks produce 73.89% eligible time. This
        # tests the coverage threshold alone, not measured video performance.
        for row in data[3][59:103]:row['excluded']='1'
        return data

    def test_explicit_seventy_percent_only_changes_coverage_gate(self):
        data=self.planned_actions_fixture()
        ordinary=self.run_fixture(data)
        declared=self.run_fixture(data,minimum_eligible_fraction=.70)
        self.assertTrue(self.failed(ordinary,'eligible_time_coverage'))
        self.assertTrue(declared['acceptance_passed'],declared['failures'])
        self.assertEqual(ordinary['performance']['eligible_windows'],declared['performance']['eligible_windows'])
        self.assertEqual(ordinary['performance']['lanes'],declared['performance']['lanes'])
        self.assertEqual(ordinary['memory'],declared['memory'])

    def test_seventy_percent_never_excuses_gpu_frame_or_memory_failures(self):
        for fault in ('gpu','frame','memory'):
            with self.subTest(fault=fault):
                data=self.planned_actions_fixture()
                if fault=='gpu':data[0]['backend_recoveries']=1
                elif fault=='frame':
                    target=next(row for row in data[1] if row['lane']=='udp' and float(row['present_us'])>110000000)
                    target['decoded_us']=str(float(target['present_us'])+1)
                else:
                    for row in data[3]:row['private_bytes']=str(int((300+float(row['seconds'])*2)*1048576))
                self.assertFalse(self.run_fixture(data,minimum_eligible_fraction=.70)['acceptance_passed'])

    def test_cli_coverage_fraction_has_closed_upper_bound_and_positive_lower_bound(self):
        self.assertEqual(eligible_fraction_argument('.70'),.70)
        self.assertEqual(eligible_fraction_argument('1'),1.)
        for value in ('0','-.1','1.01','nan','inf'):
            with self.subTest(value=value),self.assertRaises(argparse.ArgumentTypeError):
                eligible_fraction_argument(value)


if __name__=='__main__':unittest.main()
