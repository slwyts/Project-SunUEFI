'use strict';
const targets = new Set(['android', 'uefi', 'linux', 'setup']);
const strings = {
  zh: {espImage:'ESP 镜像',rootImage:'root 镜像',notSelected:'尚未选择',selectFile:'选择本地镜像',advanced:'高级操作',selecting:'正在读取所选镜像…',backendMissing:'请更新模块以安装存储组件。',alreadyCreated:'ESP 和 root 已创建。',createHint:'建立启动分区和系统分区，保留 Android 数据。',resizeHint:'调整 ext4 文件系统和分区边界。',noPartitions:'请先创建独立分区。',selectImages:'先选择 ESP 和 root 镜像。',tabBoot:'启动',tabStorage:'存储',tabMaintenance:'维护',storageTitle:'独立存储分区',refresh:'刷新',flashTitle:'安装或更新系统',imageLocation:'将 esp.img 和 root.ext4.img 放到「下载/SunUEFI」文件夹。',flash:'安装所选镜像',partitionTitle:'分区管理',rootSize:'root 目标容量',create:'创建独立分区',resize:'调整 root 容量',delete:'删除并归还 Android 空间',continue:'继续',absent:'未创建',mounted:'使用中',unmounted:'未挂载',planning:'正在准备操作…',storageRunning:'正在处理，请勿重启或拔除电源…',storageDone:'操作完成。',storageFailed:'操作未完成。',flashWarning:'这会覆盖独立分区中的系统和启动文件。Android 数据不会被清除。',deleteWarning:'删除独立分区中的系统和文件，并将空间归还 Android。正常重启后，原厂系统会完成文件系统扩容。',createWarning:'建立 ESP 启动分区和 root 系统分区；需要时先缩小 Android 数据分区。',resizeWarning:'调整 root 文件系统及分区容量。移动分区起点的操作暂不支持。',before:'当前容量',after:'目标容量',restartRequired:'请正常重启，完成 Android 空间扩容。',storageReboot:'重启完成空间归还',returned:'归还空间',spacePending:'分区表已更新，等待重启完成 Android 扩容。',assistant:'引导助手',current:'当前系统',reading:'读取中',choose:'选择启动系统',uefiDesc:'Linux 与启动菜单',entry:'进入',remember:'以后开机沿用此选择；只选择不会立即生效。',restart:'保存并重启',tools:'引导维护',check:'检查引导',checkDesc:'检查安装状态和启动选择',reinstall:'重新安装引导',reinstallDesc:'HyperOS 更新后使用',details:'设备信息',installTitle:'重新安装引导？',installBody:'从当前 HyperOS 提取内核并重新打包引导镜像，保留启动选择。Android 数据不会被清除。',cancel:'取消',confirm:'重新安装',menu:'启动菜单',linux:'Linux',setup:'固件设置',ready:'引导已安装',healthy:'引导正常',needsCheck:'需要检查',needsInstall:'需要安装',unavailable:'状态不可用',checking:'正在检查引导…',saving:'正在保存选择，即将重启…',installing:'正在重新安装，请勿关闭此页面…',installed:'引导已更新，可以选择系统并重启。',unchanged:'引导正常，无需重新安装。',checked:'引导正常，可以切换系统。',slot:'当前槽位',savedRoute:'已保存的启动选择',version:'模块版本',yes:'已安装',timeout:'操作用时过长，请稍后重试。',manager:'请从 SukiSU／KernelSU 的模块页面打开。',failed:'操作未完成，请重试。',invalid:'无法读取引导状态。',unknown:'未知',noEntry:'没有可用的 UEFI 入口。',switchUnavailable:'请先检查或重新安装引导。'},
  en: {espImage:'ESP image',rootImage:'Root image',notSelected:'Not selected',selectFile:'Choose file',advanced:'Advanced',selecting:'Reading the selected image…',backendMissing:'Update the module to install storage tools.',alreadyCreated:'ESP and root already exist.',createHint:'Create Linux ESP and root, keeping Android data.',resizeHint:'Resize the ext4 filesystem and partition.',noPartitions:'Create Linux partitions first.',selectImages:'Select both ESP and root images first.',tabBoot:'Boot',tabStorage:'Storage',tabMaintenance:'Maintenance',storageTitle:'Independent storage',refresh:'Refresh',flashTitle:'Install or update a system',imageLocation:'Place esp.img and root.ext4.img in Downloads/SunUEFI.',flash:'Install selected images',partitionTitle:'Partitions',rootSize:'Target root size',create:'Create independent partitions',resize:'Resize root',delete:'Delete and return space to Android',continue:'Continue',absent:'Not created',mounted:'In use',unmounted:'Unmounted',planning:'Preparing operation…',storageRunning:'Working. Do not restart or disconnect power…',storageDone:'Operation completed.',storageFailed:'The operation did not finish.',flashWarning:'This replaces the Linux system and ESP boot files. Android data will not be erased.',deleteWarning:'This removes sunuefi_esp and sunuefi_root. Linux data becomes inaccessible. Freed space is not automatically returned to Android.',createWarning:'Create Linux ESP and root partitions, shrinking Android storage first if necessary.',resizeWarning:'Resize the root filesystem and partition. Moving its start is not supported yet.',before:'Current size',after:'Target size',restartRequired:'Restart normally to finish expanding Android storage.',storageReboot:'Restart to finish',returned:'Returned space',spacePending:'Partition table updated. Restart to finish expanding Android storage.',assistant:'Boot assistant',current:'Current system',reading:'Loading',choose:'Choose a system',uefiDesc:'Linux and boot menu',entry:'Open',remember:'Future boots follow this choice. Selecting alone does not apply it.',restart:'Save and restart',tools:'Boot maintenance',check:'Check boot',checkDesc:'Check BOOT and the saved choice',reinstall:'Reinstall boot',reinstallDesc:'Use after a HyperOS update',details:'Device information',installTitle:'Reinstall boot?',installBody:'Extract the current HyperOS kernel and repack BOOT, keeping your boot choice. Android data will not be erased.',cancel:'Cancel',confirm:'Reinstall',menu:'Boot menu',linux:'Linux',setup:'Firmware settings',ready:'Boot installed',healthy:'Boot healthy',needsCheck:'Check required',needsInstall:'Install required',unavailable:'Status unavailable',checking:'Checking boot…',saving:'Saving your choice. Restarting…',installing:'Reinstalling. Keep this page open…',installed:'Boot updated. You can choose a system and restart.',unchanged:'Boot is healthy. No reinstall needed.',checked:'Boot is healthy. You can switch systems.',slot:'Active slot',savedRoute:'Saved boot choice',version:'Module version',yes:'Installed',timeout:'This is taking too long. Please try again shortly.',manager:'Open this page from SukiSU / KernelSU modules.',failed:'The operation did not finish. Please try again.',invalid:'Could not read boot status.',unknown:'Unknown',noEntry:'No UEFI entry is available.',switchUnavailable:'Check or reinstall boot first.'}
};
let language = /^en\b/i.test(typeof navigator === 'object' ? navigator.language : '') ? 'en' : 'zh';
try { const saved = localStorage.getItem('sunuefi-language'); if (saved === 'zh' || saved === 'en') language = saved; } catch (_) {}
const t = key => strings[language][key] || key;
let counter = 0;
function nativeAction(action, target, extra, bytes) {
  let valid = ['quick-status','status','reinstall','storage-status','storage-reboot'].includes(action) && !target && !extra;
  if (action === 'switch') valid = targets.has(target) && !extra;
  if (action === 'storage-select') valid = ['esp','root'].includes(target) && /^[0-9a-f]{2,2048}$/.test(extra || '') && /^[0-9]{1,13}$/.test(String(bytes || ''));
  if (action === 'storage-plan') valid = ['flash','create','resize','delete','delete-return'].includes(target) && (!extra || ['32','64','128'].includes(String(extra)));
  if (action === 'storage-execute' || action === 'storage-job') valid = /^[a-zA-Z0-9_-]{1,80}$/.test(target || '') && !extra;
  if (!valid) return Promise.reject(new Error(t('failed')));
  if (!window.ksu || typeof window.ksu.exec !== 'function') return Promise.reject(new Error(t('manager')));
  const command = '/system/bin/sh /data/adb/modules/piano_sunuefi/action.sh ' + action + (target ? ' ' + target : '') + (extra ? ' ' + extra : '') + (bytes ? ' ' + bytes : '');
  return new Promise((resolve, reject) => {
    const name = 'sunuefi_cb_' + counter++;
    const timer = setTimeout(() => { delete window[name]; reject(new Error(t('timeout'))); }, action === 'quick-status' || action === 'storage-status' || action === 'storage-job' ? 8000 : 120000);
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
  let storage = null, storagePlan = null, rootSize = '64', activeTab = 'boot', activeAction = '', activeJob = null, needsStorageReboot = false;
  const route = () => document.querySelector('input[name="route"]:checked').value;
  const destinationName = id => t(id === 'uefi' ? 'menu' : id);
  function say(key, error) { messageKey = key; $('message').textContent = t(key); $('message').classList.toggle('error', !!error); }
  function errorMessage(error) { messageKey = ''; $('message').textContent = error.message || t('failed'); $('message').classList.add('error'); }
  function updateButtons() {
    document.body.classList.toggle('working',busy);
    for (const id of ['restart','check','reinstall','storage-flash','storage-create','storage-resize','storage-delete','choose-esp','choose-root','storage-reboot']) $(id).setAttribute('aria-busy',String(busy && activeAction === id));
    const ready = status && status.wrapped && (status.owned === true || status.state_available === true) && status.owned !== false;
    $('restart').disabled = busy || needsStorageReboot || !ready || (route() === 'uefi' && !entries.some(item => item.id === selectedEntry));
    $('check').disabled = busy;
    $('reinstall').disabled = busy;
    $('entry-group').hidden = route() !== 'uefi';
    const caps = storage && storage.capabilities || {};
    for (const op of ['flash','create','resize','delete']) {
      $('storage-' + op).disabled = busy || !(caps[op === 'flash' ? 'flash_existing' : op === 'delete' ? 'release_to_android' : op]);
    }
    const chosenSources = storage && storage.sources || [];
    $('storage-flash').disabled = busy || !caps.flash_existing || !['esp','root'].every(kind => chosenSources.some(item => item.kind === kind && item.selected));
    for (const id of ['storage-flash','storage-create','storage-resize','storage-delete']) if (needsStorageReboot) $(id).disabled = true;
    $('storage-reboot').hidden = !needsStorageReboot; $('storage-reboot').disabled = busy;
    $('storage-refresh').disabled = busy;
    $('choose-esp').disabled = busy; $('choose-root').disabled = busy;
    if (storage && storage.layout === 'existing') $('storage-create').disabled = true;
    document.querySelectorAll('.entry').forEach(button => { button.setAttribute('aria-pressed', String(button.dataset.entry === selectedEntry)); });
  }
  function renderDetails() {
    $('details').replaceChildren();
    const target = status && status.request_target;
    const saved = target === 0 ? 'Android' : target > 0 ? 'UEFI · ' + destinationName(target === 2 ? 'linux' : target === 3 ? 'setup' : 'uefi') : t('unknown');
    for (const [key,value] of [['slot', status && status.slot || t('unknown')],['savedRoute',saved],['version','0.3.0']]) {
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
    if (storage) renderStorage();
    $('page-title').textContent = t(activeTab === 'boot' ? 'tabBoot' : activeTab === 'storage' ? 'tabStorage' : 'tabMaintenance');
  }
  async function refresh(deep) {
    if (busy) return;
    if (deep) { busy = true; activeAction = 'check'; say('checking'); updateButtons(); }
    try { show(await nativeAction(deep ? 'status' : 'quick-status'), !deep); if (deep) say(status.owned ? 'checked' : 'switchUnavailable', !status.owned); }
    catch (error) { $('health').textContent = t('unavailable'); errorMessage(error); }
    finally { busy = false; updateButtons(); }
  }
  function sizeText(bytes) {
    if (Number(bytes) < 1048576) return Math.round(Number(bytes)/1024) + ' KiB';
    const value = Number(bytes) / 1073741824;
    return value >= 1 ? value.toLocaleString(language === 'en' ? 'en-US' : 'zh-CN', {maximumFractionDigits:1}) + ' GiB' : Math.round(Number(bytes)/1048576) + ' MiB';
  }
  function renderStorage() {
    $('partition-list').replaceChildren();
    for (const item of storage.partitions || []) {
      const box = document.createElement('div'), name = document.createElement('h3'), capacity = document.createElement('strong'), detail = document.createElement('p');
      box.className = 'partition'; name.textContent = item.name; capacity.textContent = item.present ? sizeText(item.bytes) : t('absent');
      detail.textContent = item.present ? (item.filesystem === 'vfat' ? 'FAT32' : item.filesystem) + ' · ' + t(item.mounted ? 'mounted' : 'unmounted') : '—';
      box.append(name,capacity,detail); $('partition-list').append(box);
    }
    const sources = storage.sources || [];
    for (const kind of ['esp','root']) {
      const source = sources.find(item => item.kind === kind);
      $('source-' + kind).textContent = source && source.selected ? source.name + ' · ' + sizeText(source.bytes) : t('notSelected');
    }
    const caps = storage.capabilities || {};
    $('create-reason').textContent = storage.layout === 'existing' ? t('alreadyCreated') : caps.create ? t('createHint') : t('backendMissing');
    $('resize-reason').textContent = storage.layout !== 'existing' ? t('noPartitions') : caps.resize ? t('resizeHint') : t('backendMissing');
    $('delete-reason').textContent = storage.layout === 'existing' ? t('deleteWarning') : t('noPartitions');
    $('flash-reason').textContent = !caps.flash_existing ? (storage.layout !== 'existing' ? t('noPartitions') : t('selectImages')) : '';
    updateButtons();
  }
  async function refreshStorage() {
    try {
      $('storage-refresh').setAttribute('aria-busy','true');
      storage = await nativeAction('storage-status'); needsStorageReboot = !!storage.pending_android_expansion; renderStorage();
      if (storage.active_job_id && activeJob !== storage.active_job_id) { activeJob = storage.active_job_id; busy = true; activeAction = ''; updateButtons(); pollStorage(activeJob); }
    }
    catch (error) { errorMessage(error); }
    finally { $('storage-refresh').setAttribute('aria-busy','false'); }
  }
  function selectTab(name, focus) {
    activeTab = name; $('page-title').textContent = t(name === 'boot' ? 'tabBoot' : name === 'storage' ? 'tabStorage' : 'tabMaintenance');
    $('device-details').hidden = name !== 'maintenance';
    window.scrollTo(0,0);
    for (const tab of document.querySelectorAll('[data-tab]')) {
      const active = tab.dataset.tab === name;
      tab.setAttribute('aria-selected', String(active)); tab.tabIndex = active ? 0 : -1;
      $('panel-' + tab.dataset.tab).hidden = !active;
      if (active && focus) tab.focus();
    }
    if (name === 'storage') refreshStorage();
  }
  document.querySelectorAll('[data-tab]').forEach(tab => {
    tab.addEventListener('click', () => selectTab(tab.dataset.tab));
    tab.addEventListener('keydown', event => {
      if (!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown','Home','End'].includes(event.key)) return;
      event.preventDefault(); const names = ['boot','storage','maintenance'];
      const index = names.indexOf(tab.dataset.tab);
      selectTab(names[event.key === 'Home' ? 0 : event.key === 'End' ? 2 : (index + (event.key === 'ArrowLeft' || event.key === 'ArrowUp' ? 2 : 1)) % 3], true);
    });
  });
  document.querySelectorAll('[data-size]').forEach(button => button.addEventListener('click', () => {
    rootSize = button.dataset.size; document.querySelectorAll('[data-size]').forEach(choice => choice.setAttribute('aria-pressed',String(choice.dataset.size === rootSize)));
  }));
  for (const kind of ['esp','root']) {
    $('choose-' + kind).addEventListener('click', () => { $('file-' + kind).value = ''; $('file-' + kind).click(); });
    $('file-' + kind).addEventListener('change', async event => {
      const file = event.target.files[0]; if (!file || busy) return;
      const hex = Array.from(new TextEncoder().encode(file.name), byte => byte.toString(16).padStart(2,'0')).join('');
      busy = true; activeAction = 'choose-' + kind; updateButtons(); say('selecting');
      try { await nativeAction('storage-select',kind,hex,String(file.size)); await refreshStorage(); say(''); }
      catch (error) { errorMessage(error); }
      finally { busy = false; updateButtons(); }
    });
  }
  $('storage-refresh').addEventListener('click', refreshStorage);
  async function prepareStorage(operation, confirmSize) {
    if (busy) return;
    if (['create','resize'].includes(operation) && !confirmSize) {
      storagePlan = {operation,needsSize:true}; $('storage-dialog-title').textContent = t(operation);
      $('storage-warning').textContent = t(operation + 'Warning'); $('storage-plan-details').replaceChildren();
      $('storage-size-choice').hidden = false; $('storage-confirm').textContent = t('continue'); $('storage-dialog').showModal(); return;
    }
    busy = true; activeAction = 'storage-' + operation; updateButtons(); say('planning');
    try {
      storagePlan = await nativeAction('storage-plan',operation === 'delete' ? 'delete-return' : operation,['create','resize'].includes(operation) ? rootSize : null);
      storagePlan.operation = operation;
      $('storage-dialog-title').textContent = t(operation); $('storage-warning').textContent = t(operation + 'Warning');
      $('storage-size-choice').hidden = true; $('storage-confirm').textContent = t(operation);
      $('storage-plan-details').replaceChildren();
      const details = [];
      if (storagePlan.current_root_bytes) details.push(['before', sizeText(storagePlan.current_root_bytes)]);
      if (storagePlan.target_root_bytes) details.push(['after', sizeText(storagePlan.target_root_bytes)]);
      if (storagePlan.returned_bytes) details.push(['returned', sizeText(storagePlan.returned_bytes)]);
      for (const source of storagePlan.sources || []) if (source.selected) details.push([source.kind === 'esp' ? 'espImage' : 'rootImage', source.name + ' · ' + sizeText(source.bytes)]);
      for (const [key,value] of details) { const row = document.createElement('div'), label = document.createElement('dt'), text = document.createElement('dd'); label.textContent=t(key); text.textContent=value; row.append(label,text); $('storage-plan-details').append(row); }
      $('storage-dialog').showModal(); say('');
    } catch (error) { errorMessage(error); }
    finally { busy = false; updateButtons(); }
  }
  for (const operation of ['flash','create','resize','delete']) $('storage-' + operation).addEventListener('click', () => prepareStorage(operation));
  $('storage-cancel').addEventListener('click', () => { storagePlan = null; $('storage-dialog').close(); });
  async function pollStorage(job) {
    try {
      const result = await nativeAction('storage-job',job);
      if (result.status === 'pending_android_expansion') {
        busy = false; needsStorageReboot = true; say('spacePending'); await refreshStorage(); updateButtons(); return;
      }
      if (result.status === 'completed' || result.status === 'success') {
        busy = false; activeJob = null; say('storageDone'); if (result.restart_required) $('message').textContent += ' ' + t('restartRequired');
        storage = null; await refreshStorage(); await refresh(false); updateButtons(); return;
      }
      if (result.status === 'failed' || result.status === 'error') throw new Error(result.error || result.message || t('storageFailed'));
      say('storageRunning');
      if (typeof result.percent === 'number') $('message').textContent += ' ' + Math.round(result.percent) + '%';
      setTimeout(() => pollStorage(job),1000);
    } catch (error) { busy = false; activeJob = null; errorMessage(error); updateButtons(); }
  }
  $('storage-reboot').addEventListener('click', async () => {
    if (busy) return; busy = true; activeAction = 'storage-reboot'; say('saving'); updateButtons();
    try { await nativeAction('storage-reboot'); } catch (error) { busy = false; errorMessage(error); updateButtons(); }
  });
  $('storage-confirm').addEventListener('click', async () => {
    if (busy || !storagePlan) return;
    if (storagePlan.needsSize) { const operation = storagePlan.operation; $('storage-dialog').close(); storagePlan = null; await prepareStorage(operation,true); return; }
    $('storage-dialog').close(); busy = true; activeAction = 'storage-' + storagePlan.operation; updateButtons(); say('storageRunning');
    try { const result = await nativeAction('storage-execute',storagePlan.plan_id); storagePlan = null; activeJob = result.job_id; await pollStorage(activeJob); }
    catch (error) { busy = false; errorMessage(error); updateButtons(); }
  });
  $('language').addEventListener('click', () => { language = language === 'en' ? 'zh' : 'en'; try { localStorage.setItem('sunuefi-language',language); } catch (_) {} translate(); });
  document.querySelectorAll('input[name="route"]').forEach(input => input.addEventListener('change', () => { selectionChanged = true; updateButtons(); }));
  $('check').addEventListener('click', () => refresh(true));
  $('restart').addEventListener('click', async () => {
    if (busy) return; const target = route() === 'android' ? 'android' : selectedEntry;
    busy = true; activeAction = 'restart'; updateButtons(); say('saving');
    try { await nativeAction('switch',target); }
    catch (error) { busy = false; updateButtons(); errorMessage(error); }
  });
  $('reinstall').addEventListener('click', () => $('install-dialog').showModal());
  $('cancel-install').addEventListener('click', () => $('install-dialog').close());
  $('confirm-install').addEventListener('click', async () => {
    $('install-dialog').close(); if (busy) return; busy = true; activeAction = 'reinstall'; updateButtons(); say('installing');
    try {
      const result = await nativeAction('reinstall'); selectionChanged = false;
      show(await nativeAction('quick-status'), true); say(result.status === 'ADOPTED_READ_ONLY_VERIFIED' ? 'unchanged' : 'installed');
    } catch (error) { errorMessage(error); }
    finally { busy = false; updateButtons(); }
  });
  translate(); refresh(false);
}
if (typeof module !== 'undefined') module.exports = {nativeAction};
