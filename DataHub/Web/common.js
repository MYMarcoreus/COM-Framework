// DataHub 前端公共小件：客户端标识 + 租户列表状态 + 带租户头部的 fetch。
// 由聊天页（index.html → app.js）与租户管理页（tenants.html → tenants.js）
// 共同加载，保证两页共享同一账号与同一套"我的租户 / 当前租户"本地状态。
(function (global) {
  'use strict';

  var DH = global.DH = {};

  // ---- 客户端标识（X-Client-Id）：同一浏览器跨页面 / 跨请求保持一致 ----
  function getClientId() {
    var KEY = 'datahub_client_id';
    var id = '';
    try { id = localStorage.getItem(KEY) || ''; } catch (e) {}
    if (!id) {
      // 生成 UUID v4
      id = 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function (c) {
        var r = Math.random() * 16 | 0, v = c === 'x' ? r : (r & 0x3 | 0x8);
        return v.toString(16);
      });
      try { localStorage.setItem(KEY, id); } catch (e) {}
    }
    return id;
  }
  DH.CLIENT_ID = getClientId();

  // ---- 租户列表 / 当前租户（localStorage 持久化，两页共享） ----
  var KEY_LIST = 'datahub_tenants';          // [{code,name}]
  var KEY_CUR = 'datahub_current_tenant';    // code（缺省 'public'）
  var PUBLIC_NAME = '公共租户';

  DH.tenants = [];
  DH.currentCode = 'public';
  DH.currentName = PUBLIC_NAME;

  function persistList() {
    try { localStorage.setItem(KEY_LIST, JSON.stringify(DH.tenants)); } catch (e) {}
  }
  function persistCur() {
    try { localStorage.setItem(KEY_CUR, DH.currentCode); } catch (e) {}
  }
  function refreshName() {
    DH.currentName = DH.tenantNameOf(DH.currentCode);
  }

  // 从 localStorage 恢复租户列表与当前租户。
  DH.load = function () {
    try { DH.tenants = JSON.parse(localStorage.getItem(KEY_LIST) || '[]') || []; } catch (e) { DH.tenants = []; }
    DH.currentCode = 'public';
    try { DH.currentCode = localStorage.getItem(KEY_CUR) || 'public'; } catch (e) { DH.currentCode = 'public'; }
    refreshName();
  };

  DH.tenantNameOf = function (code) {
    if (code === 'public') return PUBLIC_NAME;
    for (var i = 0; i < DH.tenants.length; i++) {
      if (DH.tenants[i].code === code) return DH.tenants[i].name;
    }
    return code;
  };

  // 加入 / 更新租户到"我的租户"（幂等）。
  DH.addTenant = function (code, name) {
    if (!code) return;
    for (var i = 0; i < DH.tenants.length; i++) {
      if (DH.tenants[i].code === code) {
        if (name) DH.tenants[i].name = name;
        persistList();
        refreshName();
        return;
      }
    }
    DH.tenants.push({ code: code, name: name || code });
    persistList();
    refreshName();
  };

  // 从"我的租户"移除；若移除的恰是当前租户则回落公共租户。
  // @return true = 移除的是当前租户（调用方通常需重置页面视图）。
  DH.removeTenant = function (code) {
    if (code === 'public') return false;
    var wasCurrent = (DH.currentCode === code);
    var next = [];
    for (var i = 0; i < DH.tenants.length; i++) {
      if (DH.tenants[i].code !== code) next.push(DH.tenants[i]);
    }
    DH.tenants = next;
    persistList();
    if (wasCurrent) {
      DH.currentCode = 'public';
      persistCur();
    }
    refreshName();
    return wasCurrent;
  };

  // 切换当前租户（不操作"我的租户"列表）。
  DH.setCurrent = function (code) {
    DH.currentCode = code || 'public';
    persistCur();
    refreshName();
  };

  // ---- 设备令牌（X-Token）：登录即注册，服务端签发随机设备令牌并校验归属 ----
  // 身份 = X-Client-Id + X-Token 双因子式校验；服务端重启令牌表清空时，
  // 注册接口幂等返回新令牌，客户端自动续期（401 时 apiFetch 会自动重注册重试一次）。
  var KEY_TOKEN = 'datahub_device_token';
  DH.token = '';
  try { DH.token = localStorage.getItem(KEY_TOKEN) || ''; } catch (e) { DH.token = ''; }
  DH._tokenP = null;
  // @param force 强制重新注册（用于 401 续期）；始终幂等（服务端记住则原样返回）。
  DH.ensureDevice = function (force) {
    if (!force && DH.token) return Promise.resolve(DH.token);
    if (DH._tokenP) return DH._tokenP;
    DH._tokenP = fetch('/api/device/register', { method: 'POST', body: DH.CLIENT_ID })
      .then(function (r) { return r.json(); })
      .then(function (j) {
        DH.token = j.token || '';
        if (DH.token) { try { localStorage.setItem(KEY_TOKEN, DH.token); } catch (e) {} }
        DH._tokenP = null;
        return DH.token;
      })
      .catch(function () { DH._tokenP = null; return ''; });
    return DH._tokenP;
  };

  // ---- 当前租户状态对账（B2/B6）：周期调用，检测改名 / 被踢 / 租户被删 ----
  // 返回 {changed, kicked, renamed}；changed=true 时调用方应重置视图（本函数已回落清理）。
  DH.reconcileCurrent = function () {
    if (!DH.currentCode || DH.currentCode === 'public') return Promise.resolve({ changed: false });
    return DH.apiFetch('/api/tenant/state')
      .then(function (r) { return r.json(); })
      .then(function (st) {
        st = st || {};
        // 不存在（被删）或非公共且角色为空（被踢）→ 从"我的租户"移除并回落公共。
        if (st.exists === false || (st.code && st.code !== 'public' && !st.role)) {
          var wasCurrent = DH.removeTenant(st.code || DH.currentCode);
          return { changed: wasCurrent, kicked: true, deleted: (st.exists === false) };
        }
        // 改名：刷新本地缓存与当前显示名。
        if (st.exists && st.name && st.name !== DH.tenantNameOf(st.code)) {
          DH.addTenant(st.code, st.name);
          if (st.code === DH.currentCode) refreshName();
          return { changed: false, renamed: true, name: st.name };
        }
        return { changed: false };
      })
      .catch(function () { return { changed: false }; });
  };

  // ---- 统一 fetch：自动附加 X-Client-Id（账号）/ X-Tenant（当前租户）/ X-Token（设备令牌） ----
  // 已显式传入 X-Tenant 时不再覆写（管理页需按某租户查询角色时覆写）。
  // 返回 Promise<Response>（先确保令牌已注册；401 时自动续期重试一次）。
  DH.apiFetch = function (url, options) {
    options = options || {};
    options.headers = options.headers || {};
    options.headers['X-Client-Id'] = DH.CLIENT_ID;
    if (!options.headers['X-Tenant']) options.headers['X-Tenant'] = DH.currentCode;
    return DH.ensureDevice().then(function (token) {
      if (token) options.headers['X-Token'] = token;
      return fetch(url, options).then(function (resp) {
        // 服务端重启令牌表清空 → 401：幂等重注册一次再重试（防 stale 循环）。
        if (resp.status === 401 && !options._retried) {
          options._retried = true;
          return DH.ensureDevice(true).then(function () { return fetch(url, options); });
        }
        return resp;
      });
    });
  };

  // 首次加载即恢复状态。
  DH.load();
})(window);
