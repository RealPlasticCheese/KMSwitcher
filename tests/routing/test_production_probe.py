from pathlib import Path
import unittest
from diagnostics.routing.model import Station, Rect, LEFT, RIGHT
from production import ProductionRouter


@unittest.skipUnless((Path(__file__).resolve().parents[2] / 'build/desktop-tests/desktop-tests.dll').exists(),
                     'Run python scripts/test-desktop.py to build the production probe')
class ProductionProbeTests(unittest.TestCase):
    def test_probe_uses_real_cross_ready_selection_and_rejects_internal_seams(self):
        router = ProductionRouter()
        router.load((Station(1, (Rect(0, 0, 100, 100), Rect(100, 0, 100, 100))),
                     Station(2, (Rect(200, 0, 100, 100),))))
        self.assertEqual(router.cross(1, RIGHT, (199, 50)), 2)
        self.assertEqual(router.cross(2, LEFT, (200, 50)), 1)
        self.assertEqual(router.cross(1, RIGHT, (99, 50)), 0)

    def test_probe_reports_rejected_route_without_inventing_a_selection(self):
        router = ProductionRouter()
        router.load((Station(1, (Rect(0, 0, 100, 100),)),
                     Station(2, (Rect(100, 0, 100, 100),), power='sleep', reachable=False)))
        self.assertEqual(router.cross(1, RIGHT, (99, 50)), 0)
