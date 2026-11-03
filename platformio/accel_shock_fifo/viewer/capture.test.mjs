import test from 'node:test';
import assert from 'node:assert/strict';
import { CaptureParser, captureToCsv, parseCaptureFile, createDemoCapture, MAX_LINE_LENGTH, MAX_INPUT_LENGTH } from './capture.mjs';

const valid = () => captureToCsv({ ...createDemoCapture(), id: 7, triggerMs: 1234, source: 'hardware' });
const sampleRows = () => valid().trimEnd().split('\n');
function stream(text, chunkSize = 13) {
  const captures = [], errors = [], statuses = [];
  const parser = new CaptureParser({ onCapture: (c) => captures.push(c), onError: (e) => errors.push(e), onStatus: (s) => statuses.push(s) });
  for (let i = 0; i < text.length; i += chunkSize) parser.feed(text.slice(i, i + chunkSize));
  parser.finish();
  return { parser, captures, errors, statuses };
}

test('100 samples survive arbitrary serial chunks, CRLF, BOM and surrounding logs', () => {
  const result = stream('\uFEFF' + valid().replaceAll('\n', '\r\n') + '# armed,odr_hz=400,range_g=8,threshold_g=0.500,count=100\n');
  assert.equal(result.errors.length, 0);
  assert.equal(result.captures.length, 1);
  assert.equal(result.captures[0].samples.length, 100);
  assert.equal(result.captures[0].samples[99].timeMs, 247.5);
  assert.equal(result.statuses[0].type, 'armed');
});

test('firmware can omit source metadata; unknown metadata is harmless', () => {
  const [capture] = parseCaptureFile(valid().replace(',source=hardware', ',version=1'));
  assert.equal(capture.source, 'hardware');
});

test('CSV round trip preserves demo provenance and all sample values', () => {
  const [first] = parseCaptureFile(captureToCsv(createDemoCapture()));
  const [second] = parseCaptureFile(captureToCsv(first));
  assert.deepEqual(second, first);
  assert.equal(second.source, 'demo');
});

test('EOF without LF still completes a valid frame', () => {
  assert.equal(parseCaptureFile(valid().trimEnd()).length, 1);
});

test('dropped, duplicate, reordered, surplus and short records are rejected', () => {
  const rows = sampleRows();
  const corruptions = [
    [...rows.slice(0, 17), ...rows.slice(18)],
    [...rows.slice(0, 17), rows[16], ...rows.slice(17)],
    [...rows.slice(0, 17), rows[18], rows[17], ...rows.slice(19)],
    [...rows.slice(0, -1), rows.at(-2), rows.at(-1)],
    [...rows.slice(0, -3), rows.at(-1)],
  ];
  for (const corruption of corruptions) assert.throws(() => parseCaptureFile(corruption.join('\n')));
});

test('missing and mismatched end markers cannot publish a capture', () => {
  assert.throws(() => parseCaptureFile(valid().replace('# capture_end,id=7', '')));
  assert.throws(() => parseCaptureFile(valid().replace('# capture_end,id=7', '# capture_end,id=8')));
  assert.throws(() => parseCaptureFile('# capture_end,id=7\n'));
});

test('invalid metadata cannot silently alter count, units or source', () => {
  for (const [before, after] of [['count=100', 'count=99'], ['odr_hz=400', 'odr_hz=0'], ['range_g=8', 'range_g=3'], ['trigger_ms=1234', 'trigger_ms=NaN'], ['id=7,', 'id=7,id=7,'], ['source=hardware', 'source=unknown']]) {
    assert.throws(() => parseCaptureFile(valid().replace(before, after)));
  }
});

test('nonfinite, empty, out-of-range values and wrong timestamps are rejected', () => {
  for (const field of ['NaN', 'Infinity', '', '9.0000', '1e999', '=1+1']) {
    const rows = sampleRows();
    const columns = rows[2].split(','); columns[2] = field; rows[2] = columns.join(',');
    assert.throws(() => parseCaptureFile(rows.join('\n')));
  }
  assert.throws(() => parseCaptureFile(valid().replace('1,2.500,', '1,3.000,')));
});

test('capture_error invalidates a partial frame and next good frame can recover', () => {
  const input = sampleRows().slice(0, 30).join('\n') + '\n# capture_error,id=7,reason=fifo_overrun,collected=28\n' + valid();
  const result = stream(input);
  assert.equal(result.captures.length, 1);
  assert.match(result.errors[0], /fifo_overrun/);
});

test('firmware fatal error is visible, invalidates pending data and reports stopped state', () => {
  const input = sampleRows().slice(0, 30).join('\n') + '\n# fatal,reason=cleanup_failed\n';
  const result = stream(input);
  assert.equal(result.captures.length, 0);
  assert.match(result.errors[0], /cleanup_failed/);
  assert.equal(result.statuses[0].type, 'fatal');
});

test('delayed FIFO service is reported without publishing a measurement', () => {
  const result = stream('# capture_error,id=7,reason=fifo_service_delayed,collected=42\n');
  assert.equal(result.captures.length, 0);
  assert.match(result.errors[0], /fifo_service_delayed/);
  assert.match(result.errors[0], /42\/100/);
});

test('new begin and armed interrupting an incomplete frame report an error', () => {
  const partial = sampleRows().slice(0, 5).join('\n') + '\n';
  for (const text of [partial + valid(), partial + '# armed,odr_hz=400\n' + valid()]) {
    const result = stream(text);
    assert.equal(result.captures.length, 1);
    assert.equal(result.errors.length, 1);
  }
});

test('disconnect drops partial state; its continuation cannot complete a capture', () => {
  const captures = [], errors = [];
  const parser = new CaptureParser({ onCapture: (c) => captures.push(c), onError: (e) => errors.push(e) });
  const rows = sampleRows();
  parser.feed(rows.slice(0, 50).join('\n') + '\n');
  parser.reset('切断');
  parser.feed(rows.slice(50).join('\n') + '\n');
  assert.equal(captures.length, 0);
  assert.match(errors[0], /未完了/);
  parser.feed(valid());
  assert.equal(captures.length, 1);
});

test('oversized input/lines are bounded, rejected and recover at the next frame', () => {
  const result = stream('x'.repeat(MAX_LINE_LENGTH + 50) + '\n' + valid(), 33);
  assert.equal(result.captures.length, 1);
  assert.equal(result.errors.length, 1);
  assert.throws(() => parseCaptureFile('x'.repeat(MAX_INPUT_LENGTH + 1)));
});

test('a CSV file with a valid frame followed by an invalid one is rejected as a whole', () => {
  assert.throws(() => parseCaptureFile(valid() + valid().replace('count=100', 'count=101')));
  assert.throws(() => parseCaptureFile(valid().repeat(21)));
});
