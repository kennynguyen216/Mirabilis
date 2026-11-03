import unittest
import physics_gate


class PhysicsTelemetry(unittest.TestCase):
    def records(self):
        values = []
        for frame, phase in enumerate(['awake']*4 + ['sleep']*2 + ['wake', 'teleport', 'restore', 'edit-restore', 'unchanged-restore', 'reopened', 'sandbox']):
            count = 1 if frame < 4 else 2 if frame < 6 else 3
            values.append({'phase': phase, 'frame': frame+1, 'bodies': 3,
                'sdf': count, 'surface': count, 'ray-shadow': count, 'trace': count,
                'eligible': 3 if phase == 'sleep' else 2, 'dynamic-draws': 1,
                'sdf-enabled': 1, 'surface-enabled': 1, 'ray-shadow-enabled': 1, 'trace-enabled': 0,
                'sdf-placed': 3 if phase == 'sleep' else 2, 'sdf-instances': 3 if phase == 'sleep' else 2,
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

    def test_hardware_motion_updates_instances_without_rebuilding_meshes(self):
        records=self.records()
        for i,item in enumerate(records):
            item.update({'hardware-world':1,'ray-blas':2,'ray-shadow':i+1,'sdf':0,'sdf-placed':0})
        physics_gate.check_raster(records)
        records[3]['ray-blas']=3
        with self.assertRaises(AssertionError):physics_gate.check_raster(records)
        records[3]['ray-blas']=2
        for item in records:item['ray-shadow']=1
        with self.assertRaises(AssertionError):physics_gate.check_raster(records)

    def test_child_variant_retains_input_and_uses_fresh_id(self):
        source = {'version': 2, 'nextPersistentId': 8, 'objects': [{'id': 2, 'components': {'rigidBody': {'motion': 'Dynamic'}}}]}
        variant = physics_gate.child_variant(source)
        self.assertEqual(len(source['objects']), 1)
        self.assertEqual(variant['objects'][-1]['id'], 8)
        self.assertEqual(variant['objects'][-1]['parent'], 2)
        self.assertEqual(variant['objects'][-1]['components']['rigidBody']['motion'], 'Static')
        self.assertEqual(variant['nextPersistentId'], 9)

    def test_software_gi_keeps_distance_fields_stable_with_hardware_shadows(self):
        records = self.records()
        for i, item in enumerate(records):
            item.update({'hardware-world':0, 'ray-blas':2, 'ray-shadow':i+1})
        physics_gate.check_raster(records)
        records[3]['sdf'] += 1
        with self.assertRaises(AssertionError): physics_gate.check_raster(records)

    def test_software_gi_without_hardware_shadows_needs_no_ray_scene(self):
        records = self.records()
        for item in records:
            item.update({'hardware-world':0, 'ray-blas':0, 'ray-shadow':0, 'ray-shadow-enabled':0})
        physics_gate.check_raster(records)

    def test_trace_unchanged_restore_rebuild_is_rejected(self):
        records = self.records()
        physics_gate.check_trace_restore(records)
        records[10]['trace'] = 1
        with self.assertRaises(AssertionError):
            physics_gate.check_trace_restore(records)


if __name__ == '__main__':
    unittest.main()
