"""Validate compute visibility measurements independently of the native exporter."""
import argparse
import csv
import hashlib
import json
import math
import statistics
from pathlib import Path


def read(path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def metric(values):
    values = sorted(values)
    assert values and all(math.isfinite(v) and v >= 0 for v in values)
    def percentile(p):
        index = (len(values) - 1) * p
        lo, hi = math.floor(index), math.ceil(index)
        return values[lo] + (values[hi] - values[lo]) * (index - lo)
    return dict(count=len(values), mean=statistics.mean(values), median=statistics.median(values),
                p95=percentile(.95), p99=percentile(.99), maximum=max(values))


def validate(root, formal):
    protocol, environment = read(root / 'protocol.json'), read(root / 'environment.json')
    assert protocol['success'] and protocol['schema'] in (1, 2)
    if protocol['schema'] == 2:
        assert protocol['predicate_sha256'] == environment['predicate_sha256']
    assert protocol['source_sha256'] == environment['source_sha256']
    assert protocol['executable_sha256'] == environment['executable_sha256']
    assert protocol['shader_sha256'] == environment['shader_sha256']
    assert environment['source_and_binaries_unchanged']
    assert protocol['sizes'] == [32, 1024, 65536] and protocol['layout'] == 'SelfEngine.VisibilitySynthetic.1'
    assert protocol['normal_count_readback'] is False and protocol['present_used'] is False
    assert protocol['cpu_reference_output_capacity_reused'] and protocol['cpu_gpu_timings_additive'] is False
    assert protocol['frustum_planes'] == [[1, 0, 0, 1], [-1, 0, 0, 1], [0, 1, 0, 1], [0, -1, 0, 1], [0, 0, 1, 1], [0, 0, -1, 1]]
    if formal:
        assert not protocol['smoke'] and protocol['configuration'] == 'Release'
        assert protocol['repetitions'] == 3 and protocol['warmup_iterations'] == 128 and protocol['sample_iterations'] == 1000
    warmup, samples, repetitions = (protocol[k] for k in ('warmup_iterations', 'sample_iterations', 'repetitions'))
    checks = read(root / 'validation.json')
    assert len(checks) == protocol['validation_cases'] == 9
    for check in checks:
        count, rate = check['count'], check['rate']
        assert count in protocol['sizes'] and rate in (0, 1, 2)
        assert check['visible'] == (0 if rate == 0 else (count + 9) // 10 if rate == 1 else count) and check['guards']
        assert check['statistics']['result_readbacks'] == 1 and check['statistics']['result_readback_resources'] == 2
        assert check['statistics']['warnings'] == check['statistics']['errors'] == 0
        assert check['after_shutdown']['warnings'] == check['after_shutdown']['errors'] == 0
        assert check['after_shutdown']['tracked_resources'] == check['after_shutdown']['default_used_bytes'] == 0
        assert check['ticks'] == sorted(check['ticks']) and check['frequency_hz'] > 0
        assert math.isclose(check['gpu_copy_ms'], (check['ticks'][3] - check['ticks'][2]) * 1000 / check['frequency_hz'], rel_tol=1e-9, abs_tol=1e-10)
    assert {(r['count'], r['rate']) for r in checks} == {(c, r) for c in protocol['sizes'] for r in range(3)}
    names, runs = [], []
    for expected_round in range(1, repetitions + 1):
        order = [(count, rate) for count in protocol['sizes'] for rate in range(3)]
        if expected_round % 2 == 0:
            order.reverse()
        names += [f'{count}-{rate}-{expected_round}' for count, rate in order]
    assert [r['run'] for r in protocol['runs']] == names
    fields = ('cpu_reference_ms', 'cpu_frame_ms', 'cpu_prepare_ms', 'cpu_record_ms', 'cpu_submit_ms',
              'cpu_reuse_wait_ms', 'cpu_query_read_ms', 'gpu_setup_reset_ms', 'gpu_dependency_cull_ms', 'gpu_total_ms')
    for item in protocol['runs']:
        directory = root / item['run']
        assert read(directory / 'run.json') == item
        count, rate = item['count'], item['rate']
        visible = 0 if rate == 0 else (count + 9) // 10 if rate == 1 else count
        assert item['cpu_visible_checksum'] == (warmup + samples) * visible
        assert item['input_bytes'] == count * 32 and item['cpu_reference_capacity_bytes'] >= count * 4
        assert len(item['input_sha256']) == 64
        stats, initial, after = (item[k] for k in ('statistics', 'warmup_statistics', 'after_shutdown'))
        assert after['warnings'] == after['errors'] == 0
        assert stats['submissions'] == warmup + samples and stats['uav_barriers'] == (warmup + samples) * 2
        assert stats['warnings'] == stats['errors'] == stats['result_readbacks'] == stats['result_readback_resources'] == stats['pending'] == 0
        if protocol['configuration'] == 'Debug':
            assert stats['debug_info_queue']
        for key in ('default_reserved_bytes', 'default_used_bytes', 'upload_reserved_bytes', 'readback_allocation_bytes', 'tracked_resources'):
            assert stats[key] == initial[key], (item['run'], 'steady allocation changed', key)
            assert after[key] == 0, (item['run'], 'unreleased allocation', key)
        with (directory / 'frames.csv').open(encoding='utf-8', newline='') as stream:
            rows = list(csv.DictReader(stream))
        assert len(rows) == warmup + samples
        measured = []
        for index, row in enumerate(rows, 1):
            assert int(row['sequence']) == index and int(row['input_count']) == count and int(row['capacity']) == count
            assert row['phase'] == ('warmup' if index <= warmup else 'sample')
            ticks = [int(row[k]) for k in ('setup_begin', 'reset_end', 'cull_end', 'copy_end')]
            frequency = int(row['frequency_hz'])
            assert ticks == sorted(ticks) and ticks[0] > 0 and frequency > 0
            for key, start, end in (('gpu_setup_reset_ms', 0, 1), ('gpu_dependency_cull_ms', 1, 2)):
                assert math.isclose(float(row[key]), (ticks[end] - ticks[start]) * 1000 / frequency, rel_tol=1e-9, abs_tol=1e-10)
            row['gpu_total_ms'] = (ticks[2] - ticks[0]) * 1000 / frequency
            assert all(math.isfinite(float(row[key])) and float(row[key]) >= 0 for key in fields)
            if index > warmup:
                measured.append(row)
        assert len(measured) == samples
        runs.append(dict(run=item['run'], count=count, rate=rate, round=item['round'], input_sha256=item['input_sha256'],
                         metrics={key: metric([float(row[key]) for row in measured]) for key in fields}, statistics=stats,
                         cpu_reference_capacity_bytes=item['cpu_reference_capacity_bytes']))
    assert len({r['statistics']['adapter'] for r in runs}) == 1
    groups = {}
    for count in protocol['sizes']:
        for rate in range(3):
            group = [r for r in runs if r['count'] == count and r['rate'] == rate]
            assert len(group) == repetitions and len({r['input_sha256'] for r in group}) == 1
            groups[f'{count}-{rate}'] = dict(count=count, rate=rate, input_sha256=group[0]['input_sha256'],
                metrics={key: dict(mean_run_median=statistics.mean(r['metrics'][key]['median'] for r in group),
                                   stdev_run_median=statistics.stdev(r['metrics'][key]['median'] for r in group) if len(group) > 1 else 0,
                                   run_medians=[r['metrics'][key]['median'] for r in group]) for key in fields})
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
    print(f'GPU visibility protocol validated: {len(result["runs"])} runs; formal={args.formal}.')


if __name__ == '__main__':
    main()
