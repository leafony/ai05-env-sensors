// Firmware transport parser. No browser APIs: also used by the Node tests.
export const SAMPLE_COUNT = 100;
export const MAX_LINE_LENGTH = 512;
export const MAX_INPUT_LENGTH = 256 * 1024;
export const CSV_HEADER = 'index,time_ms,x_g,y_g,z_g';

const decimal = /^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$/;

function number(text, name) {
  if (typeof text !== 'string' || !decimal.test(text.trim())) {
    throw new Error(`${name}が数値ではありません`);
  }
  const value = Number(text);
  if (!Number.isFinite(value)) throw new Error(`${name}が有限の数値ではありません`);
  return value;
}

function integer(text, name, min = 0, max = 0xffffffff) {
  const value = number(text, name);
  if (!Number.isInteger(value) || value < min || value > max) {
    throw new Error(`${name}が範囲外です`);
  }
  return value;
}

function metadata(line, prefix) {
  if (!line.startsWith(prefix + ',')) throw new Error('メタデータの書式が不正です');
  const fields = Object.create(null);
  for (const field of line.slice(prefix.length + 1).split(',')) {
    const match = /^([a-z][a-z0-9_]*)=([^,\r\n]+)$/.exec(field.trim());
    if (!match || Object.hasOwn(fields, match[1])) throw new Error('メタデータが不正または重複しています');
    fields[match[1]] = match[2];
  }
  return fields;
}

function beginCapture(line) {
  const fields = metadata(line, '# capture_begin');
  const id = integer(fields.id, 'id');
  const triggerMs = integer(fields.trigger_ms, 'trigger_ms');
  const odrHz = number(fields.odr_hz, 'odr_hz');
  if (odrHz <= 0 || odrHz > 1600) throw new Error('odr_hzが範囲外です');
  const rangeG = number(fields.range_g, 'range_g');
  if (![2, 4, 8, 16].includes(rangeG)) throw new Error('range_gが不正です');
  if (integer(fields.count, 'count') !== SAMPLE_COUNT) throw new Error('100サンプルの記録ではありません');
  const source = fields.source ?? 'hardware';
  if (!['hardware', 'demo'].includes(source)) throw new Error('sourceが不正です');
  return { id, triggerMs, odrHz, rangeG, source, samples: [], hasHeader: false };
}

export class CaptureParser {
  constructor({ onCapture = () => {}, onError = () => {}, onStatus = () => {} } = {}) {
    this.onCapture = onCapture;
    this.onError = onError;
    this.onStatus = onStatus;
    this.buffer = '';
    this.active = null;
    this.discardLine = false;
    this.firstLine = true;
  }

  fail(message) {
    this.active = null;
    this.onError(message);
  }

  reset(reason = '受信を終了しました') {
    if (this.active) this.fail(`${reason}。未完了のキャプチャを破棄しました`);
    this.buffer = '';
    this.active = null;
    this.discardLine = false;
    this.firstLine = true;
  }

  feed(chunk) {
    if (typeof chunk !== 'string') throw new TypeError('feed expects text');
    if (chunk.length > MAX_INPUT_LENGTH) {
      this.reset();
      this.fail('受信データが大きすぎます');
      return;
    }
    // Keep at most one bounded line, even when a serial stream never sends LF.
    for (const character of chunk) {
      if (character === '\n') {
        if (!this.discardLine) this.line(this.buffer.replace(/\r$/, ''));
        this.buffer = '';
        this.discardLine = false;
      } else if (!this.discardLine) {
        this.buffer += character;
        if (this.buffer.length > MAX_LINE_LENGTH) {
          this.buffer = '';
          this.discardLine = true;
          this.fail('受信行が長すぎます。キャプチャを破棄しました');
        }
      }
    }
  }

  finish() {
    if (this.buffer && !this.discardLine) this.line(this.buffer.replace(/\r$/, ''));
    this.buffer = '';
    this.discardLine = false;
    if (this.active) this.fail('終端のないキャプチャです。データを破棄しました');
  }

