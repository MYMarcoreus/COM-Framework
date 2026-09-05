// DataHubAdmin 服务端管理页逻辑：经控制面代理访问 DataHub /api/admin/*。
(function () {
  'use strict';
  function $(id) { return document.getElementById(id); }
  function esc(s) {
    return String(s == null ? '' : s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  }
  function toast(msg) {
    var t = $('msg'); t.textContent = msg; t.style.display = 'block';
    clearTimeout(t._tm); t._tm = setTimeout(function () { t.style.display = 'none'; }, 2000);
  }
  function fmtBytes(n) {
    n = Number(n) || 0;
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
    return (n / 1048576).toFixed(1) + ' MB';
  }
  function fmtTime(ms) {
    if (!ms) return '-';
    var d = new Date(ms), p = function (x) { return (x < 10 ? '0' : '') + x; };
    return d.getFullYear() + '-' + p(d.getMonth() + 1) + '-' + p(d.getDate()) + ' ' + p(d.getHours()) + ':' + p(d.getMinutes());
  }
  function api(url, options) { return fetch(url, options).then(function (r) { return r.json().catch(function () { return {}; }); }); }
  function form(fields) {
    var s = new URLSearchParams();
    Object.keys(fields).forEach(function (k) { s.set(k, String(fields[k] == null ? '' : fields[k])); });
    return { method: 'POST', headers: { 'Content-Type': 'application/x-www-form-urlencoded' }, body: s.toString() };
  }
  function limitsText(t) {
    var parts = [];
    if (t.maxItems > 0) parts.push('≤' + t.maxItems + ' 条');
    if (t.maxTotalBytes > 0) parts.push('总量 ' + fmtBytes(t.maxTotalBytes));
    if (t.maxItemBytes > 0) parts.push('单条 ' + fmtBytes(t.maxItemBytes));
    return parts.length ? parts.join(' / ') : '不限';
  }

  // ---- 一览 ----
  function loadOverview() {
    api('/api/admin/overview').then(function (j) {
      var arr = j.tenants || [];
      $('sCount').textContent = j.count != null ? j.count : arr.length;
      $('sItems').textContent = j.totalItems != null ? j.totalItems : '-';
      $('sBytes').textContent = fmtBytes(j.totalBytes);
      var html = '';
      arr.forEach(function (t) {
        var dft = t.isDefault ? ' <span class="badge dft">公共</span>' : '';
        html += '<div class="row">' +
          '<b>' + esc(t.name) + '</b>' + dft +
          '<span class="code">' + esc(t.code) + '</span>' +
          '<span class="stat">成员 ' + t.members + '</span>' +
          '<span class="stat">条目 ' + t.items + '</span>' +
          '<span class="stat">' + fmtBytes(t.bytes) + '</span>' +
          '<span class="stat">' + esc(limitsText(t)) + '</span>' +
          '<span class="sp"></span>' +
          '<button class="btn ghost" onclick="__show(\'' + esc(t.code) + '\')">详情</button>' +
          '</div>';
      });
      $('tenantList').innerHTML = arr.length ? html : '<div style="color:var(--muted);font-size:13px;">暂无租户</div>';
    }).catch(function () { $('tenantList').innerHTML = '<div style="color:var(--danger)">加载失败（确认上游 DataHub 已启动）</div>'; });
  }

  // ---- 详情 ----
  function showTenant(code) {
    api('/api/admin/tenant?code=' + encodeURIComponent(code)).then(function (t) {
      if (t.error) { toast(t.error); return; }
      $('detailPanel').style.display = '';
      var dft = t.isDefault ? ' <span class="badge dft">公共租户（不可删）</span>' : '';
      var nItems = (t.count != null) ? t.count : ((t.items || []).length);
      var h = '<div class="row"><b>' + esc(t.name) + '</b>' + dft +
        '<span class="code">' + esc(t.code) + '</span>' +
        '<span class="stat">条目 ' + nItems + ' · ' + fmtBytes(t.bytes) + '</span>' +
        '<span class="stat">配额 ' + esc(limitsText(t)) + '</span><span class="sp"></span></div>';

      // 成员
      h += '<div style="font-weight:600;margin:10px 0 4px;">成员（' + (t.members || []).length + '）</div>';
      (t.members || []).forEach(function (m) {
        h += '<div class="row">' +
          '<span class="code">' + esc(m.account) + '</span>' +
          (m.role === 'owner' ? '<span class="badge owner">Owner</span>' : '<span class="badge">成员</span>') +
          '<span class="stat">加入 ' + fmtTime(m.joined) + '</span>' +
          '<span class="sp"></span>' +
          '<select data-code="' + esc(t.code) + '" data-account="' + esc(m.account) + '" onchange="__role(this)">' +
          '<option value="owner"' + (m.role === 'owner' ? ' selected' : '') + '>Owner</option>' +
          '<option value="member"' + (m.role === 'member' ? ' selected' : '') + '>成员</option></select>' +
          (t.isDefault ? '' : '<button class="btn danger" onclick="__kick(\'' + esc(t.code) + '\',\'' + esc(m.account) + '\')">移除</button>') +
          '</div>';
      });

      // 数据条目
      h += '<div style="font-weight:600;margin:10px 0 4px;">数据条目（' + (t.items || []).length + '）</div>';
      if (!(t.items || []).length) {
        h += '<div style="color:var(--muted);font-size:13px;">暂无数据</div>';
      }
      (t.items || []).forEach(function (it) {
        h += '<div class="row item-line">' +
          (it.kind === 'text' ? '💬 文本' : '📎 文件') +
          (it.name ? ' ' + esc(it.name) : '') +
          '<span class="code">' + esc(it.id) + '</span>' +
          '<span class="stat">来自 ' + esc(it.from) + '</span>' +
          '<span class="stat">' + fmtBytes(it.size) + ' · ' + fmtTime(it.created) + '</span>' +
          '<span class="sp"></span>' +
          (it.kind === 'text' ? '<button class="btn ghost" onclick="__preview(\'' + esc(t.code) + '\',\'' + esc(it.id) + '\')">预览</button>' : '') +
          '<button class="btn danger" onclick="__delItem(\'' + esc(t.code) + '\',\'' + esc(it.id) + '\')">删除</button></div>';
      });

      // 租户操作
      if (t.isDefault) {
        h += '<div class="grp"><span style="color:var(--muted);font-size:12px;">公共租户仅可改名/调配额。</span></div>';
      }
      h += '<div class="grp" style="margin-top:12px;border-top:1px solid var(--border);padding-top:10px;">' +
        '<b style="font-size:13px;">租户操作</b></div>';
      h += '<div class="grp"><label>名称</label><input id="dName" maxlength="48" value="' + esc(t.name) + '" style="min-width:140px">' +
        '<button class="btn" onclick="__rename(\'' + esc(t.code) + '\')">保存名称</button></div>';
      if (!t.isDefault) {
        h += '<div class="grp"><label>条数上限</label><input id="dItems" type="number" min="0" value="' + (t.maxItems || 0) + '" style="width:90px">' +
          '<label>总量(MB)</label><input id="dTotal" type="number" min="0" value="' + (Math.round((t.maxTotalBytes || 0) / 1048576)) + '" style="width:90px">' +
          '<label>单条(MB)</label><input id="dItem" type="number" min="0" value="' + (Math.round((t.maxItemBytes || 0) / 1048576)) + '" style="width:90px">' +
          '<button class="btn" onclick="__limits(\'' + esc(t.code) + '\')">保存配额</button></div>';
        h += '<div class="grp"><button class="btn danger" onclick="__delTenant(\'' + esc(t.code) + '\')">删除该租户（含数据）</button></div>';
      }
      $('dTitle').innerHTML = '租户详情 · ' + esc(t.name);
      $('dBody').innerHTML = h;
    }).catch(function () { toast('加载失败'); });
  }

  // ---- 操作 ----
  function rename(code) { api('/api/admin/tenant/rename', form({ code: code, name: $('dName').value })).then(function (j) { if (j.ok) { toast('已重命名'); showTenant(code); loadOverview(); } else toast(j.error || '失败'); }); }
  function setLimits(code) {
    var mb = function (v) { v = parseInt(v, 10) || 0; return v > 0 ? v * 1048576 : 0; };
    api('/api/admin/tenant/limits', form({ code: code, maxItems: $('dItems').value, maxTotalBytes: mb($('dTotal').value), maxItemBytes: mb($('dItem').value) })).then(function (j) { if (j.ok) { toast('配额已更新'); showTenant(code); loadOverview(); } else toast(j.error || '失败'); });
  }
  function setRole(sel) {
    api('/api/admin/member/role', form({ code: sel.getAttribute('data-code'), account: sel.getAttribute('data-account'), role: sel.value })).then(function (j) {
      if (j.ok) { toast('角色已更新'); showTenant(sel.getAttribute('data-code')); } else toast(j.error || '失败（需保留至少一名 Owner）');
    });
  }
  function kick(code, account) {
    api('/api/admin/member?code=' + encodeURIComponent(code) + '&account=' + encodeURIComponent(account), { method: 'DELETE' }).then(function (j) { if (j.ok) { toast('已移除成员'); showTenant(code); loadOverview(); } else toast(j.error || '失败（需保留 Owner）'); });
  }
  function preview(code, id) {
    api('/api/admin/item?code=' + encodeURIComponent(code) + '&id=' + encodeURIComponent(id)).then(function (j) {
      if (j.error) { toast(j.error); return; }
      var el = document.getElementById('dPreview');
      if (!el) { el = document.createElement('div'); el.id = 'dPreview'; el.className = 'preview'; $('dBody').appendChild(el); }
      el.textContent = (j.text != null) ? j.text : ('[文件 ' + j.name + '，' + fmtBytes(j.size) + ']');
    });
  }
  function delItem(code, id) {
    api('/api/admin/item?code=' + encodeURIComponent(code) + '&id=' + encodeURIComponent(id), { method: 'DELETE' }).then(function (j) { if (j.ok) { toast('已删除条目'); showTenant(code); loadOverview(); } else toast(j.error || '失败'); });
  }
  function delTenant(code) {
    if (!confirm('确认删除租户 ' + code + ' 及其全部数据？此操作不可恢复。')) return;
    api('/api/admin/tenant?code=' + encodeURIComponent(code), { method: 'DELETE' }).then(function (j) {
      if (j.ok) { toast('已删除租户'); $('detailPanel').style.display = 'none'; loadOverview(); } else toast(j.error || '失败');
    });
  }

  window.__show = showTenant;
  window.__rename = rename;
  window.__limits = setLimits;
  window.__role = setRole;
  window.__kick = kick;
  window.__preview = preview;
  window.__delItem = delItem;
  window.__delTenant = delTenant;
  window.loadOverview = loadOverview;

  loadOverview();
})();
