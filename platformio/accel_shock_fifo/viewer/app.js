import { CaptureParser, captureToCsv, parseCaptureFile, createDemoCapture, magnitude, MAX_INPUT_LENGTH } from './capture.mjs';

const byId = (id) => document.getElementById(id);
const ui = Object.fromEntries(['connect', 'disconnect', 'open-file', 'file-input', 'demo', 'save', 'connection-status', 'support', 'error', 'capture-select', 'source-banner', 'sample-info', 'peak-info', 'range-info', 'chart', 'empty-chart', 'sample-index', 'sample-value', 'capture-info', 'clipping', 'data-rows'].map((id) => [id, byId(id)]));
const colors = { x: '#d34848', y: '#198363', z: '#286fd1', magnitude: '#6d46a8' };
let captures = [];
let selected = null;
let nextKey = 1;
let port = null;
let reader = null;
let readTask = null;
let connecting = false;
let stopping = false;
let chartGeometry = null;

function error(message) {
  ui.error.textContent = message;
  ui.error.hidden = false;
}

function clearError() {
  ui.error.hidden = true;
  ui.error.textContent = '';
}

function connectionStatus(message) {
  ui['connection-status'].textContent = message;
  ui['connection-status'].classList.toggle('connected', Boolean(port));
}

function connectionButtons() {
  ui.connect.disabled = !('serial' in navigator) || connecting || Boolean(port);
  ui.disconnect.disabled = !port || stopping;
}

const parser = new CaptureParser({
  onCapture: (capture) => addCapture(capture, 'serial'),
  onError: (message) => error(message),
  onStatus: ({ type }) => {
    if (type === 'armed' && port) connectionStatus('接続済み・衝撃を待っています');
    if (type === 'fatal' && port) connectionStatus('接続済み・センサが停止しています');
  },
});

function addCapture(capture, origin) {
  clearError();
  captures.unshift({ ...capture, origin, key: nextKey++ });
  captures = captures.slice(0, 20);
  selected = captures[0];
  refreshSelect();
  renderCapture();
}

function refreshSelect() {
  ui['capture-select'].replaceChildren();
  for (const capture of captures) {
    const option = document.createElement('option');
    option.value = String(capture.key);
    const kind = capture.source === 'demo' ? 'デモ' : capture.origin === 'serial' ? 'シリアル' : 'CSV';
    option.textContent = `#${capture.id} · ${kind} · ${capture.triggerMs} ms`;
    option.selected = capture.key === selected?.key;
    ui['capture-select'].append(option);
  }
  ui['capture-select'].disabled = captures.length === 0;
}

function renderCapture() {
  const capture = selected;
  ui.save.disabled = !capture;
  ui['sample-index'].disabled = !capture;
  ui['empty-chart'].hidden = Boolean(capture);
  if (!capture) { drawChart(); return; }
  ui['sample-index'].value = '0';
  ui['source-banner'].hidden = false;
  ui['source-banner'].classList.toggle('demo', capture.source === 'demo');
  ui['source-banner'].textContent = capture.source === 'demo'
    ? 'デモ・実機データではありません。表示を試すために生成した波形です。'
    : capture.origin === 'serial' ? 'シリアルから100サンプルを受信しました。' : 'CSVから100サンプルを読み込みました。';
  ui['sample-info'].textContent = `${capture.samples.length}点 / ${capture.odrHz} Hz`;
  ui['peak-info'].textContent = `${Math.max(...capture.samples.map(magnitude)).toFixed(3)} g`;
  ui['range-info'].textContent = `±${capture.rangeG} g`;
  ui['capture-info'].textContent = capture.source === 'demo'
    ? 'デモ波形です。割り込み受信時刻は実機の時刻を表しません。'
    : `割り込み受信時刻: 起動後 ${capture.triggerMs} ms。横軸の時刻とは基準が異なります。`;
  const clipping = capture.samples.filter((s) => [s.x, s.y, s.z].some((v) => Math.abs(v) >= capture.rangeG * 0.99)).length;
  ui.clipping.hidden = clipping === 0;
  ui.clipping.textContent = `${clipping}サンプルにレンジ上限付近の値があります。飽和してピークを測れていない可能性があります。`;
  ui['data-rows'].replaceChildren();
  for (const sample of capture.samples) {
    const row = document.createElement('tr');
    for (const value of [sample.index, sample.timeMs.toFixed(3), sample.x.toFixed(4), sample.y.toFixed(4), sample.z.toFixed(4), magnitude(sample).toFixed(4)]) {
      const cell = document.createElement('td');
      cell.textContent = String(value);
      row.append(cell);
    }
    ui['data-rows'].append(row);
  }
  updateSample();
}

