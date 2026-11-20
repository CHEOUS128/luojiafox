"""Small artificial log fixtures for the audit logic only."""

import unittest
from analyze_commander_benchmark import analyze


def sample():
    lines = []
    for r in (1, 2, 3):
        for mode in ("flat", "light", "detailed"):
            prefix = f"round={r} mode={mode}"
            lines.append(f"2026-11-20 [info] COMMANDER_BEGIN {prefix} width=1920 height=1080")
            lines.append(f"2026-11-20 [info] COMMANDER_RESULT {prefix} seconds=60 valid=true presents=1999 "
                         "p95_ms=33.000 longest_ms=110.000 over33=100 f1=200 f3=200 f4=200 "
                         "gap1_ms=400.000 gap3_ms=450.000 gap4_ms=500.000")
    return lines


class AuditTests(unittest.TestCase):
    def test_one_marker_pair_uses_actual_lane_counts_and_1080p(self):
        lines = []
        for index, setting in enumerate(("off", "on")):
            for entry in sample()[2:4]:
                line = entry.replace("COMMANDER_", "COMMANDER_MARKERS_")
                line = line.replace("mode=light", f"mode=light markers={setting}")
                lines.append(line.replace("f3=200", "f3=190") if index else line)
                if "MARKERS_BEGIN " in line:
                    lines.extend(f"COMMANDER_VIDEO seat={seat} decoded=1920x1080 texture=512x288"
                                 for seat in (1, 3, 4))
        report = analyze(lines, markers=True)
        self.assertTrue(report["complete"], report["errors"])
        self.assertEqual(report["candidates"]["on"]["status"], "pass")
        self.assertEqual(report["windows"][1]["percent_of_off"], [100, 95, 100])

        for token, replacement in [("f3=190", "f3=189"), ("over33=100", "over33=101")]:
            fail = lines[:-1] + [lines[-1].replace(token, replacement)]
            self.assertEqual(analyze(fail, markers=True)["candidates"]["on"]["status"], "fail")
        for invalid in [lines[:-1], lines[5:] + lines[:5], lines + lines,
                        [s for s in lines if "VIDEO seat=3 " not in s],
                        [s.replace("mode=light", "mode=detailed") for s in lines],
                        [s.replace("width=1920", "width=2560") for s in lines]]:
            self.assertFalse(analyze(invalid, markers=True)["complete"])

    def test_mode_and_lane_independence_at_exact_threshold(self):
        lines = sample()
        lines[3] = lines[3].replace("f3=200", "f3=190")
        lines[17] = lines[17].replace("f4=200", "f4=189")
        r = analyze(lines)
        self.assertTrue(r["complete"])
        self.assertEqual(r["candidates"]["light"]["status"], "pass")
        self.assertEqual(r["candidates"]["detailed"]["status"], "fail")
        self.assertEqual(r["windows"][1]["percent_of_flat"], [100, 95, 100])
        self.assertEqual(r["windows"][8]["longest_lane_gaps_ms"], [400, 450, 500])

    def test_rounded_p95_does_not_turn_a_failure_into_a_pass(self):
        lines = sample()
        lines[3] = lines[3].replace("over33=100", "over33=101")
        self.assertEqual(analyze(lines)["candidates"]["light"]["status"], "fail")

    def test_incomplete_duplicate_invalid_different_size_and_empty_baseline(self):
        lines = sample()
        for broken in [lines[:-1], lines + lines[:2],
                       [s.replace("valid=true", "valid=false") if i == 1 else s for i,s in enumerate(lines)],
                       [s.replace("width=1920", "width=2560") if i == 2 else s for i,s in enumerate(lines)],
                       [s.replace("f1=200", "f1=0") if i == 1 else s for i,s in enumerate(lines)],
                       [s.replace("p95_ms=33.000", "p95_ms=nan") if i == 3 else s for i,s in enumerate(lines)]]:
            with self.subTest(changed=broken[-1]):
                result = analyze(broken)
                self.assertFalse(result["complete"])
                self.assertEqual(result["candidates"]["light"]["status"], "unverified")


if __name__ == "__main__":
    unittest.main()
