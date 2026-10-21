type Json = Record<string, unknown>;
type ConnState = 'connecting' | 'online' | 'offline';
type CanType = 'Unknown' | 'NMT' | 'SYNC' | 'EMCY' | 'TIME' | 'TPDO1' | 'RPDO1' | 'TPDO2' | 'RPDO2' | 'TPDO3' | 'RPDO3' | 'TPDO4' | 'RPDO4' | 'SDO Tx' | 'SDO Rx' | 'HB/Guard' | 'LSS Tx' | 'LSS Rx';
interface Frame { timestamp: number; id: number; extended: boolean; remote: boolean; dlc: number; data: Uint8Array; type: CanType; node?: number; sdo?: string; sequence: number }
interface DeviceState { firmware: string; uptime_ms: number; network: { initialized: boolean; link_up: boolean; source: string; ip: string }; can: { state: string; bitrate_kbps: number; received: number }; storage: { state: string; mode: string; file: string } }
interface Statistics { can: { received: number; ring_overflows: number; gui_overflows: number; errors: number; bus_load_permille: number }; analyzer: { frames: number; unique_nodes: number; queue_overflows: number }; logger: { stored_frames: number; dropped_frames: number; write_errors: number; recoveries: number }; websocket: { active: boolean; connections: number; batches: number; frames: number; lost_frames: number } }
interface NodeInfo { node_id: number; last_type: string; first_seen_us: number; last_seen_us: number; frames: number; bytes: number; sdo_requests: number; sdo_responses: number; sdo_started: number; sdo_completed: number; sdo_aborted: number; sdo_timeouts: number; sdo_conflicts: number; sdo_orphans: number; sdo_protocol_errors: number; sdo_errors: number }
interface NodeEvent { timestamp_us: number; id: number; type: string; sdo_status: string; index: number; subindex: number; response_time_us: number; transaction_time_us: number; transferred_bytes: number; abort_code: number; extended: boolean; remote: boolean; dlc: number; has_response_time: boolean; has_sdo_address: boolean; data_offset: number; data: string }
interface LoggerConfig { mode: 'full' | 'selected'; selected_nodes: number[]; recording: boolean; active_mode: 'off' | 'full' | 'selected'; card_state?: string; file?: string; file_size_bytes?: number; free_bytes?: number; stored_frames?: number; dropped_frames?: number; write_errors?: number; recoveries?: number; last_error?: number; selected_node_count?: number }
interface NetworkConfig { dhcp_enabled: boolean; static_ip: string; netmask: string; gateway: string; http_port: number; dhcp_timeout_ms: number }
interface CanConfig { bitrate_kbps: number; auto_bitrate: boolean; deep_sdo_enabled: boolean }
interface LogInfo { path: string; session_id: number; size_bytes: number; download_url: string }

const $ = <T extends HTMLElement>(id: string): T => {
  const value = document.getElementById(id);
  if (!value) throw new Error(`Missing #${id}`);
  return value as T;
};
const text = (id: string, value: string): void => { $(id).textContent = value; };
const types: CanType[] = ['Unknown','NMT','SYNC','EMCY','TIME','TPDO1','RPDO1','TPDO2','RPDO2','TPDO3','RPDO3','TPDO4','RPDO4','SDO Tx','SDO Rx','HB/Guard','LSS Tx','LSS Rx'];
const frameBuffer: Frame[] = [];
let pendingFrames: Frame[] = [];
let filteredFrames: Frame[] = [];
let nodes: NodeInfo[] = [];
let selectedNode: number | undefined;
let loggerConfig: LoggerConfig = { mode: 'full', selected_nodes: [], recording: false, active_mode: 'off' };
let analyzerFrames = 0;
let deepSdoEnabled = true;
let paused = false;
let filterDirty = true;
let sortKey: 'timestamp'|'id'|'type'|'node' = 'timestamp';
let sortAsc = false;
type NodeSort = keyof NodeInfo | 'traffic' | 'sdo_success' | 'errors';
let nodeSort: NodeSort = 'node_id';
let nodeSortAsc = true;
let nodeView: 'events'|'objects' = 'events';
let expectedSequence: number | undefined;
let missedFrames = 0;
let gapBlocks = 0;
let reconnects = 0;
let ws: WebSocket | undefined;
let reconnectTimer = 0;
let retryMs = 500;
let flushTimer = 0;
let toastTimer = 0;
let lastRateFrames = 0;
let lastRateTime = performance.now();
let liveRate = 0;
let totalLiveFrames = 0;