function updateSample() {
  if (selected) {
    const s = selected.samples[Number(ui['sample-index'].value)];
    ui['sample-value'].textContent = `#${s.index} · ${s.timeMs.toFixed(3)} ms　X ${s.x.toFixed(3)} g　Y ${s.y.toFixed(3)} g　Z ${s.z.toFixed(3)} g　合成 ${magnitude(s).toFixed(3)} g`;
  }
  drawChart();
}

function selectedSeries() {
  return [...document.querySelectorAll('input[name="series"]:checked')].map((input) => input.value);
}

function niceStep(span) {
  const raw = Math.max(span, 0.1) / 6;
  const power = 10 ** Math.floor(Math.log10(raw));
  return [1, 2, 5, 10].find((n) => n * power >= raw) * power;
}

function drawChart() {
  const canvas = ui.chart;
  const bounds = canvas.getBoundingClientRect();
  const width = Math.max(250, bounds.width);
  const height = Math.max(240, bounds.height);
  const scale = window.devicePixelRatio || 1;
  canvas.width = Math.round(width * scale);
  canvas.height = Math.round(height * scale);
  const context = canvas.getContext('2d');
  context.scale(scale, scale);
  const left = 56, right = width - 18, top = 20, bottom = height - 44;
  const series = selectedSeries();
  const valueOf = (sample, field) => field === 'magnitude' ? magnitude(sample) : sample[field];
  const values = selected ? selected.samples.flatMap((s) => series.map((field) => valueOf(s, field))) : [];
  const min = Math.min(0, ...values), max = Math.max(1, ...values);
  const step = niceStep(max - min);
  const low = Math.floor(min / step) * step;
  const high = Math.ceil(max / step) * step;
  const duration = selected?.samples.at(-1).timeMs || 247.5;
  const x = (time) => left + time / duration * (right - left);
  const y = (value) => bottom - (value - low) / (high - low) * (bottom - top);
  chartGeometry = { left, right, width };
  context.font = '11px system-ui, sans-serif';
  context.lineWidth = 1;
  for (let value = low; value <= high + step * 0.1; value += step) {
    const py = y(value);
    context.strokeStyle = Math.abs(value) < step / 100 ? '#bac8d8' : '#e6edf4';
    context.beginPath(); context.moveTo(left, py); context.lineTo(right, py); context.stroke();
    context.fillStyle = '#6b7e93'; context.textAlign = 'right';
    context.fillText(value.toFixed(step < 1 ? 1 : 0), left - 10, py + 4);
  }
  for (let i = 0; i <= 4; i++) {
    const time = duration * i / 4, px = x(time);
    context.strokeStyle = '#e6edf4';
    context.beginPath(); context.moveTo(px, top); context.lineTo(px, bottom); context.stroke();
    context.fillStyle = '#6b7e93'; context.textAlign = 'center';
    context.fillText(time.toFixed(1), px, bottom + 19);
  }
  context.textAlign = 'left'; context.fillStyle = '#52677d'; context.fillText('g', 14, top + 3);
  context.textAlign = 'right'; context.fillText('時間 (ms)', right, height - 4);
  if (!selected) return;
  context.save();
  context.beginPath(); context.rect(left, top, right - left, bottom - top); context.clip();
  for (const field of series) {
    context.beginPath(); context.strokeStyle = colors[field]; context.lineWidth = field === 'magnitude' ? 2.1 : 1.6;
    selected.samples.forEach((sample, index) => {
      if (!index) context.moveTo(x(sample.timeMs), y(valueOf(sample, field)));
      else context.lineTo(x(sample.timeMs), y(valueOf(sample, field)));
    });
    context.stroke();
  }
  const sample = selected.samples[Number(ui['sample-index'].value)];
  context.strokeStyle = '#64768c'; context.lineWidth = 1; context.setLineDash([3, 4]);
  context.beginPath(); context.moveTo(x(sample.timeMs), top); context.lineTo(x(sample.timeMs), bottom); context.stroke();
  context.setLineDash([]);
  for (const field of series) {
    context.beginPath(); context.fillStyle = colors[field]; context.arc(x(sample.timeMs), y(valueOf(sample, field)), 3, 0, 2 * Math.PI); context.fill();
  }
  context.restore();
}

