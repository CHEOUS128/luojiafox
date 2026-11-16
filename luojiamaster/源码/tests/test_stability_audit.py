"""CPU-only checks of the measurement audit; fake rows are unit fixtures only."""
import copy
import unittest

from audit_long_video_performance import (audit_local_input_v2, audit_final_render_counters,
    input_coverage_windows, DEFAULT_LIMITS)


def input_fixture():
    rows = []
    for i in range(12):
        enqueued = 2000000+i*250000
        rows.append(dict(event_id=str(i+1),key_down=str((i+1)%2),event_enqueued_us=str(enqueued),
            sample_started_us=str(enqueued+14000),sink_received_us=str(enqueued+14001),
            event_to_sink_us='14001',sample_to_sink_us='1',mouse_x='2',keyboard_value=str((i+1)%2),
            matched='1',excluded='0',outcome='matched',lost='0',loss_reason='',resolved_us=str(enqueued+14001)))
    summary = dict(input_events_generated=12,input_events_matched=12,input_events_lost=0,
        input_events_mismatched=0,input_events_scheduled=12,input_events_skipped_no_catchup=0,
        input_events_pending_before_finish=False,input_events_pending_at_stop=False)
    return summary, rows


def failures(checks):
    return [row['check'] for row in checks if not row['passed']]


class InputAuditTest(unittest.TestCase):
    def test_normal_complete_event_accounting(self):
        summary, rows = input_fixture()
        checks, coverage = audit_local_input_v2(summary,rows,5.0)
        self.assertEqual(failures(checks),[])
        self.assertEqual(coverage['expected_fixed_slots'],12)
        self.assertEqual(coverage['generated_slot_fraction'],1)
        self.assertEqual(coverage['maximum_no_event_seconds'],.25)

    def test_lost_is_recorded_and_fails_normal_profile(self):
        summary, rows = input_fixture()
        summary.update(input_events_matched=11,input_events_lost=1)
        rows[3].update(matched='0',outcome='lost',lost='1',loss_reason='focus_lost',
            sink_received_us='',event_to_sink_us='',sample_to_sink_us='',mouse_x='',keyboard_value='')
        checks, coverage = audit_local_input_v2(summary,rows,5.0)
        self.assertEqual(failures(checks),['all_generated_local_input_matched'])
        self.assertEqual(coverage['loss_reasons'],{'focus_lost':1})

    def test_disguised_lost_or_duplicate_csv_fails(self):
        summary, rows = input_fixture()
        rows[2]['lost']='1'
        rows[3]['event_id']=rows[2]['event_id']
        checks,_=audit_local_input_v2(summary,rows,5.0)
        self.assertIn('local_input_csv_payload_and_chronology',failures(checks))
        rows.pop()
        checks,_=audit_local_input_v2(summary,rows,5.0)
        self.assertIn('local_input_csv_count_matches',failures(checks))

    def test_pending_finished_as_lost_does_not_hide_stop(self):
        summary, rows = input_fixture()
        summary['input_events_pending_before_finish']=True
        checks,_=audit_local_input_v2(summary,rows,5.0)
        self.assertIn('no_pending_input_before_finish',failures(checks))

    def test_probe_stopped_but_all_old_events_matched_fails(self):
        summary, rows = input_fixture()
        # Valid old outputs cannot prove continued generation during 60 seconds.
        summary.update(input_events_scheduled=232,input_events_skipped_no_catchup=220)
        checks,coverage=audit_local_input_v2(summary,rows,60.0)
        self.assertIn('local_input_generation_coverage',failures(checks))
        self.assertIn('local_input_generation_continues',failures(checks))
        self.assertGreater(coverage['maximum_no_event_seconds'],50)

    def test_tiny_periodic_generation_cannot_pass_coverage(self):
        summary, rows = input_fixture()
        rows=rows[::4]
        for i,row in enumerate(rows): row['event_id']=str(i+1)
        summary.update(input_events_generated=3,input_events_matched=3,
                       input_events_skipped_no_catchup=9)
        checks,coverage=audit_local_input_v2(summary,rows,5.0)
        self.assertIn('local_input_generation_coverage',failures(checks))
        self.assertNotIn('local_input_generation_continues',failures(checks))
        self.assertEqual(coverage['generated_slot_fraction'],.25)

    def test_video_loop_does_not_exclude_input(self):
        events=[dict(seconds='3',event='source_loop',detail='2'),
                dict(seconds='3',event='loop_parameter_recovery',detail='2')]
        self.assertEqual(input_coverage_windows(10,events),[[2.0,10]])
        summary,rows=input_fixture()
        a,b=audit_local_input_v2(summary,rows,5.0,events)
        self.assertEqual(failures(a),[])
        self.assertEqual(b['expected_fixed_slots'],12)

    def test_only_declared_window_and_fault_exercises_are_separate(self):
        events=[dict(seconds='3',event='window_action',detail='2'),
                dict(seconds='5',event='window_action',detail='0'),
                dict(seconds='9',event='focus_lost',detail='external'),
                dict(seconds='12',event='fault_begin',detail='wait'),
                dict(seconds='13',event='fault_clear',detail='wait')]
        self.assertEqual(input_coverage_windows(20,events),[[2,3],[7,12],[15,20]])
        self.assertEqual(DEFAULT_LIMITS['minimum_input_slot_coverage'],.95)
        self.assertEqual(DEFAULT_LIMITS['maximum_input_event_gap_seconds'],2.0)

    def test_missing_counter_is_not_assumed_zero(self):
        summary,rows=input_fixture();summary.pop('input_events_lost')
        checks,_=audit_local_input_v2(summary,rows,5)
        self.assertIn('local_input_counters_recorded',failures(checks))
        summary['input_events_lost']=None
        checks,_=audit_local_input_v2(summary,rows,5)
        self.assertIn('local_input_counters_recorded',failures(checks))


