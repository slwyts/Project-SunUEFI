'use strict';
const targets = new Set(['android', 'uefi', 'linux', 'setup']);
const strings = {
  zh: {assistant:'引导助手',current:'当前系统',reading:'读取中',choose:'选择启动系统',uefiDesc:'Linux 与启动菜单',entry:'进入',remember:'以后开机沿用此选择；只选择不会立即生效。',restart:'保存并重启',tools:'引导维护',check:'检查引导',checkDesc:'检查安装状态和启动选择',reinstall:'重新安装引导',reinstallDesc:'HyperOS 更新后使用',details:'设备信息',installTitle:'重新安装引导？',installBody:'从当前 HyperOS 提取内核并重新打包引导镜像，保留启动选择。Android 数据不会被清除。',cancel:'取消',confirm:'重新安装',menu:'启动菜单',linux:'Linux',setup:'固件设置',ready:'引导已安装',healthy:'引导正常',needsCheck:'需要检查',needsInstall:'需要安装',unavailable:'状态不可用',checking:'正在检查引导…',saving:'正在保存选择，即将重启…',installing:'正在重新安装，请勿关闭此页面…',installed:'引导已更新，可以选择系统并重启。',unchanged:'引导正常，无需重新安装。',checked:'引导正常，可以切换系统。',slot:'当前槽位',savedRoute:'已保存的启动选择',version:'模块版本',yes:'已安装',timeout:'操作用时过长，请稍后重试。',manager:'请从 SukiSU／KernelSU 的模块页面打开。',failed:'操作未完成，请重试。',invalid:'无法读取引导状态。',unknown:'未知',noEntry:'没有可用的 UEFI 入口。',switchUnavailable:'请先检查或重新安装引导。'},
  en: {assistant:'Boot assistant',current:'Current system',reading:'Loading',choose:'Choose a system',uefiDesc:'Linux and boot menu',entry:'Open',remember:'Future boots follow this choice. Selecting alone does not apply it.',restart:'Save and restart',tools:'Boot maintenance',check:'Check boot',checkDesc:'Check BOOT and the saved choice',reinstall:'Reinstall boot',reinstallDesc:'Use after a HyperOS update',details:'Device information',installTitle:'Reinstall boot?',installBody:'Extract the current HyperOS kernel and repack BOOT, keeping your boot choice. Android data will not be erased.',cancel:'Cancel',confirm:'Reinstall',menu:'Boot menu',linux:'Linux',setup:'Firmware settings',ready:'Boot installed',healthy:'Boot healthy',needsCheck:'Check required',needsInstall:'Install required',unavailable:'Status unavailable',checking:'Checking boot…',saving:'Saving your choice. Restarting…',installing:'Reinstalling. Keep this page open…',installed:'Boot updated. You can choose a system and restart.',unchanged:'Boot is healthy. No reinstall needed.',checked:'Boot is healthy. You can switch systems.',slot:'Active slot',savedRoute:'Saved boot choice',version:'Module version',yes:'Installed',timeout:'This is taking too long. Please try again shortly.',manager:'Open this page from SukiSU / KernelSU modules.',failed:'The operation did not finish. Please try again.',invalid:'Could not read boot status.',unknown:'Unknown',noEntry:'No UEFI entry is available.',switchUnavailable:'Check or reinstall boot first.'}
};
let language = /^en\b/i.test(typeof navigator === 'object' ? navigator.language : '') ? 'en' : 'zh';
try { const saved = localStorage.getItem('sunuefi-language'); if (saved === 'zh' || saved === 'en') language = saved; } catch (_) {}
const t = key => strings[language][key] || key;
let counter = 0;
function nativeAction(action, target) {
  if (!['quick-status', 'status', 'switch', 'reinstall'].includes(action) || (target && !targets.has(target))) return Promise.reject(new Error(t('failed')));
  if (!window.ksu || typeof window.ksu.exec !== 'function') return Promise.reject(new Error(t('manager')));
  const command = '/system/bin/sh /data/adb/modules/piano_sunuefi/action.sh ' + action + (target ? ' ' + target : '');
  return new Promise((resolve, reject) => {
    const name = 'sunuefi_cb_' + counter++;
    const timer = setTimeout(() => { delete window[name]; reject(new Error(t('timeout'))); }, action === 'quick-status' ? 8000 : 120000);
    window[name] = (code, stdout, stderr) => {
      clearTimeout(timer); delete window[name];
      if (Number(code) !== 0) { reject(new Error(String(stderr || t('failed')).trim())); return; }
      try { resolve(JSON.parse(String(stdout).trim())); } catch (_) { reject(new Error(t('invalid'))); }
    };
    try { window.ksu.exec(command, '{}', name); }
    catch (error) { clearTimeout(timer); delete window[name]; reject(error); }
  });
}
if (typeof document !== 'undefined') {
  const $ = id => document.getElementById(id);
  let status = null, entries = [], selectedEntry = 'linux', busy = false, selectionChanged = false, messageKey = '';
  const route = () => document.querySelector('input[name="route"]:checked').value;
  const destinationName = id => t(id === 'uefi' ? 'menu' : id);
  function say(key, error) { messageKey = key; $('message').textContent = t(key); $('message').classList.toggle('error', !!error); }
  function errorMessage(error) { messageKey = ''; $('message').textContent = error.message || t('failed'); $('message').classList.add('error'); }
  function updateButtons() {
    const ready = status && status.wrapped && (status.owned === true || status.state_available === true) && status.owned !== false;
    $('restart').disabled = busy || !ready || (route() === 'uefi' && !entries.some(item => item.id === selectedEntry));
    $('check').disabled = busy;
    $('reinstall').disabled = busy;
    $('entry-group').hidden = route() !== 'uefi';
    document.querySelectorAll('.entry').forEach(button => { button.setAttribute('aria-pressed', String(button.dataset.entry === selectedEntry)); });
  }
  function renderDetails() {
    $('details').replaceChildren();
    const target = status && status.request_target;
    const saved = target === 0 ? 'Android' : target > 0 ? 'UEFI · ' + destinationName(target === 2 ? 'linux' : target === 3 ? 'setup' : 'uefi') : t('unknown');
    for (const [key,value] of [['slot', status && status.slot || t('unknown')],['savedRoute',saved],['version','0.2.0']]) {
      const row = document.createElement('div'), label = document.createElement('dt'), text = document.createElement('dd');
      label.textContent = t(key); text.textContent = value; row.append(label,text); $('details').append(row);
    }
  }
  function renderEntries() {
    $('entries').replaceChildren();
    for (const item of entries) {
      const button = document.createElement('button'); button.className = 'entry'; button.dataset.entry = item.id; button.textContent = destinationName(item.id);
      button.addEventListener('click', () => { selectedEntry = item.id; selectionChanged = true; updateButtons(); }); $('entries').append(button);
    }
    if (!entries.length) { const note = document.createElement('p'); note.className = 'hint'; note.textContent = t('noEntry'); $('entries').append(note); }
  }
  function renderHealth() {
    $('health').textContent = !status ? t('unavailable') : status.wrapped && status.owned === true ? t('healthy') : status.wrapped && status.state_available ? t('ready') : status.wrapped ? t('needsCheck') : t('needsInstall');
  }
  function show(current, quick) {
    status = quick ? current : {...status, ...current};
    if (quick) entries = (current.entries || []).filter(item => item.available && ['uefi','linux','setup'].includes(item.id)).sort((a,b) => (a.id === 'linux' ? -1 : b.id === 'linux' ? 1 : 0));
    if (!selectionChanged) {
      const target = status.request_target ?? 0;
      document.querySelector('input[name="route"][value="' + (target === 0 ? 'android' : 'uefi') + '"]').checked = true;
      selectedEntry = target === 0 || target === 2 ? 'linux' : target === 3 ? 'setup' : 'uefi';
    }
    if (!entries.some(item => item.id === selectedEntry)) selectedEntry = entries.length ? entries[0].id : '';
    renderEntries(); renderHealth(); renderDetails(); updateButtons();
  }
  function translate() {
    document.documentElement.lang = language === 'en' ? 'en' : 'zh-CN';
    document.querySelectorAll('[data-i18n]').forEach(element => { element.textContent = t(element.dataset.i18n); });
    $('language').textContent = language === 'en' ? '中文' : 'EN';
    $('language').setAttribute('aria-label', language === 'en' ? '切换为中文' : 'Switch to English');
    if (status) { renderEntries(); renderHealth(); renderDetails(); updateButtons(); }
    if (messageKey) say(messageKey, $('message').classList.contains('error'));
  }
  async function refresh(deep) {
    if (busy) return;
    if (deep) { busy = true; say('checking'); updateButtons(); }
    try { show(await nativeAction(deep ? 'status' : 'quick-status'), !deep); if (deep) say(status.owned ? 'checked' : 'switchUnavailable', !status.owned); }
    catch (error) { $('health').textContent = t('unavailable'); errorMessage(error); }
    finally { busy = false; updateButtons(); }
  }
  $('language').addEventListener('click', () => { language = language === 'en' ? 'zh' : 'en'; try { localStorage.setItem('sunuefi-language',language); } catch (_) {} translate(); });
  document.querySelectorAll('input[name="route"]').forEach(input => input.addEventListener('change', () => { selectionChanged = true; updateButtons(); }));
  $('check').addEventListener('click', () => refresh(true));
  $('restart').addEventListener('click', async () => {
    if (busy) return; const target = route() === 'android' ? 'android' : selectedEntry;
    busy = true; updateButtons(); say('saving');
    try { await nativeAction('switch',target); }
    catch (error) { busy = false; updateButtons(); errorMessage(error); }
  });
  $('reinstall').addEventListener('click', () => $('install-dialog').showModal());
  $('cancel-install').addEventListener('click', () => $('install-dialog').close());
  $('confirm-install').addEventListener('click', async () => {
    $('install-dialog').close(); if (busy) return; busy = true; updateButtons(); say('installing');
    try {
      const result = await nativeAction('reinstall'); selectionChanged = false;
      show(await nativeAction('quick-status'), true); say(result.status === 'ADOPTED_READ_ONLY_VERIFIED' ? 'unchanged' : 'installed');
    } catch (error) { errorMessage(error); }
    finally { busy = false; updateButtons(); }
  });
  translate(); refresh(false);
}
if (typeof module !== 'undefined') module.exports = {nativeAction};