async function readSerial(activePort) {
  const decoder = new TextDecoder();
  try {
    reader = activePort.readable.getReader();
    while (!stopping) {
      const { value, done } = await reader.read();
      if (done) break;
      if (value) parser.feed(decoder.decode(value, { stream: true }));
    }
  } catch (failure) {
    if (!stopping) error(`シリアル受信が終了しました: ${failure.message}`);
  } finally {
    parser.reset('接続が切れました');
    try { reader?.releaseLock(); } catch { /* Reader may already be released after disconnect. */ }
    reader = null;
    try { await activePort.close(); } catch { /* Unplugged ports can already be closed. */ }
    if (port === activePort) port = null;
    stopping = false;
    connectionButtons();
    connectionStatus('未接続・表示中の記録はCSVで保存できます');
  }
}

async function connect() {
  if (port || connecting) return;
  connecting = true;
  clearError();
  connectionButtons();
  let requestedPort;
  let opened = false;
  try {
    requestedPort = await navigator.serial.requestPort();
    await requestedPort.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none', flowControl: 'none', bufferSize: 8192 });
    opened = true;
    // Do not deliberately reset the board or enter its bootloader.
    await requestedPort.setSignals({ dataTerminalReady: false, requestToSend: false });
    parser.reset();
    port = requestedPort;
    stopping = false;
    connectionStatus('接続済み・データ待ち（Run側にしてResetを押してください）');
    readTask = readSerial(port);
  } catch (failure) {
    if (opened) { try { await requestedPort.close(); } catch { /* Report the original failure. */ } }
    if (failure.name === 'NotFoundError') connectionStatus('接続するポートが選択されませんでした');
    else { error(`接続できませんでした: ${failure.message}`); connectionStatus('未接続'); }
  } finally {
    connecting = false;
    connectionButtons();
  }
}

async function disconnect() {
  if (!port || stopping) return;
  stopping = true;
  connectionButtons();
  connectionStatus('接続を解除しています…');
  try { await reader?.cancel(); } catch { /* readSerial handles unplugging. */ }
  await readTask;
}

ui.connect.addEventListener('click', connect);
ui.disconnect.addEventListener('click', disconnect);
ui['open-file'].addEventListener('click', () => ui['file-input'].click());
ui['file-input'].addEventListener('change', async () => {
  const file = ui['file-input'].files[0];
  ui['file-input'].value = '';
  if (!file) return;
  clearError();
  try {
    if (file.size > MAX_INPUT_LENGTH) throw new Error('CSVは256 KiB以下にしてください');
    const imported = parseCaptureFile(await file.text());
    for (const capture of imported) addCapture(capture, 'file');
  } catch (failure) { error(`CSVを開けませんでした: ${failure.message}`); }
});
ui.demo.addEventListener('click', () => {
  clearError();
  addCapture(createDemoCapture(), 'demo');
});
ui.save.addEventListener('click', () => {
  if (!selected) return;
  const blob = new Blob([captureToCsv(selected)], { type: 'text/csv;charset=utf-8' });
  const url = URL.createObjectURL(blob);
  const link = document.createElement('a');
  link.href = url;
  link.download = `${selected.source === 'demo' ? 'demo-' : ''}accel-shock-${selected.id}.csv`;
  link.click();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
});
ui['capture-select'].addEventListener('change', () => {
  selected = captures.find((capture) => capture.key === Number(ui['capture-select'].value));
  renderCapture();
});
ui['sample-index'].addEventListener('input', updateSample);
document.querySelectorAll('input[name="series"]').forEach((input) => input.addEventListener('change', drawChart));
ui.chart.addEventListener('pointermove', (event) => {
  if (!selected || !chartGeometry) return;
  const { left, right } = chartGeometry;
  const px = event.clientX - ui.chart.getBoundingClientRect().left;
  ui['sample-index'].value = String(Math.max(0, Math.min(99, Math.round((px - left) / (right - left) * 99))));
  updateSample();
});
new ResizeObserver(drawChart).observe(ui.chart.parentElement);
if (!('serial' in navigator)) {
  ui.support.textContent = 'このブラウザーではシリアル接続を利用できません。PC版Chrome / Edgeでlocalhostから開いてください。CSVとデモはこのまま利用できます。';
}
window.addEventListener('beforeunload', (event) => {
  if (port) { event.preventDefault(); event.returnValue = ''; }
});
connectionButtons();
drawChart();