class RenderCounterAuditTest(unittest.TestCase):
    def fixture(self):
        return dict(backend_uploads_succeeded=20,backend_upload_failures=0,backend_recoveries=0,
            backend_resource_creations=19,backend_wait_timeouts=0,
            backend_wait_timeouts_before_shutdown=0,backend_diagnostics_phase='after_shutdown')
    def test_actual_final_counters_required(self):
        summary=self.fixture()
        self.assertEqual(failures(audit_final_render_counters(summary,[])),[])
        summary.pop('backend_wait_timeouts')
        self.assertEqual(len(failures(audit_final_render_counters(summary,[]))),2)
    def test_shutdown_timeout_cannot_be_replaced_with_sample_zero(self):
        summary=self.fixture();summary.update(backend_wait_timeouts=1)
        sample=dict(backend_uploads='20',backend_upload_failures='0',backend_recoveries='0',
                    backend_resources='19',backend_wait_timeouts='0')
        self.assertEqual(failures(audit_final_render_counters(summary,[sample])),[])
        # Integrity succeeds, but normal-profile zero-error check must still fail.
        self.assertNotEqual(summary['backend_wait_timeouts'],0)
        summary.update(backend_wait_timeouts=0,backend_wait_timeouts_before_shutdown=1)
        self.assertIn('final_backend_counters_cover_shutdown_and_samples',
                      failures(audit_final_render_counters(summary,[sample])))
    def test_final_smaller_than_last_sample_fails(self):
        summary=self.fixture()
        sample=dict(backend_uploads='21',backend_upload_failures='0',backend_recoveries='0',
                    backend_resources='19',backend_wait_timeouts='0')
        self.assertIn('final_backend_counters_cover_shutdown_and_samples',
                      failures(audit_final_render_counters(summary,[sample])))


if __name__ == '__main__':
    unittest.main(verbosity=2)
