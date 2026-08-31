// ============================================================================
// IndexHtml.cpp —— DataHub 内置网页（精简单页，手机 / 电脑浏览器直接使用）
//
// 功能：
//   - 粘贴文本 → 生成提取码；输入提取码 → 获取文本
//   - 上传文件 → 生成提取码；输入提取码 → 下载文件
//   - 列表展示全部数据项，可一键复制提取码 / 下载 / 删除
// ============================================================================
#include "Module/HttpServerModule.h"

namespace datahub {

/// @brief 内置网页内容（HTML5 单页）。
const char* CHttpServerModule::IndexHtml()
{
    return R"html(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>DataHub · 设备间数据传输</title>
<style>
  :root { --bg:#f5f6f8; --card:#fff; --pri:#3b82f6; --pri-d:#2563eb; --text:#1f2937;
          --muted:#6b7280; --border:#e5e7eb; --ok:#16a34a; --err:#dc2626; }
  * { box-sizing:border-box; margin:0; padding:0; }
  body { font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"Helvetica Neue",Arial,
         "PingFang SC","Microsoft YaHei",sans-serif; background:var(--bg); color:var(--text);
         padding:16px; min-height:100vh; }
  .wrap { max-width:640px; margin:0 auto; }
  header { text-align:center; padding:8px 0 18px; }
  header h1 { font-size:24px; font-weight:700; letter-spacing:.5px; }
  header p { color:var(--muted); font-size:13px; margin-top:6px; }
  .card { background:var(--card); border:1px solid var(--border); border-radius:12px;
          padding:16px; margin-bottom:16px; box-shadow:0 1px 3px rgba(0,0,0,.05); }
  .card h2 { font-size:16px; margin-bottom:12px; display:flex; align-items:center; gap:8px; }
  .tabs { display:flex; gap:8px; margin-bottom:16px; }
  .tab { flex:1; text-align:center; padding:10px; border-radius:8px; cursor:pointer;
         background:var(--card); border:1px solid var(--border); color:var(--muted);
         font-size:14px; font-weight:600; transition:.15s; }
  .tab.active { background:var(--pri); border-color:var(--pri); color:#fff; }
  .panel { display:none; }
  .panel.active { display:block; }
  label { display:block; font-size:13px; color:var(--muted); margin-bottom:6px; }
  textarea, input[type=text], input[type=file] { width:100%; padding:10px 12px;
         border:1px solid var(--border); border-radius:8px; font-size:14px; outline:none;
         font-family:inherit; background:#fff; color:var(--text); }
  textarea { min-height:110px; resize:vertical; }
  textarea:focus, input[type=text]:focus { border-color:var(--pri); }
  .btn { display:inline-block; width:100%; padding:11px; border:none; border-radius:8px;
         background:var(--pri); color:#fff; font-size:15px; font-weight:600; cursor:pointer;
         margin-top:12px; transition:.15s; }
  .btn:hover { background:var(--pri-d); }
  .btn:disabled { opacity:.6; cursor:not-allowed; }
  .btn.ok { background:var(--ok); }
  .btn.ok:hover { background:#15803d; }
  .result { margin-top:12px; padding:10px 12px; border-radius:8px; font-size:13px;
            word-break:break-all; display:none; }
  .result.show { display:block; }
  .result.success { background:#ecfdf5; color:var(--ok); border:1px solid #a7f3d0; }
  .result.error { background:#fef2f2; color:var(--err); border:1px solid #fecaca; }
  .result .code { font-size:26px; font-weight:800; letter-spacing:6px; text-align:center;
                  padding:8px 0; user-select:all; }
  .item { display:flex; align-items:center; gap:10px; padding:10px 0;
          border-bottom:1px solid var(--border); font-size:14px; }
  .item:last-child { border-bottom:none; }
  .item .meta { flex:1; min-width:0; }
  .item .meta .name { font-weight:600; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .item .meta .sub { color:var(--muted); font-size:12px; margin-top:2px; }
  .item .id { font-family:ui-monospace,SFMono-Regular,Menlo,monospace; font-size:13px;
              color:var(--pri); cursor:pointer; padding:4px 8px; background:#eff6ff;
              border-radius:6px; user-select:all; }
  .item .ops { display:flex; gap:6px; }
  .item .ops a, .item .ops button { text-decoration:none; border:none; cursor:pointer;
              font-size:12px; padding:6px 10px; border-radius:6px; background:#f3f4f6;
              color:var(--text); }
  .item .ops a:hover { background:var(--pri); color:#fff; }
  .item .ops button.del:hover { background:var(--err); color:#fff; }
  .empty { text-align:center; color:var(--muted); padding:24px 0; font-size:14px; }
  .hint { font-size:12px; color:var(--muted); margin-top:8px; line-height:1.6; }
  .row { display:flex; gap:10px; }
  .row > * { flex:1; }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <h1>📦 DataHub</h1>
    <p>在不同设备之间传输文本与文件 · 同局域网内浏览器直接访问</p>
  </header>

  <div class="tabs">
    <div class="tab active" data-tab="share" onclick="switchTab('share')">📤 发送</div>
    <div class="tab" data-tab="get" onclick="switchTab('get')">📥 获取</div>
    <div class="tab" data-tab="list" onclick="switchTab('list'); loadList()">📋 列表</div>
  </div>

  <!-- 发送 -->
  <div class="panel active" id="panel-share">
    <div class="card">
      <h2>✏️ 粘贴文本</h2>
      <textarea id="txtContent" placeholder="在这里粘贴要发送的文本内容…"></textarea>
      <button class="btn" onclick="sendText()">生成提取码</button>
      <div class="result" id="resText"></div>
    </div>
    <div class="card">
      <h2>📎 上传文件</h2>
      <input type="file" id="fileInput">
      <button class="btn" onclick="sendFile()">上传并生成提取码</button>
      <div class="result" id="resFile"></div>
    </div>
  </div>

  <!-- 获取 -->
  <div class="panel" id="panel-get">
    <div class="card">
      <h2>🔑 输入提取码</h2>
      <input type="text" id="getCode" placeholder="6 位提取码" maxlength="6"
             autocomplete="off" autocapitalize="characters">
      <div class="row" style="margin-top:12px;">
        <button class="btn" onclick="getText()">获取文本</button>
        <button class="btn ok" onclick="getFile()">下载文件</button>
      </div>
      <div class="result" id="resGet"></div>
      <p class="hint">提示：点击右侧「📋 列表」可查看所有数据项的提取码。</p>
    </div>
  </div>

  <!-- 列表 -->
  <div class="panel" id="panel-list">
    <div class="card">
      <h2>📋 全部数据</h2>
      <div id="listWrap"><div class="empty">加载中…</div></div>
    </div>
  </div>
</div>

<script>
function switchTab(name) {
  document.querySelectorAll('.tab').forEach(function(t){
    t.classList.toggle('active', t.dataset.tab === name);
  });
  document.querySelectorAll('.panel').forEach(function(p){ p.classList.remove('active'); });
  document.getElementById('panel-' + name).classList.add('active');
}

function showResult(id, ok, msg, big) {
  var el = document.getElementById(id);
  el.className = 'result show ' + (ok ? 'success' : 'error');
  el.innerHTML = msg;
  if (big) el.scrollIntoView({behavior:'smooth', block:'center'});
}

function escapeHtml(s) {
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
                  .replace(/"/g,'&quot;').replace(/'/g,'&#39;');
}

async function postJson(url, body, headers) {
  var resp = await fetch(url, { method:'POST', body:body, headers:headers || {} });
  return await resp.json();
}

function sendText() {
  var content = document.getElementById('txtContent').value;
  if (!content.trim()) { showResult('resText', false, '内容不能为空'); return; }
  document.getElementById('resText').className = 'result show';
  document.getElementById('resText').textContent = '正在生成提取码…';
  fetch('/api/text', { method:'POST', body:content })
    .then(function(r){ return r.json(); })
    .then(function(j){
      if (j.id) showResult('resText', true,
        '提取码：<div class="code" onclick="copyCode(\'' + j.id + '\')">' + j.id + '</div>' +
        '<div style="text-align:center;">点击提取码可复制</div>', true);
      else showResult('resText', false, j.error || '发送失败');
    })
    .catch(function(){ showResult('resText', false, '网络错误'); });
}

function sendFile() {
  var input = document.getElementById('fileInput');
  if (!input.files || !input.files.length) { showResult('resFile', false, '请先选择文件'); return; }
  var file = input.files[0];
  document.getElementById('resFile').className = 'result show';
  document.getElementById('resFile').textContent = '正在上传 ' + file.name + ' …';
  fetch('/api/file', {
    method:'POST',
    headers:{ 'X-File-Name': encodeURIComponent(file.name) },
    body:file
  })
  .then(function(r){ return r.json(); })
  .then(function(j){
    if (j.id) showResult('resFile', true,
      '上传成功！提取码：<div class="code" onclick="copyCode(\'' + j.id + '\')">' + j.id + '</div>' +
      '<div style="text-align:center;">点击提取码可复制</div>', true);
    else showResult('resFile', false, j.error || '上传失败');
  })
  .catch(function(){ showResult('resFile', false, '网络错误'); });
}

function getText() {
  var code = document.getElementById('getCode').value.trim();
  if (!code) { showResult('resGet', false, '请输入提取码'); return; }
  fetch('/api/text/' + encodeURIComponent(code))
    .then(function(r){
      if (!r.ok) return r.json().then(function(j){ throw new Error(j.error || '获取失败'); });
      return r.text();
    })
    .then(function(text){
      showResult('resGet', true, '获取成功，内容如下：<br><pre style="white-space:pre-wrap;background:#f9fafb;padding:8px;border-radius:6px;margin-top:6px;font-size:13px;">' + escapeHtml(text) + '</pre>', true);
    })
    .catch(function(e){ showResult('resGet', false, e.message || '网络错误'); });
}

function getFile() {
  var code = document.getElementById('getCode').value.trim();
  if (!code) { showResult('resGet', false, '请输入提取码'); return; }
  window.location.href = '/api/file/' + encodeURIComponent(code);
}

function loadList() {
  var wrap = document.getElementById('listWrap');
  fetch('/api/list')
    .then(function(r){ return r.json(); })
    .then(function(j){
      var items = j.items || [];
      if (!items.length) { wrap.innerHTML = '<div class="empty">暂无数据</div>'; return; }
      var html = '';
      items.forEach(function(it){
        var name = it.type === 'text' ? '📝 ' + (it.name || '文本') : '📎 ' + (it.name || '文件');
        var size = formatSize(it.size);
        html += '<div class="item">' +
                '<div class="meta"><div class="name">' + escapeHtml(name) + '</div>' +
                '<div class="sub">' + size + ' · ' + new Date(it.time).toLocaleString() + '</div></div>' +
                '<div class="id" onclick="copyCode(\'' + it.id + '\')" title="点击复制">' + it.id + '</div>' +
                '<div class="ops">' +
                (it.type === 'file' ? '<a href="/api/file/' + it.id + '">下载</a>' :
                 '<button onclick="previewText(\'' + it.id + '\')">查看</button>') +
                '<button class="del" onclick="delItem(\'' + it.id + '\')">删除</button>' +
                '</div></div>';
      });
      wrap.innerHTML = html;
    })
    .catch(function(){ wrap.innerHTML = '<div class="empty">加载失败</div>'; });
}

function formatSize(n) {
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n/1024).toFixed(1) + ' KB';
  return (n/1048576).toFixed(1) + ' MB';
}

function previewText(id) {
  fetch('/api/text/' + id).then(function(r){ return r.text(); }).then(function(t){
    alert(t.length > 500 ? t.slice(0,500) + '\n…(内容过长已截断)' : t);
  });
}

function delItem(id) {
  if (!confirm('确定删除 ' + id + ' 吗？')) return;
  fetch('/api/item/' + id, { method:'DELETE' })
    .then(function(r){ return r.json(); })
    .then(function(j){ if (j.ok) loadList(); else alert(j.error || '删除失败'); })
    .catch(function(){ alert('网络错误'); });
}

function copyCode(code) {
  if (navigator.clipboard) {
    navigator.clipboard.writeText(code).then(function(){ toast('已复制: ' + code); });
  } else {
    var ta = document.createElement('textarea');
    ta.value = code; document.body.appendChild(ta); ta.select();
    document.execCommand('copy'); document.body.removeChild(ta);
    toast('已复制: ' + code);
  }
}

function toast(msg) {
  var el = document.createElement('div');
  el.style.cssText = 'position:fixed;bottom:24px;left:50%;transform:translateX(-50%);' +
    'background:#111827;color:#fff;padding:10px 18px;border-radius:8px;font-size:14px;' +
    'z-index:99;box-shadow:0 4px 12px rgba(0,0,0,.2);';
  el.textContent = msg;
  document.body.appendChild(el);
  setTimeout(function(){ el.remove(); }, 1500);
}
</script>
</body>
</html>
)html";
}

} // namespace datahub
