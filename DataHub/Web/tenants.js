// DataHub 根页（/）租户选择/管理逻辑：创建 / 凭码加入 / 进入 / 移出。
// 共享状态（账号、我的租户、当前租户）来自 common.js 的 window.DH。
(function () {
  'use strict';

  var DH = window.DH;
  function $(id) { return document.getElementById(id); }
  function esc(s) {
    return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  }
  function toast(msg) {
    var t = $('toast');
    t.textContent = msg; t.style.display = 'block';
    clearTimeout(t._tm);
    t._tm = setTimeout(function () { t.style.display = 'none'; }, 2200);
  }

  var roleOf = {};  // code -> 'owner' | 'member' | ''（服务端花名册里"我"的角色）
  function roleName(role) { return role === 'owner' ? 'Owner' : (role === 'member' ? '成员' : ''); }

  // 逐个非公共租户拉花名册，找出"我"的角色（X-Tenant 按租户覆写）。
  function loadRoles() {
    var codes = [];
    for (var i = 0; i < DH.tenants.length; i++) codes.push(DH.tenants[i].code);
    var chain = Promise.resolve();
    codes.forEach(function (code) {
      chain = chain.then(function () {
        return DH.apiFetch('/api/tenant/members', { headers: { 'X-Tenant': code } })
          .then(function (r) { if (!r.ok) throw 0; return r.json(); })
          .then(function (j) {
            var me = '';
            var arr = j.members || [];
            for (var k = 0; k < arr.length; k++) {
              if (arr[k].account === DH.CLIENT_ID) { me = arr[k].role; break; }
            }
            roleOf[code] = me;
          })
          .catch(function () { roleOf[code] = ''; });
      });
    });
    return chain.then(render);
  }

  function render() {
    // 当前聊天租户提示。
    var curName = DH.currentName + (DH.currentCode !== 'public' ? ' · ' + DH.currentCode : '');
    $('mHint').innerHTML = '当前聊天租户：<b>' + esc(curName) + '</b>' +
      '　<a class="link" href="/chat">去聊天 →</a>';

    // 列表：公共租户恒在，其后为已加入租户。
    var html = rowHtml('public', '公共租户', 'public', '开放', '进入');
    var joined = false;
    for (var i = 0; i < DH.tenants.length; i++) {
      var t = DH.tenants[i];
      var role = roleName(roleOf[t.code] || '');
      var label = (t.code === DH.currentCode) ? '当前 · 进入' : '进入';
      html += rowHtml(t.code, t.name, t.code, role, label);
      joined = true;
    }
    $('mList').innerHTML = joined
      ? html
      : html + '<div class="empty">还没有加入其它租户——在上方创建一个，或凭码加入。</div>';
  }

  function rowHtml(code, name, codeText, role, goLabel) {
    var roleTag = role ? '<span class="trole' + (role === 'Owner' ? ' owner' : '') + '">' + esc(role) + '</span>' : '';
    var rm = (code === 'public')
      ? ''
      : '<button class="rm" onclick="window.__tenantsRemove(\'' + code + '\')">移出</button>';
    return '<div class="trow">' +
      '<span class="tname">' + esc(name) + '</span>' +
      '<span class="tcode">' + esc(codeText) + '</span>' +
      roleTag +
      '<span class="tspacer"></span>' +
      '<button class="go" onclick="window.__tenantsEnter(\'' + code + '\')">' + goLabel + '</button>' +
      rm +
      '</div>';
  }

  // 进入：确保成员身份（join 幂等）→ 设为当前租户 → 回聊天页。
  function enter(code) {
    if (code === 'public') {
      DH.setCurrent('public');
      location.href = '/chat';
      return;
    }
    DH.apiFetch('/api/tenant/join', { method: 'POST', body: code })
      .then(function (r) { if (!r.ok) throw new Error('无法加入 (' + r.status + ')'); return r.json(); })
      .then(function () { DH.setCurrent(code); location.href = '/chat'; })
      .catch(function (e) { toast(e.message || '进入失败'); });
  }

  // 移出：仅从"我的租户"移除；若移除的恰是当前租户则回落公共。
  function remove(code) {
    if (code === 'public') return;
    var wasCurrent = DH.removeTenant(code);
    delete roleOf[code];
    toast(wasCurrent ? '已移出，当前回到公共租户' : '已移出该租户');
    render();
  }

  function createTenant() {
    var name = $('mkName').value.trim();
    if (!name) { toast('请输入租户名称'); return; }
    DH.apiFetch('/api/tenant', { method: 'POST', body: name })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (j.code) {
          DH.addTenant(j.code, j.name || name);
          $('mkName').value = '';
          roleOf[j.code] = 'owner';
          render();
          toast('已创建「' + (j.name || name) + '」，码 ' + j.code + '（你是 Owner）');
        } else {
          toast(j.error || '创建失败');
        }
      })
      .catch(function () { toast('网络错误'); });
  }

  function joinTenant() {
    var code = $('mjCode').value.trim();
    if (!code) { toast('请输入 6 位租户码'); return; }
    // 先校验租户存在并取名称，再登记为成员。
    DH.apiFetch('/api/tenant/info?code=' + encodeURIComponent(code))
      .then(function (r) { return r.json(); })
      .then(function (j) {
        if (!j.code) throw new Error('租户不存在');
        return DH.apiFetch('/api/tenant/join', { method: 'POST', body: code })
          .then(function (r) { if (!r.ok) throw new Error('加入失败'); return r.json(); })
          .then(function (join) { return { name: j.name || code, role: join.role || 'member' }; });
      })
      .then(function (res) {
        DH.addTenant(code, res.name);
        $('mjCode').value = '';
        roleOf[code] = res.role;
        render();
        toast('已加入租户「' + res.name + '」');
      })
      .catch(function (e) { toast(e.message || '加入失败'); });
  }

  // 供行内 onclick 调用（全局作用域查找）。
  window.__tenantsEnter = enter;
  window.__tenantsRemove = remove;

  $('mkBtn').addEventListener('click', createTenant);
  $('mkName').addEventListener('keydown', function (e) { if (e.key === 'Enter') { e.preventDefault(); createTenant(); } });
  $('mjBtn').addEventListener('click', joinTenant);
  $('mjCode').addEventListener('keydown', function (e) { if (e.key === 'Enter') { e.preventDefault(); joinTenant(); } });

  // 初次渲染 + 拉取各租户里"我"的角色。
  render();
  loadRoles();
})();
