import unittest
import physics_gate


class PhysicsTelemetry(unittest.TestCase):
    def records(self):
        values = []
        for frame, phase in enumerate(['awake']*4 + ['sleep']*2 + ['wake', 'teleport', 'restore', 'edit-restore', 'unchanged-restore', 'reopened', 'sandbox']):
            count = 1 if frame < 4 else 2 if frame < 6 else 3
            values.append({'phase': phase, 'frame': frame+1, 'bodies': 3,
                'sdf': count, 'surface': count, 'ray-shadow': count, 'trace': 0,
                'eligible': 3 if phase == 'sleep' else 2, 'dynamic-draws': 1,
                'sdf-enabled': 1, 'surface-enabled': 1, 'ray-shadow-enabled': 1, 'trace-enabled': 0,
                'history-reset': 1 if phase in ('teleport', 'restore') else 0})
        return values

    def test_phase_parser_and_successful_lifecycle(self):
        records = self.records()
        text = '\n'.join('PHYSICS ' + ' '.join(f'{key}={value}' for key, value in record.items()) for record in records)
        self.assertEqual(physics_gate.telemetry(text), records)
        physics_gate.check_raster(records)

    def test_per_frame_rebuild_and_missing_dynamic_draws_fail(self):
        for field, index, value in [('sdf', 3, 9), ('dynamic-draws', 1, 0), ('history-reset', 7, 0)]:
            with self.subTest(field=field):
                records = self.records()
                records[index][field] = value
                with self.assertRaises(AssertionError):
                    physics_gate.check_raster(records)

    def test_child_variant_retains_input_and_uses_fresh_id(self):
        source = {'version': 2, 'nextPersistentId': 8, 'objects': [{'id': 2, 'components': {'rigidBody': {'motion': 'Dynamic'}}}]}
        variant = physics_gate.child_variant(source)
        self.assertEqual(len(source['objects']), 1)
        self.assertEqual(variant['objects'][-1]['id'], 8)
        self.assertEqual(variant['objects'][-1]['parent'], 2)
        self.assertEqual(variant['objects'][-1]['components']['rigidBody']['motion'], 'Static')
        self.assertEqual(variant['nextPersistentId'], 9)

    def test_trace_unchanged_restore_rebuild_is_rejected(self):
        records = self.records()
        physics_gate.check_trace_restore(records)
        records[10]['trace'] = 1
        with self.assertRaises(AssertionError):
            physics_gate.check_trace_restore(records)


if __name__ == '__main__':
    unittest.main()