const escapeHtml = (value: unknown): string => String(value).replace(/[&<>'"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;',"'":'&#39;','"':'&quot;'}[c] || c));
const hex = (value: number, width: number): string => value.toString(16).toUpperCase().padStart(width, '0');
const formatInt = (value: number): string => new Intl.NumberFormat('ru-RU').format(value || 0);
const formatBytes = (value: number): string => value >= 1073741824 ? `${(value/1073741824).toFixed(1)} ГиБ` : value >= 1048576 ? `${(value/1048576).toFixed(1)} МиБ` : value >= 1024 ? `${(value/1024).toFixed(1)} КиБ` : `${value} Б`;
const formatTime = (microseconds: number): string => `${Math.floor(microseconds / 1_000_000)}.${String(Math.floor((microseconds % 1_000_000) / 10)).padStart(5,'0')} с`;
const modeName = (mode: string): string => ({ off: 'Выключена', full: 'Все кадры', selected: 'Выбранные ноды' }[mode] || mode);
const statusName = (status: string): string => ({ none: '—', waiting: 'ожидание', completed: 'завершено', aborted: 'отклонено', timeout: 'таймаут', ambiguous: 'неоднозначно', protocol_error: 'ошибка протокола', orphan: 'ответ без запроса' }[status] || status);
const nodeErrorCount = (node: NodeInfo): number => node.sdo_aborted + node.sdo_timeouts + node.sdo_protocol_errors + node.sdo_conflicts + node.sdo_orphans;
const nodeTraffic = (node: NodeInfo): number => analyzerFrames ? node.frames * 100 / analyzerFrames : 0;
const nodeSdoSuccess = (node: NodeInfo): number | undefined => {
  if (deepSdoEnabled) {
    const finished = node.sdo_completed + node.sdo_aborted + node.sdo_timeouts + node.sdo_protocol_errors;
    return finished ? node.sdo_completed * 100 / finished : undefined;
  }
  return node.sdo_requests ? Math.min(100, node.sdo_responses * 100 / node.sdo_requests) : undefined;
};
const spacedHex = (value: string, offsetBytes = 0): string => (value.slice(offsetBytes * 2).match(/.{1,2}/g) || []).join(' ');

function notify(message: string, error = false): void {
  const toast = $('toast');
  toast.textContent = message;
  toast.className = `show${error ? ' error' : ''}`;
  clearTimeout(toastTimer);
  toastTimer = window.setTimeout(() => toast.className = '', 3200);
}

async function api<T>(path: string, init?: RequestInit): Promise<T> {
  const response = await fetch(path, { cache: 'no-store', ...init, headers: { 'Content-Type': 'application/json', ...(init?.headers || {}) } });
  const body = await response.json().catch(() => ({})) as Json;
  if (!response.ok) throw new Error(String(body.message || body.error || `HTTP ${response.status}`));
  return body as T;
}

function setConnection(state: ConnState, label: string): void {
  $('conn-dot').className = `dot ${state === 'online' ? 'online' : state === 'offline' ? 'offline' : ''}`;
  text('conn-label', label);
}

function decodeFrame(frame: Omit<Frame,'type'|'node'|'sdo'>): Frame {
  let type: CanType = 'Unknown';
  let node: number | undefined;
  const id = frame.id;
  if (!frame.extended) {
    if (id === 0) { type = 'NMT'; if (frame.dlc >= 2 && frame.data[1] && frame.data[1] <= 127) node = frame.data[1]; }
    else if (id === 0x080) type = 'SYNC';
    else if (id === 0x100) type = 'TIME';
    else if (id === 0x7e4) type = 'LSS Tx';
    else if (id === 0x7e5) type = 'LSS Rx';
    else {
      const ranges: Array<[number, CanType]> = [[0x080,'EMCY'],[0x180,'TPDO1'],[0x200,'RPDO1'],[0x280,'TPDO2'],[0x300,'RPDO2'],[0x380,'TPDO3'],[0x400,'RPDO3'],[0x480,'TPDO4'],[0x500,'RPDO4'],[0x580,'SDO Tx'],[0x600,'SDO Rx'],[0x700,'HB/Guard']];
      for (const [base, candidateType] of ranges) {
        const candidateNode = id - base;
        if (candidateNode >= 1 && candidateNode <= 127) { type = candidateType; node = candidateNode; break; }
      }
    }
  }
  let sdo: string | undefined;
  const decodedType = String(type) as CanType;
  if ((decodedType === 'SDO Tx' || decodedType === 'SDO Rx') && frame.dlc >= 4 && !frame.remote) {
    const command = frame.data[0] || 0;
    const indexed = decodedType === 'SDO Rx'
      ? ((command >= 0x20 && command <= 0x3f) || command === 0x40 || command === 0x80 || (command >> 5) === 5 || (command >> 5) === 6)
      : ((command >= 0x40 && command <= 0x5f) || command === 0x60 || command === 0x80 || (command >> 5) === 5 || (command >> 5) === 6);
    if (indexed) sdo = `0x${hex((frame.data[1] || 0) | ((frame.data[2] || 0) << 8),4)}:${hex(frame.data[3] || 0,2)}`;
  }
  return { ...frame, type, node, sdo };
}