  line(raw) {
    let line = raw;
    if (this.firstLine) {
      line = line.replace(/^\uFEFF/, '');
      this.firstLine = false;
    }
    line = line.trim();
    if (!line) return;
    try {
      if (line.startsWith('# capture_begin')) {
        if (this.active) this.fail('前のキャプチャが未完了のため破棄しました');
        this.active = beginCapture(line);
        return;
      }
      if (line.startsWith('# capture_error')) {
        const fields = metadata(line, '# capture_error');
        const id = integer(fields.id, 'id');
        const collected = integer(fields.collected, 'collected', 0, SAMPLE_COUNT);
        this.fail(`記録 #${id} は無効です: ${fields.reason ?? 'unknown'}（${collected}/100サンプル）`);
        return;
      }
      if (line.startsWith('# fatal')) {
        const fields = metadata(line, '# fatal');
        if (!fields.reason) throw new Error('fatalのreasonがありません');
        this.fail(`センサが停止しました: ${fields.reason}。電源とI2C接続を確認してResetを押してください`);
        this.onStatus({ type: 'fatal', fields });
        return;
      }
      if (line.startsWith('# armed')) {
        if (this.active) this.fail('100サンプルの受信完了前に待機へ戻りました');
        this.onStatus({ type: 'armed', fields: metadata(line, '# armed') });
        return;
      }
      if (line.startsWith('# capture_end')) {
        const fields = metadata(line, '# capture_end');
        if (!this.active) throw new Error('開始行のないキャプチャ終端です');
        if (integer(fields.id, 'id') !== this.active.id) throw new Error('開始と終端のidが一致しません');
        if (!this.active.hasHeader || this.active.samples.length !== SAMPLE_COUNT) {
          throw new Error(`100サンプルそろっていません（${this.active.samples.length}/100）`);
        }
        const { hasHeader, ...capture } = this.active;
        this.active = null;
        this.onCapture(capture);
        return;
      }
      if (line.startsWith('#')) return;
      if (!this.active) return; // Boot messages are not measurement data.
      if (!this.active.hasHeader) {
        if (line !== CSV_HEADER) throw new Error('CSVの列名が一致しません');
        this.active.hasHeader = true;
        return;
      }
      const columns = line.split(',');
      if (columns.length !== 5) throw new Error('CSVの列数が不正です');
      const index = integer(columns[0], 'index', 0, SAMPLE_COUNT - 1);
      if (index !== this.active.samples.length) throw new Error('サンプル番号に欠落・重複・順序の乱れがあります');
      const timeMs = number(columns[1], 'time_ms');
      const expectedMs = index * 1000 / this.active.odrHz;
      if (Math.abs(timeMs - expectedMs) > 0.0011) throw new Error('サンプル時刻とODRが一致しません');
      const axes = columns.slice(2).map((value, axis) => number(value, ['x_g', 'y_g', 'z_g'][axis]));
      if (axes.some((value) => Math.abs(value) > this.active.rangeG * 1.01)) {
        throw new Error('加速度が設定レンジを超えています');
      }
      this.active.samples.push({ index, timeMs, x: axes[0], y: axes[1], z: axes[2] });
    } catch (error) {
      this.fail(error.message);
    }
  }
}

export function captureToCsv(capture) {
  const begin = `# capture_begin,id=${capture.id},trigger_ms=${capture.triggerMs},odr_hz=${capture.odrHz},range_g=${capture.rangeG},count=${SAMPLE_COUNT},source=${capture.source}`;
  const rows = capture.samples.map((s) => `${s.index},${s.timeMs.toFixed(3)},${s.x.toFixed(4)},${s.y.toFixed(4)},${s.z.toFixed(4)}`);
  return [begin, CSV_HEADER, ...rows, `# capture_end,id=${capture.id}`, ''].join('\n');
}

export function parseCaptureFile(text) {
  if (text.length > MAX_INPUT_LENGTH) throw new Error('CSVは256 KiB以下にしてください');
  const captures = [];
  const errors = [];
  const parser = new CaptureParser({ onCapture: (c) => captures.push(c), onError: (e) => errors.push(e) });
  parser.feed(text);
  parser.finish();
  if (errors.length) throw new Error(errors[0]);
  if (!captures.length) throw new Error('有効なキャプチャがありません。開始・終端行を含むCSVを選んでください');
  if (captures.length > 20) throw new Error('1ファイルのキャプチャは20件までです');
  return captures;
}

export function createDemoCapture() {
  const samples = Array.from({ length: SAMPLE_COUNT }, (_, index) => {
    const decay = Math.exp(-index / 16);
    return {
      index,
      timeMs: index * 2.5,
      x: 2.8 * decay * Math.sin(index * 0.75),
      y: 1.6 * decay * Math.sin(index * 0.58 + 0.3),
      z: 1 + 4.5 * decay * Math.sin(index * 0.9),
    };
  });
  return { id: 1, triggerMs: 0, odrHz: 400, rangeG: 8, source: 'demo', samples };
}

export function magnitude(sample) {
  return Math.hypot(sample.x, sample.y, sample.z);
}
