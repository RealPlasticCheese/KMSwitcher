"""The same oracle/fixtures run through compiled C CROSS/PREPARE/READY/SELECT."""
import test_model
import ctypes
from production import build_production_plan
from diagnostics.routing.model import Rect, Station, RIGHT


class ProductionRoutingTests(test_model.RoutingModelTests):
    plan_factory = staticmethod(build_production_plan)

    def test_pending_native_handoff_rejects_live_changes_and_old_replies(self):
        from production import _prepare
        stations = tuple(Station(i + 1, (Rect(i * 100, 0, 100, 100),)) for i in range(3))
        for slot in (1, 2, 3):
            for cause in range(10):
                with self.subTest(slot=slot, cause=cause):
                    # A new revision forces a clean fixture; subsequent changes do not reload it.
                    native = _prepare(stations, 8, 100 + slot * 10 + cause)
                    native.kms_routing_probe_begin.argtypes = [ctypes.c_uint, ctypes.c_uint, ctypes.c_int32, ctypes.c_int32]
                    request = native.kms_routing_probe_begin(1, RIGHT, 99, 50)
                    self.assertGreater(request, 0)
                    native.kms_routing_probe_change(slot, cause)
                    self.assertEqual(native.kms_routing_probe_finish(2, request, 1), 0)

    def test_native_workspace_is_bounded_and_steady_crossings_reuse_the_plan(self):
        from production import _prepare
        stations = (Station(1, (Rect(0, 0, 100, 100),)), Station(2, (Rect(100, 0, 100, 100),)))
        plan = build_production_plan(stations, revision=999)
        native = _prepare(stations, 8, 999)
        self.assertLessEqual(native.kms_routing_probe_bytes(), 32768)
        builds = native.kms_routing_probe_builds()
        for _ in range(100): self.assertEqual(plan.cross(1, RIGHT, 99, 50).target, (2, 0))
        self.assertEqual(native.kms_routing_probe_builds(), builds)