function parseBatch(buffer: ArrayBuffer): Frame[] {
  if (buffer.byteLength < 16) throw new Error('Короткий WebSocket пакет');
  const view = new DataView(buffer);
  if (view.getUint8(0) !== 0x43 || view.getUint8(1) !== 0x53 || view.getUint8(2) !== 0x57 || view.getUint8(3) !== 0x53) throw new Error('Неизвестный WebSocket magic');
  if (view.getUint8(4) !== 1) throw new Error(`Версия WebSocket ${view.getUint8(4)} не поддерживается`);
  const count = view.getUint16(6, true);
  const sequence = view.getUint32(8, true);
  const reportedLost = view.getUint32(12, true);
  if (buffer.byteLength !== 16 + count * 24) throw new Error('Повреждён размер WebSocket пакета');
  if (expectedSequence !== undefined && sequence !== expectedSequence) {
    const sequenceGap = (sequence - expectedSequence) >>> 0;
    missedFrames += Math.max(sequenceGap, reportedLost);
    gapBlocks++;
  } else if (reportedLost > 0) {
    missedFrames += reportedLost;
    gapBlocks++;
  }
  expectedSequence = (sequence + count) >>> 0;
  const result: Frame[] = [];
  for (let index = 0; index < count; index++) {
    const offset = 16 + index * 24;
    const low = view.getUint32(offset, true);
    const high = view.getUint32(offset + 4, true);
    const timestamp = high * 4294967296 + low;
    const flags = view.getUint8(offset + 12);
    const dlc = Math.min(view.getUint8(offset + 13), 8);
    const data = new Uint8Array(dlc);
    for (let byte = 0; byte < dlc; byte++) data[byte] = view.getUint8(offset + 16 + byte);
    result.push(decodeFrame({ timestamp, id: view.getUint32(offset + 8, true), extended: (flags & 1) !== 0, remote: (flags & 2) !== 0, dlc, data, sequence: (sequence + index) >>> 0 }));
  }
  text('gap-count', formatInt(missedFrames));
  text('gap-blocks', formatInt(gapBlocks));
  return result;
}

