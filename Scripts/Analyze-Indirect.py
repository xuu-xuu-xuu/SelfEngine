"""Independently check paired drawing measurements, exact pixels and raw GPU ticks."""
import argparse
import csv
import hashlib
import json
import math
import statistics
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def table(path):
    with path.open(encoding='utf-8', newline='') as stream:
        return list(csv.DictReader(stream))


def metric(values):
    values = sorted(values)
    assert values and all(math.isfinite(v) and v >= 0 for v in values)
    def percentile(p):
        index = (len(values) - 1) * p
        lo, hi = math.floor(index), math.ceil(index)
        return values[lo] + (values[hi] - values[lo]) * (index - lo)
    return dict(count=len(values), mean=statistics.mean(values), median=statistics.median(values),
                p95=percentile(.95), p99=percentile(.99), maximum=max(values))


ALLOCATION = ('default_reserved_bytes', 'default_used_bytes', 'upload_reserved_bytes',
              'readback_allocation_bytes', 'tracked_resources')
CPU_SCOPES = dict(cpu_command_record_ms='CPU.CommandRecord', cpu_submit_present_ms='CPU.SubmitPresent',
                  cpu_fence_wait_ms='CPU.GpuFenceWait', cpu_query_read_ms='CPU.QueryRead',
                  cpu_indirect_query_read_ms='CPU.IndirectQueryRead', cpu_validation_ms='CPU.Validation',
                  cpu_mesh_validation_ms='CPU.MeshValidation', cpu_instance_packing_ms='CPU.InstancePacking')
FIELDS = ('cpu_validation_ms', 'cpu_cull_ms', 'cpu_gather_ms', 'cpu_render_ms', 'cpu_frame_ms',
          'cpu_command_record_ms', 'cpu_submit_present_ms', 'cpu_fence_wait_ms', 'cpu_query_read_ms',
          'cpu_indirect_query_read_ms', 'cpu_mesh_validation_ms', 'cpu_instance_packing_ms', 'cpu_preparation_ms', 'gpu_frame_ms', 'gpu_geometry_ms',
          'gpu_local_ms', 'gpu_group_ms', 'gpu_gather_ms', 'gpu_compute_ms', 'gpu_indirect_draw_ms')


def released(stats):
    assert stats['stopped'] and stats['warnings'] == stats['errors'] == 0
    assert all(stats[key] == 0 for key in ALLOCATION)


def close(actual, expected):
    assert math.isclose(float(actual), expected, rel_tol=1e-9, abs_tol=1e-10), (actual, expected)


