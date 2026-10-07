/* No shell strings or assumed manager APIs. A verified adapter is a future input. */
const targets = Object.freeze({uefi: 'UEFI 菜单', linux: 'Linux', setup: 'UEFI 设置'});
function canRequest(status, bridge) {
  return Boolean(bridge && status && status.device_passthrough_verified === true &&
    status.request_handling_verified === true && status.standard_recovery_preserved === true &&
    status.webui_bridge_verified === true && status.entry_policy === 'explicit-request-only' &&
    status.request_bootarg === 'sunuefi.boot=uefi');
}
function previewText(target) {
  if (!Object.prototype.hasOwnProperty.call(targets, target)) throw new Error('Unknown target');
  return `目标：${targets[target]}。将核对当前活动槽、ROM 和模块拥有的包装，再准备一次性请求。` +
    '普通 Android / 原厂 Recovery 保持原有入口。此预览不写入、不重启。';
}
if (typeof module !== 'undefined' && module.exports) module.exports = {canRequest, previewText};
if (typeof document !== 'undefined') {
  const preview = document.getElementById('preview-detail');
  const target = document.getElementById('preview-target');
  const title = document.getElementById('status-title');
  const detail = document.getElementById('status-detail');
  const buttons = [...document.querySelectorAll('button[data-target]')];
  preview.textContent = previewText(target.value);
  target.addEventListener('change', () => { preview.textContent = previewText(target.value); });
  fetch('status.json', {cache: 'no-store'}).then(response => {
    if (!response.ok) throw new Error('Local status unavailable');
    return response.json();
  }).then(status => {
    // Static package information is not a live device status or permission to execute.
    const bridge = window.pianoModuleBridge;
    const ready = canRequest(status, bridge) && typeof bridge.status === 'function' && typeof bridge.request === 'function';
    title.textContent = ready ? '模块信息可用；需要核对当前设备' : '源码预览，启动请求尚未就绪';
    detail.textContent = '一次性请求与原厂 Recovery 保留需要真实验证。当前页面只显示模块信息和只读预览。';
    // Even a future adapter must obtain fresh read-only native status before enabling a request.
    if (ready) bridge.status().then(current => {
      buttons.forEach(button => { button.disabled = !canRequest(current, bridge); });
    }).catch(() => { buttons.forEach(button => { button.disabled = true; }); });
    buttons.forEach(button => button.addEventListener('click', async () => {
      if (button.disabled || !ready) return;
      const selected = button.dataset.target;
      if (!window.confirm(`${previewText(selected)}\n确认准备请求？本页不会自行重启。`)) return;
      buttons.forEach(item => { item.disabled = true; });
      try {
        const current = await bridge.status();
        if (!canRequest(current, bridge)) throw new Error('Native request is not ready');
        const result = await bridge.request(selected);
        detail.textContent = result && result.stored === true ? '原生工具报告请求已保存。重启方式仍由已验证的消费工具负责。' : '原生工具没有确认请求已保存。';
      } catch (_) { detail.textContent = '请求未完成。没有执行页面自行重启。'; }
    }));
  }).catch(() => { detail.textContent = '本地状态无法读取；启动请求保持禁用。'; });
}