function connectStream(): void {
  clearTimeout(reconnectTimer);
  setConnection('connecting', reconnects ? 'Переподключение…' : 'Подключение…');
  const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
  ws = new WebSocket(`${protocol}//${location.host}/api/v1/stream`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { setConnection('online','Live'); retryMs = 500; };
  ws.onmessage = event => {
    if (typeof event.data === 'string') {
      try {
        const hello = JSON.parse(event.data) as { sequence?: number };
        if (typeof hello.sequence === 'number' && expectedSequence !== undefined && hello.sequence !== expectedSequence) {
          missedFrames += (hello.sequence - expectedSequence) >>> 0; gapBlocks++;
        }
        if (typeof hello.sequence === 'number') expectedSequence = hello.sequence;
      } catch { /* hello is informational */ }
      return;
    }
    try {
      const batch = parseBatch(event.data as ArrayBuffer);
      if (!paused) pendingFrames.push(...batch);
    } catch (error) { notify(error instanceof Error ? error.message : 'Ошибка live stream', true); ws?.close(); }
  };
  ws.onclose = () => {
    reconnects++;
    setConnection('offline', `Нет связи · попытка ${reconnects}`);
    reconnectTimer = window.setTimeout(connectStream, retryMs);
    retryMs = Math.min(retryMs * 2, 10_000);
  };
  ws.onerror = () => ws?.close();
}

function applyFrameFilters(): void {
  const search = ($<HTMLInputElement>('frame-search').value || '').trim().toLowerCase();
  const typeFilter = $<HTMLSelectElement>('type-filter').value;
  const nodeValue = Number($<HTMLInputElement>('node-filter').value) || 0;
  filteredFrames = frameBuffer.filter(frame => {
    if (typeFilter && frame.type !== typeFilter) return false;
    if (nodeValue && frame.node !== nodeValue) return false;
    if (!search) return true;
    const data = Array.from(frame.data, byte => hex(byte,2)).join(' ');
    return `0x${hex(frame.id, frame.extended ? 8 : 3)} ${frame.type} ${frame.node || ''} ${frame.sdo || ''} ${data}`.toLowerCase().includes(search);
  });
  const direction = sortAsc ? 1 : -1;
  filteredFrames.sort((left, right) => {
    const a = sortKey === 'timestamp' ? left.timestamp : sortKey === 'id' ? left.id : sortKey === 'node' ? (left.node || 0) : left.type;
    const b = sortKey === 'timestamp' ? right.timestamp : sortKey === 'id' ? right.id : sortKey === 'node' ? (right.node || 0) : right.type;
    return (typeof a === 'string' ? a.localeCompare(String(b)) : a - Number(b)) * direction;
  });
  filterDirty = false;
  text('visible-count', `${formatInt(filteredFrames.length)} кадров`);
  $('frame-spacer').style.height = `${filteredFrames.length * 34}px`;
}

function renderVisibleFrames(): void {
  if (filterDirty) applyFrameFilters();
  const viewport = $('frame-viewport');
  const start = Math.max(0, Math.floor(viewport.scrollTop / 34) - 4);
  const count = Math.ceil(viewport.clientHeight / 34) + 8;
  const end = Math.min(filteredFrames.length, start + count);
  let html = '';
  for (let index = start; index < end; index++) {
    const frame = filteredFrames[index];
    if (!frame) continue;
    const bytes = Array.from(frame.data, byte => hex(byte,2)).join(' ');
    html += `<div class="frame-row frame-grid" style="transform:translateY(${index * 34}px)"><span>${formatTime(frame.timestamp)}</span><span>0x${hex(frame.id,frame.extended?8:3)}</span><span>${frame.extended ? 'EXT' : 'STD'}</span><span class="type">${frame.type}</span><span>${frame.node ?? '—'}</span><span class="sdo">${frame.sdo || '—'}</span><span>${frame.dlc}</span><span class="data">${bytes || (frame.remote ? 'RTR' : '—')}</span></div>`;
  }
  $('frame-rows').innerHTML = html;
}

function flushFrames(): void {
  if (pendingFrames.length) {
    totalLiveFrames += pendingFrames.length;
    frameBuffer.push(...pendingFrames);
    pendingFrames = [];
    if (frameBuffer.length > 5000) frameBuffer.splice(0, frameBuffer.length - 5000);
    filterDirty = true;
  }
  const now = performance.now();
  if (now - lastRateTime >= 1000) {
    liveRate = Math.round((totalLiveFrames - lastRateFrames) * 1000 / Math.max(now - lastRateTime, 1));
    lastRateFrames = totalLiveFrames;
    lastRateTime = now;
    text('live-rate', `${formatInt(liveRate)} кадр/с`);
  }
  renderVisibleFrames();
  flushTimer = window.setTimeout(flushFrames, 100);
}

async function refreshStatus(): Promise<void> {
  try {
    const [state, stats, logger] = await Promise.all([api<DeviceState>('/api/v1/state'), api<Statistics>('/api/v1/statistics'), api<LoggerConfig>('/api/v1/config/logger')]);
    loggerConfig = logger;
    renderLoggerControls();
    text('firmware', `FW ${state.firmware}`);
    text('can-rate', state.can.bitrate_kbps ? `${state.can.bitrate_kbps} кбит/с` : 'Авто');
    text('can-state', state.can.state);
    text('bus-load', `${(stats.can.bus_load_permille / 10).toFixed(1)} %`);
    text('traffic-total', `${formatInt(stats.can.received)} кадров`);
    text('can-drops', formatInt(stats.can.ring_overflows + stats.can.gui_overflows + stats.analyzer.queue_overflows));
    text('can-errors', formatInt(stats.can.errors));
    text('log-mode', modeName(state.storage.mode));
    text('log-file', state.storage.file || state.storage.state);
    text('log-free', formatBytes(logger.free_bytes || 0));
  } catch (error) {
    if (!ws || ws.readyState !== WebSocket.OPEN) setConnection('offline','Устройство недоступно');
  } finally { window.setTimeout(refreshStatus, 2000); }
}

function renderNodes(): void {
  const sorted = [...nodes].sort((a,b) => {
    const value = (node: NodeInfo): string | number => nodeSort === 'traffic' ? nodeTraffic(node) : nodeSort === 'sdo_success' ? (nodeSdoSuccess(node) ?? -1) : nodeSort === 'errors' ? nodeErrorCount(node) : node[nodeSort];
    const left = value(a), right = value(b);
    return (typeof left === 'string' ? left.localeCompare(String(right)) : Number(left)-Number(right)) * (nodeSortAsc ? 1 : -1);
  });
  text('node-count', String(nodes.length));
  const list = $('node-list');
  if (!sorted.length) { list.className = 'node-list empty-state'; list.textContent = 'Ноды пока не обнаружены'; return; }
  list.className = 'node-list';
  list.innerHTML = sorted.map(node => {
    const success = nodeSdoSuccess(node);
    const uncertain = node.sdo_conflicts > 0 || node.sdo_orphans > 0;
    return `<div class="node-row${selectedNode===node.node_id?' selected':''}" data-node="${node.node_id}"><span class="node-id">N${node.node_id}${loggerConfig.selected_nodes.includes(node.node_id)?' *':''}</span><span>${formatTime(node.last_seen_us)}</span><span>${formatInt(node.frames)}</span><span>${nodeTraffic(node).toFixed(1)}%</span><span>${success === undefined ? '—' : `${success.toFixed(1)}%${uncertain ? ' !' : ''}`}</span><span class="${nodeErrorCount(node) ? 'warn' : ''}">${formatInt(nodeErrorCount(node))}</span><span>${escapeHtml(node.last_type)}</span></div>`;
  }).join('');
  list.querySelectorAll<HTMLElement>('[data-node]').forEach(row => row.onclick = () => selectNode(Number(row.dataset.node)));
}

function renderLoggerControls(): void {
  const status = $('logger-status');
  status.className = `logger-status${loggerConfig.recording ? ' recording' : ''}`;
  status.innerHTML = `<span class="dot"></span><span>${loggerConfig.recording ? `Идёт запись · ${escapeHtml(modeName(loggerConfig.active_mode))}` : 'Запись остановлена'}</span>`;
  $<HTMLButtonElement>('start-recording').disabled = loggerConfig.recording;
  $<HTMLButtonElement>('stop-recording').disabled = !loggerConfig.recording;
  $('storage-detail').innerHTML = [
    ['Карта', loggerConfig.card_state || '—', loggerConfig.last_error ? `Код ошибки ${loggerConfig.last_error}` : 'ошибок нет'],
    ['Текущий файл', loggerConfig.file || '—', formatBytes(loggerConfig.file_size_bytes || 0)],
    ['Свободно', formatBytes(loggerConfig.free_bytes || 0), `выбрано нод: ${formatInt(loggerConfig.selected_node_count || 0)}`],
    ['Сохранено', formatInt(loggerConfig.stored_frames || 0), 'кадров'],
    ['Потеряно', formatInt(loggerConfig.dropped_frames || 0), 'кадров'],
    ['Запись на карту', `${formatInt(loggerConfig.write_errors || 0)} ошибок`, `${formatInt(loggerConfig.recoveries || 0)} восстановлений`]
  ].map(([label,value,note]) => `<article><span>${escapeHtml(label)}</span><strong>${escapeHtml(value)}</strong><small>${escapeHtml(note)}</small></article>`).join('');
}

async function refreshNodes(showError = false): Promise<void> {
  try {
    const first = await api<{ total: number; analyzer_frames: number; items: NodeInfo[] }>('/api/v1/nodes?offset=0&limit=64');
    const second = first.total > first.items.length
      ? await api<{ items: NodeInfo[] }>('/api/v1/nodes?offset=64&limit=64')
      : { items: [] as NodeInfo[] };
    analyzerFrames = first.analyzer_frames;
    nodes = [...first.items, ...second.items];
    renderNodes();
    if (selectedNode) { renderNodeSummary(); void loadNodeEvents(selectedNode, false); }
  } catch (error) { if (showError) notify(error instanceof Error ? error.message : 'Не удалось загрузить ноды', true); }
}

function renderNodeSummary(): void {
  const node = nodes.find(item => item.node_id === selectedNode);
  if (!node) return;
  text('node-title', `Нода ${node.node_id}`);
  $('toggle-node-log').removeAttribute('disabled');
  $('toggle-node-log').textContent = loggerConfig.selected_nodes.includes(node.node_id) ? 'Убрать из лога' : 'В лог';
  const success = nodeSdoSuccess(node);
  $('node-summary').innerHTML = `<div><span>Кадры</span><strong>${formatInt(node.frames)}</strong><small>${formatBytes(node.bytes)} · ${nodeTraffic(node).toFixed(1)}%</small></div><div><span>Последний кадр</span><strong>${formatTime(node.last_seen_us)}</strong><small>${escapeHtml(node.last_type)}</small></div><div><span>SDO запросы / ответы</span><strong>${formatInt(node.sdo_requests)} / ${formatInt(node.sdo_responses)}</strong><small>успех ${success === undefined ? '—' : `${success.toFixed(1)}%`}</small></div><div><span>Завершено</span><strong>${formatInt(node.sdo_completed)}</strong><small>из ${formatInt(node.sdo_started)} транзакций</small></div><div><span>Abort / timeout</span><strong>${formatInt(node.sdo_aborted)} / ${formatInt(node.sdo_timeouts)}</strong><small>явный отказ / нет ответа</small></div><div><span>Протокол</span><strong>${formatInt(node.sdo_protocol_errors)}</strong><small>нарушение последовательности</small></div><div><span>Без запроса</span><strong>${formatInt(node.sdo_orphans)}</strong><small>orphan-ответы</small></div><div><span>Конфликты</span><strong>${formatInt(node.sdo_conflicts)}</strong><small>неоднозначные транзакции</small></div>`;
}

async function selectNode(nodeId: number): Promise<void> {
  selectedNode = nodeId;
  renderNodes(); renderNodeSummary();
  await loadNodeEvents(nodeId, true);
}

async function loadNodeEvents(nodeId: number, showError: boolean): Promise<void> {
  try {
    const result = await api<{ items: NodeEvent[] }>(`/api/v1/snapshots/nodes/${nodeId}/${nodeView}?limit=64`);
    const target = $('node-events');
    if (!result.items.length) { target.className = 'event-list empty-state'; target.textContent = 'Событий пока нет'; return; }
    target.className = 'event-list';
    target.innerHTML = [...result.items].reverse().map(event => {
      const object = event.has_sdo_address ? `0x${hex(event.index,4)}` : '—';
      const subindex = event.has_sdo_address ? hex(event.subindex,2) : '—';
      const data = event.remote ? 'RTR' : spacedHex(event.data, event.has_sdo_address ? event.data_offset : 0) || '—';
      const statusClass = event.sdo_status === 'completed' ? 'ok' : event.sdo_status === 'none' || event.sdo_status === 'waiting' ? '' : 'warn';
      return `<div class="event-row event-grid"><span>${formatTime(event.timestamp_us)}</span><span>${event.has_response_time ? `${formatInt(event.response_time_us)} мкс` : '—'}</span><span>0x${hex(event.id,event.extended?8:3)}<small>${event.extended ? 'EXT' : 'STD'}</small></span><span>${escapeHtml(event.type)}</span><span>${object}</span><span>${subindex}</span><span class="data">${data}</span><span class="${statusClass}">${escapeHtml(statusName(event.sdo_status))}${event.abort_code ? `<small>0x${hex(event.abort_code,8)}</small>` : ''}</span></div>`;
    }).join('');
  } catch (error) { if (showError) notify(error instanceof Error ? error.message : 'Не удалось загрузить события', true); }
}

async function refreshLogs(): Promise<void> {
  const list = $('log-list');
  list.className = 'log-list empty-state'; list.textContent = 'Загрузка…';
  try {
    const result = await api<{ items: LogInfo[] }>('/api/v1/logs');
    if (!result.items.length) { list.textContent = 'Завершённых логов нет'; return; }
    list.className = 'log-list';
    list.innerHTML = result.items.map(log => `<div class="log-row"><code>${escapeHtml(log.path)}</code><span>${formatBytes(log.size_bytes)}</span><a href="${escapeHtml(log.download_url)}" download><button>Скачать</button></a></div>`).join('');
  } catch (error) { list.textContent = 'Список недоступен'; notify(error instanceof Error ? error.message : 'Не удалось загрузить логи', true); }
}

async function loadSettings(): Promise<void> {
  try {
    const [network, can, logger] = await Promise.all([api<NetworkConfig>('/api/v1/config/network'), api<CanConfig>('/api/v1/config/can'), api<LoggerConfig>('/api/v1/config/logger')]);
    $<HTMLInputElement>('dhcp-enabled').checked = network.dhcp_enabled;
    $<HTMLInputElement>('static-ip').value = network.static_ip;
    $<HTMLInputElement>('netmask').value = network.netmask;
    $<HTMLInputElement>('gateway').value = network.gateway;
    $<HTMLInputElement>('http-port').value = String(network.http_port);
    $<HTMLInputElement>('dhcp-timeout').value = String(network.dhcp_timeout_ms);
    $<HTMLSelectElement>('can-bitrate').value = String(can.bitrate_kbps);
    $<HTMLInputElement>('deep-sdo').checked = can.deep_sdo_enabled;
    deepSdoEnabled = can.deep_sdo_enabled;
    loggerConfig = logger;
    $<HTMLSelectElement>('logger-mode').value = logger.mode;
    $<HTMLInputElement>('logger-nodes').value = logger.selected_nodes.join(', ');
    renderNodes();
    renderLoggerControls();
  } catch { /* Settings remain editable when the device is temporarily offline. */ }
}

async function saveJson<T = unknown>(path: string, body: unknown, success: string): Promise<T> {
  try { const result = await api<T>(path, { method: 'PUT', body: JSON.stringify(body) }); notify(success); return result; }
  catch (error) { notify(error instanceof Error ? error.message : 'Настройки не сохранены', true); throw error; }
}

async function loggerCommand(action: 'start'|'stop'): Promise<void> {
  try {
    loggerConfig = await api<LoggerConfig>(`/api/v1/logger/${action}`, { method: 'POST', body: '{}' });
    renderLoggerControls();
    notify(action === 'start' ? 'Запись запущена' : 'Запись остановлена');
    if ($('tab-logs').classList.contains('active')) await refreshLogs();
  } catch (error) { notify(error instanceof Error ? error.message : 'Команда logger не выполнена', true); }
}

async function resetStatistics(): Promise<void> {
  if (!window.confirm('Сбросить счётчики CAN, анализатора, логгера и историю кадров?')) return;
  try {
    await api<{ reset: boolean }>('/api/v1/statistics/reset', { method: 'POST', body: '{}' });
    frameBuffer.length = 0;
    pendingFrames = [];
    filteredFrames = [];
    nodes = [];
    analyzerFrames = 0;
    selectedNode = undefined;
    expectedSequence = undefined;
    missedFrames = 0;
    gapBlocks = 0;
    totalLiveFrames = 0;
    lastRateFrames = 0;
    filterDirty = true;
    text('gap-count', '0');
    text('gap-blocks', '0');
    text('node-title', 'Выберите ноду');
    $('toggle-node-log').setAttribute('disabled', '');
    $('node-summary').innerHTML = '';
    $('node-events').className = 'event-list empty-state';
    $('node-events').textContent = 'Здесь появятся события выбранной ноды';
    renderNodes();
    renderVisibleFrames();
    text('traffic-total', '0 кадров');
    text('can-drops', '0');
    text('can-errors', '0');
    await refreshNodes();
    notify('Статистика сброшена');
  } catch (error) { notify(error instanceof Error ? error.message : 'Не удалось сбросить статистику', true); }
}

function bindUi(): void {
  const typeSelect = $<HTMLSelectElement>('type-filter');
  types.forEach(type => typeSelect.add(new Option(type,type)));
  document.querySelectorAll<HTMLButtonElement>('.tabs button[data-tab]').forEach(button => button.onclick = () => {
    document.querySelectorAll('.tabs button,.tab').forEach(item => item.classList.remove('active'));
    button.classList.add('active'); $(`tab-${button.dataset.tab}`).classList.add('active');
    if (button.dataset.tab === 'logs') void refreshLogs();
    if (button.dataset.tab === 'settings') void loadSettings();
  });
  ['frame-search','type-filter','node-filter'].forEach(id => $(id).addEventListener('input', () => { filterDirty = true; renderVisibleFrames(); }));
  $('frame-viewport').addEventListener('scroll', renderVisibleFrames, { passive: true });
  document.querySelectorAll<HTMLButtonElement>('[data-sort]').forEach(button => button.onclick = () => {
    const next = button.dataset.sort as typeof sortKey;
    if (sortKey === next) sortAsc = !sortAsc; else { sortKey = next; sortAsc = next !== 'timestamp'; }
    filterDirty = true; renderVisibleFrames();
  });
  document.querySelectorAll<HTMLButtonElement>('[data-node-sort]').forEach(button => button.onclick = () => {
    const next = button.dataset.nodeSort as NodeSort;
    if (nodeSort === next) nodeSortAsc = !nodeSortAsc; else { nodeSort = next; nodeSortAsc = true; }
    renderNodes();
  });
  $('pause-stream').onclick = () => { paused = !paused; $('pause-stream').textContent = paused ? 'Продолжить' : 'Пауза'; };
  $('clear-stream').onclick = () => { frameBuffer.length = 0; pendingFrames = []; filterDirty = true; renderVisibleFrames(); };
  $('refresh-nodes').onclick = () => void refreshNodes(true);
  $('refresh-logs').onclick = () => void refreshLogs();
  $('reset-statistics').onclick = () => void resetStatistics();
  document.querySelectorAll<HTMLButtonElement>('[data-node-view]').forEach(button => button.onclick = () => {
    document.querySelectorAll('[data-node-view]').forEach(item => item.classList.remove('active'));
    button.classList.add('active'); nodeView = button.dataset.nodeView as typeof nodeView; if (selectedNode) void loadNodeEvents(selectedNode, true);
  });
  $('toggle-node-log').onclick = async () => {
    if (!selectedNode) return;
    const selected = new Set(loggerConfig.selected_nodes);
    selected.has(selectedNode) ? selected.delete(selectedNode) : selected.add(selectedNode);
    const body = { mode: loggerConfig.mode, selected_nodes: [...selected].sort((a,b)=>a-b) };
    try { loggerConfig = await saveJson<LoggerConfig>('/api/v1/config/logger', body, 'Список нод обновлён'); renderNodes(); renderNodeSummary(); renderLoggerControls(); }
    catch { /* toast already shown */ }
  };
  $<HTMLFormElement>('network-form').onsubmit = async event => {
    event.preventDefault();
    const body: NetworkConfig = { dhcp_enabled: $<HTMLInputElement>('dhcp-enabled').checked, static_ip: $<HTMLInputElement>('static-ip').value, netmask: $<HTMLInputElement>('netmask').value, gateway: $<HTMLInputElement>('gateway').value, http_port: Number($<HTMLInputElement>('http-port').value), dhcp_timeout_ms: Number($<HTMLInputElement>('dhcp-timeout').value) };
    try { await saveJson('/api/v1/config/network', body, 'Сетевые настройки сохранены'); } catch { /* handled */ }
  };
  $<HTMLFormElement>('can-form').onsubmit = async event => {
    event.preventDefault();
    const body = { bitrate_kbps: Number($<HTMLSelectElement>('can-bitrate').value), deep_sdo_enabled: $<HTMLInputElement>('deep-sdo').checked };
    try { await saveJson('/api/v1/config/can', body, 'Настройки CAN сохранены'); deepSdoEnabled = body.deep_sdo_enabled; renderNodes(); renderNodeSummary(); } catch { /* handled */ }
  };
  $<HTMLFormElement>('logger-form').onsubmit = async event => {
    event.preventDefault();
    const rawNodes = $<HTMLInputElement>('logger-nodes').value.trim();
    const tokens = rawNodes ? rawNodes.split(',').map(value => value.trim()) : [];
    const selectedNodes = tokens.map(Number);
    if (selectedNodes.some(value => !Number.isInteger(value) || value < 1 || value > 127)) {
      notify('Ноды должны быть числами от 1 до 127 через запятую', true);
      return;
    }
    const body = { mode: $<HTMLSelectElement>('logger-mode').value as LoggerConfig['mode'], selected_nodes: [...new Set(selectedNodes)].sort((a,b)=>a-b) };
    try { loggerConfig = await saveJson<LoggerConfig>('/api/v1/config/logger', body, 'Настройки записи сохранены'); renderNodes(); renderLoggerControls(); } catch { /* handled */ }
  };
  $('start-recording').onclick = () => void loggerCommand('start');
  $('stop-recording').onclick = () => void loggerCommand('stop');
}

bindUi();
renderLoggerControls();
connectStream();
void refreshStatus();
void refreshNodes();
void loadSettings();
flushFrames();
window.setInterval(() => void refreshNodes(), 3000);
window.addEventListener('beforeunload', () => { clearTimeout(flushTimer); ws?.close(); });