def validate(root, formal):
    protocol, environment = read(root / 'protocol.json'), read(root / 'environment.json')
    assert protocol['success'] and protocol['schema'] == environment['schema'] == 2
    for key in ('source_sha256', 'executable_sha256', 'runtime_sha256', 'configuration', 'smoke'):
        assert protocol[key] == environment[key], key
    assert environment['source_and_binaries_unchanged']
    assert protocol['sizes'] == [32, 1024, 65536] and protocol['layout'] == 'SelfEngine.IndirectGrid.1'
    assert protocol['present_sync_interval'] == 1 and protocol['window_visible'] is False
    assert protocol['width'] == 320 and protocol['height'] == 240
    assert protocol['normal_count_readback'] is protocol['cpu_gpu_timings_additive'] is False
    assert protocol['candidate_scene_generation_timed'] is False and protocol['per_frame_matrix_validation_packing_timed']
    warmup, samples, repetitions = (protocol[k] for k in ('warmup_iterations', 'sample_iterations', 'repetitions'))
    assert (warmup, samples, repetitions) == ((8, 16, 1) if protocol['smoke'] else (64, 256, 3))
    if formal:
        assert not protocol['smoke'] and protocol['configuration'] == 'Release'
    checks = read(root / 'validation.json')
    assert len(checks) == protocol['validation_cases'] == 9
    identities = {}
    for check in checks:
        count, rate = check['count'], check['rate']
        assert count in protocol['sizes'] and rate in range(3)
        assert check['width'] == 320 and check['height'] == 240
        cpu = (root / 'validation' / f'{count}-{rate}-cpu.rgba').read_bytes()
        gpu = (root / 'validation' / f'{count}-{rate}-gpu.rgba').read_bytes()
        assert len(cpu) == 320 * 240 * 4 and cpu == gpu
        assert hashlib.sha256(cpu).hexdigest() == check['rgba_sha256']
        stats = check['statistics']
        assert stats['submissions'] == stats['image_readbacks'] == 2
        assert stats['result_readbacks'] == stats['warnings'] == stats['errors'] == 0
        assert stats['indirect_timing_reads'] == 1 and stats['gpu_resources_created']
        released(check['after_shutdown'])
        identities[count, rate] = check['input_sha256']
    assert set(identities) == {(c, r) for c in protocol['sizes'] for r in range(3)}
    expected = []
    for round_index in range(repetitions):
        order = list(range(9))
        if round_index % 2:
            order.reverse()
        for condition in order:
            count, rate = protocol['sizes'][condition // 3], condition % 3
            for pair in range(2):
                path = 'gpu' if (pair + (round_index + condition) % 2) % 2 else 'cpu'
                expected.append(f'{count}-{rate}-{round_index + 1}-{path}')
    assert [r['run'] for r in protocol['runs']] == expected
    runs = []
    for item in protocol['runs']:
        directory = root / item['run']
        assert read(directory / 'run.json') == item
        count, rate, path = item['count'], item['rate'], item['path']
        gpu = path == 'gpu'
        visible = 0 if rate == 0 else (count + 9) // 10 if rate == 1 else count
        assert item['input_sha256'] == identities[count, rate] and len(item['input_sha256']) == 64
        stats, initial, after = (item[k] for k in ('statistics', 'warmup_statistics', 'after_shutdown'))
        assert stats['submissions'] == warmup + samples and initial['submissions'] == warmup
        assert stats['warnings'] == stats['errors'] == stats['result_readbacks'] == stats['image_readbacks'] == 0
        assert stats['dropped_frames'] == stats['profiler_dropped_frames'] == stats['gpu_dropped_frames'] == 0
        assert stats['gpu_resources_created'] == gpu
        assert stats['uav_barriers'] == (warmup + samples) * (4 if gpu else 0)
        assert stats['indirect_timing_reads'] == (warmup + samples if gpu else 0)
        assert stats['width'] == 320 and stats['height'] == 240 and not stats['visible_window']
        if protocol['configuration'] == 'Debug':
            assert stats['debug_info_queue']
        for key in ALLOCATION:
            assert stats[key] == initial[key], (item['run'], 'allocation changed', key)
        released(after)
        scopes = {}
        for scope in table(directory / 'cpu-scopes.csv'):
            sequence = int(scope['sequence'])
            assert 1 <= sequence <= warmup + samples
            assert scope['phase'] == ('warmup' if sequence <= warmup else 'sample')
            assert int(scope['calls']) > 0 and math.isfinite(float(scope['milliseconds'])) and float(scope['milliseconds']) >= 0
            key = sequence, scope['scope']
            assert key not in scopes
            scopes[key] = float(scope['milliseconds'])
        rows = table(directory / 'frames.csv')
        assert len(rows) == warmup + samples
        measured = []
        for index, row in enumerate(rows, 1):
            assert int(row['sequence']) == index and int(row['input_count']) == count and row['path'] == path
            assert row['phase'] == ('warmup' if index <= warmup else 'sample')
            assert int(row['cpu_visible']) == (0 if gpu else visible)
            if gpu:
                assert float(row['cpu_cull_ms']) == float(row['cpu_gather_ms']) == 0
            for field, name in CPU_SCOPES.items():
                close(row[field], scopes.get((index, name), 0))
            assert (index, 'CPU.Frame') in scopes
            frequency = int(row['frequency_hz'])
            ticks = [int(row[k]) for k in ('frame_begin', 'geometry_begin', 'geometry_end', 'frame_end')]
            indirect = [int(row[k]) for k in ('scan_begin', 'local_end', 'group_end', 'gather_end', 'draw_end')]
            assert ticks[0] > 0 and ticks == sorted(ticks) and frequency > 0
            close(row['gpu_frame_ms'], (ticks[3] - ticks[0]) * 1000 / frequency)
            close(row['gpu_geometry_ms'], (ticks[2] - ticks[1]) * 1000 / frequency)
            if gpu:
                assert indirect == sorted(indirect) and ticks[1] <= indirect[0] <= indirect[4] <= ticks[2]
            else:
                assert indirect == [0] * 5
            for part, field in enumerate(('gpu_local_ms', 'gpu_group_ms', 'gpu_gather_ms', 'gpu_indirect_draw_ms')):
                close(row[field], (indirect[part + 1] - indirect[part]) * 1000 / frequency)
            row['gpu_compute_ms'] = (indirect[3] - indirect[0]) * 1000 / frequency
            row['cpu_preparation_ms'] = sum(float(row[k]) for k in ('cpu_validation_ms', 'cpu_mesh_validation_ms', 'cpu_cull_ms', 'cpu_gather_ms', 'cpu_command_record_ms'))
            assert all(math.isfinite(float(row[k])) and float(row[k]) >= 0 for k in FIELDS)
            if index > warmup:
                measured.append(row)
        runs.append(dict(run=item['run'], count=count, rate=rate, round=item['round'], path=path,
                         metrics={key: metric([float(row[key]) for row in measured]) for key in FIELDS}, statistics=stats))
    assert len({r['statistics']['adapter'] for r in runs}) == 1
    groups = {}
    for count in protocol['sizes']:
        for rate in range(3):
            for path in ('cpu', 'gpu'):
                group = [r for r in runs if r['count'] == count and r['rate'] == rate and r['path'] == path]
                assert len(group) == repetitions
                groups[f'{count}-{rate}-{path}'] = dict(count=count, rate=rate, path=path, input_sha256=identities[count, rate],
                    metrics={key: dict(mean_run_median=statistics.mean(r['metrics'][key]['median'] for r in group),
                                       stdev_run_median=statistics.stdev(r['metrics'][key]['median'] for r in group) if repetitions > 1 else 0,
                                       run_medians=[r['metrics'][key]['median'] for r in group]) for key in FIELDS})
    return dict(schema=1, formal_protocol_passed=formal, protocol=protocol, environment=environment, validations=checks, runs=runs, groups=groups)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--formal', action='store_true')
    args = parser.parse_args()
    result = validate(args.input, args.formal)
    (args.input / 'summary.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    hashes = [dict(path=str(p.relative_to(args.input)), sha256=hashlib.sha256(p.read_bytes()).hexdigest())
              for p in sorted(args.input.rglob('*')) if p.is_file() and p.name != 'artifact-digests.json']
    (args.input / 'artifact-digests.json').write_text(json.dumps(hashes, indent=2) + '\n', encoding='utf-8')
    print(f'Indirect protocol validated: {len(result["runs"])} runs; formal={args.formal}.')


if __name__ == '__main__':
    main()
