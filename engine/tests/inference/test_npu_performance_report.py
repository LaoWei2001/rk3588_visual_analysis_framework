import unittest

from run_npu_performance import comparison, regressions


class PerformanceReportTests(unittest.TestCase):
    def test_total_gain_cannot_hide_one_channel_losing_published_results(self):
        report = {'runs': []}
        for variant, counts in [('baseline', [(100, 100), (100, 100)]),
                                ('candidate', [(130, 130), (105, 90)])]:
            report['runs'].append(dict(case='channels', variant=variant, channels={
                str(i): dict(seconds=10, completed=done, published=published, queue_worker_ms=20)
                for i, (done, published) in enumerate(counts)}))
        failures = regressions(comparison(report))
        self.assertEqual(len(failures), 1)
        self.assertEqual((failures[0]['channel'], failures[0]['metric']), ('1', 'publish_fps'))
        self.assertAlmostEqual(failures[0]['change_percent'], -10)

    def test_strict_default_checks_latency_and_tolerance_is_explicit(self):
        summary = {'dual': {'change_percent': {'0': dict(infer_fps=1, publish_fps=0,
                                                        queue_worker_ms=.1)}}}
        self.assertEqual(regressions(summary)[0]['metric'], 'queue_worker_ms')
        self.assertEqual(regressions(summary, allowed_percent=.2), [])


if __name__ == '__main__':
    unittest.main()
