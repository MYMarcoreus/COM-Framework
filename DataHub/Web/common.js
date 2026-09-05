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

  // ---- 统一 fetch：自动附加 X-Client-Id（账号）与 X-Tenant（当前租户） ----
  // 已显式传入 X-Tenant 时不再覆写（管理页需按某租户查询角色时覆写）。
  DH.apiFetch = function (url, options) {
    options = options || {};
    options.headers = options.headers || {};
    options.headers['X-Client-Id'] = DH.CLIENT_ID;
    if (!options.headers['X-Tenant']) options.headers['X-Tenant'] = DH.currentCode;
    return fetch(url, options);
  };

  // 首次加载即恢复状态。
  DH.load();
})(window);
