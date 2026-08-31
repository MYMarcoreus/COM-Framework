// DataHub 聊天室前端逻辑
(function () {
  'use strict';

  // ============ 状态 ============
  var seen = {};      // 已渲染的消息 id → true
  var POLL_MS = 2000; // 轮询间隔
  var membersShown = false;

  // ============ 客户端标识（跨请求标识同一浏览器，避免成员按端口膨胀） ============
  function getClientId() {
    var KEY = 'datahub_client_id';
    var id = '';
    try { id = localStorage.getItem(KEY) || ''; } catch(e){}
    if (!id) {
      // 生成 UUID v4
      id = 'xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx'.replace(/[xy]/g, function(c){
        var r = Math.random()*16|0, v = c==='x' ? r : (r&0x3|0x8);
        return v.toString(16);
      });
      try { localStorage.setItem(KEY, id); } catch(e){}
    }
    return id;
  }
  var CLIENT_ID = getClientId();

  // 统一 fetch：自动附加 X-Client-Id 请求头（成员标识）。
  function apiFetch(url, options) {
    options = options || {};
    options.headers = options.headers || {};
    options.headers['X-Client-Id'] = CLIENT_ID;
    return fetch(url, options);
  }

  // ============ 分段拉取文件 ============
  // workflow 对单次超大响应体在部分网络环境下会截断（约 110KB），故大文件
  // 采用 HTTP Range 分段拉取再拼接，每次请求都小于阈值，保证可靠传输。
  var CHUNK_SIZE = 64 * 1024;   // 64KB/段，远小于 110KB 阈值
  function mimeOf(name) {
    name = String(name || '').toLowerCase();
    var map = {
      '.png':'image/png', '.jpg':'image/jpeg', '.jpeg':'image/jpeg',
      '.gif':'image/gif', '.webp':'image/webp', '.bmp':'image/bmp',
      '.svg':'image/svg+xml', '.ico':'image/x-icon',
      '.txt':'text/plain', '.log':'text/plain', '.json':'application/json',
      '.html':'text/html', '.htm':'text/html', '.css':'text/css', '.js':'text/javascript',
      '.pdf':'application/pdf', '.zip':'application/zip', '.gz':'application/gzip',
      '.tar':'application/x-tar', '.mp3':'audio/mpeg', '.wav':'audio/wav',
      '.mp4':'video/mp4', '.webm':'video/webm', '.mov':'video/quicktime',
      '.doc':'application/msword', '.docx':'application/vnd.openxmlformats-officedocument.wordprocessingml.document',
      '.xls':'application/vnd.ms-excel', '.xlsx':'application/vnd.openxmlformats-officedocument.spreadsheetml.sheet',
      '.ppt':'application/vnd.ms-powerpoint', '.pptx':'application/vnd.openxmlformats-officedocument.presentationml.presentation'
    };
    var dot = name.lastIndexOf('.');
    if (dot >= 0) {
      var ext = name.slice(dot).toLowerCase();
      if (map[ext]) return map[ext];
    }
    return 'application/octet-stream';
  }
  function fetchFile(id, name) {
    var url = '/api/file/' + encodeURIComponent(id);
    return apiFetch(url, { headers: { 'Range': 'bytes=0-0' } })
      .then(function(r){
        if (!r.ok) throw new Error('文件不存在 (' + r.status + ')');
        // 读取 Content-Range: bytes 0-0/total
        var cr = r.headers.get('Content-Range') || '';
        var m = /\/\s*(\d+)/.exec(cr);
        if (!m) {
          // 服务器未支持 Range（小文件完整返回）——直接读完整 body
          return apiFetch(url).then(function(r2){
            if (!r2.ok) throw new Error('加载失败 (' + r2.status + ')');
            return r2.blob();
          });
        }
        var total = parseInt(m[1], 10);
        if (total <= 0) return new Blob([]);
        // 分段拉取
        var parts = [];
        var done = 0;
        var fetchOne = function(start) {
          var end = Math.min(start + CHUNK_SIZE - 1, total - 1);
          return apiFetch(url, { headers: { 'Range': 'bytes=' + start + '-' + end } })
            .then(function(r){
              if (!r.ok) throw new Error('分段拉取失败 (' + r.status + ')');
              return r.arrayBuffer();
            })
            .then(function(buf){
              parts.push(new Uint8Array(buf));
              done += buf.byteLength;
            });
        };
        var chain = Promise.resolve();
        for (var s = 0; s < total; s += CHUNK_SIZE) {
          (function(start){ chain = chain.then(function(){ return fetchOne(start); }); })(s);
        }
        return chain.then(function(){
          var merged = new Uint8Array(total);
          var off = 0;
          for (var i = 0; i < parts.length; i++) {
            merged.set(parts[i], off);
            off += parts[i].length;
          }
          // 带 MIME type 的 Blob：下载时浏览器才能识别为图片/文件而非 .txt
          return new Blob([merged.buffer], { type: mimeOf(name) });
        });
      });
  }

  // ============ DOM 工具 ============
  function $(id) { return document.getElementById(id); }
  function escapeHtml(s) {
    return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
      .replace(/"/g,'&quot;').replace(/'/g,'&#39;');
  }
  function fmtSize(n) {
    if (n < 1024) return n + ' B';
    if (n < 1048576) return (n/1024).toFixed(1) + ' KB';
    return (n/1048576).toFixed(1) + ' MB';
  }
  function fmtTime(ms) {
    var d = new Date(ms);
    function p(x){ return (x<10?'0':'')+x; }
    return p(d.getHours()) + ':' + p(d.getMinutes());
  }
  function fmtDate(ms) {
    var d = new Date(ms), now = new Date();
    var today = new Date(now.getFullYear(), now.getMonth(), now.getDate());
    var that = new Date(d.getFullYear(), d.getMonth(), d.getDate());
    var diffDays = Math.round((today - that) / 86400000);
    if (diffDays === 0) return '今天';
    if (diffDays === 1) return '昨天';
    return (d.getMonth()+1) + '月' + d.getDate() + '日';
  }
  // 客户端标识短名：UUID 前 8 位（成员展示用）。
  function shortAddr(id) {
    if (!id) return '未知';
    if (id.indexOf(':') >= 0) return id; // 兼容 IP:port 形式
    return id.slice(0, 8);
  }
  function avatarText(id) {
    if (!id) return '?';
    if (id.indexOf(':') >= 0) {
      var segs = (id.split(':')[0] || id).split('.');
      return segs.length === 4 ? segs[3] : '?';
    }
    return id.slice(0, 2).toUpperCase();
  }
  function toast(msg) {
    var t = $('toast');
    t.textContent = msg; t.style.display = 'block';
    clearTimeout(t._tm);
    t._tm = setTimeout(function(){ t.style.display = 'none'; }, 1800);
  }

  // ============ 渲染消息 ============
  var chatEl = $('chat');
  var lastDateKey = '';
  function scrollBottom() { chatEl.scrollTop = chatEl.scrollHeight; }

  function isImageName(name) {
    return /\.(png|jpe?g|gif|webp|bmp|svg)$/i.test(name || '');
  }

  function renderMsg(id, type, name, content, mineFlag, from, size, timeMs) {
    if (seen[id]) return;
    seen[id] = true;

    // 时间分组（跨天分隔）
    var d = new Date(timeMs || Date.now());
    var dateKey = fmtDate(d.getTime());
    if (dateKey !== lastDateKey) {
      var sep = document.createElement('div');
      sep.className = 'sys';
      sep.innerHTML = '<span>' + dateKey + ' ' + fmtTime(d.getTime()) + '</span>';
      chatEl.appendChild(sep);
      lastDateKey = dateKey;
    }

    var row = document.createElement('div');
    row.className = 'msg ' + (mineFlag ? 'mine' : 'other');

    var avatar = document.createElement('div');
    avatar.className = 'avatar';
    avatar.textContent = mineFlag ? '我' : avatarText(from);

    var body;
    if (type === 'file') {
      if (isImageName(name)) {
        // 图片文件：内联预览（点击放大查看原图）。用分段拉取避免大图失败。
        var wrap = document.createElement('div');
        wrap.className = 'img-wrap';
        body = wrap;
        var imgBox = document.createElement('div');
        imgBox.className = 'img-bubble';
        imgBox.onclick = function(){ viewImage(id, name); };
        var img = document.createElement('img');
        img.alt = name;
        img.onclick = function(ev){ ev.stopPropagation(); viewImage(id, name); };
        imgBox.appendChild(img);
        wrap.appendChild(imgBox);
        // 下载按钮（与文件卡片一致）
        var dl = document.createElement('div');
        dl.className = 'img-dl';
        dl.textContent = '下载';
        dl.onclick = function(ev){
          ev.stopPropagation();
          downloadFile(id, name);
        };
        wrap.appendChild(dl);
        img.classList.add('loading');
        fetchFile(id, name).then(function(blob){
          if (!img._released) {
            img.src = URL.createObjectURL(blob);
            img._url = URL.createObjectURL(blob);
          }
          img.classList.remove('loading');
        }).catch(function(){ img.classList.remove('loading'); });
      } else {
        body = document.createElement('div');
        body.className = 'file-card';
        body.onclick = function(){ downloadFile(id, name); };
        body.innerHTML =
          '<div class="ficon">📎</div>' +
          '<div class="fmeta">' +
            '<div class="fname">' + escapeHtml(name || 'file.bin') + '</div>' +
            '<div class="finfo">' + fmtSize(size) + ' · 点击下载</div>' +
          '</div>' +
          '<div class="fdown">下载</div>';
      }
    } else {
      body = document.createElement('div');
      body.className = 'bubble';
      body.innerHTML =
        '<div class="content">' + escapeHtml(content) + '</div>' +
        '<div class="meta-row">' +
          '<span class="sender">' + (mineFlag ? '我' : escapeHtml(shortAddr(from))) + '</span>' +
          '<span class="time">' + fmtTime(d.getTime()) + '</span>' +
        '</div>';
    }

    row.appendChild(avatar);
    row.appendChild(body);
    chatEl.appendChild(row);
    scrollBottom();
  }

  // ============ 成员 ============
  function toggleMembers() {
    membersShown = !membersShown;
    $('members').classList.toggle('show', membersShown);
    if (membersShown) loadMembers();
  }
  // 暴露给内联 onclick（HTML 中的 onclick="toggleMembers()" 在全局作用域查找）
  window.toggleMembers = toggleMembers;

  function loadMembers() {
    apiFetch('/api/members')
      .then(function(r){ return r.json(); })
      .then(function(j){
        var list = $('memberList');
        var arr = j.members || [];
        if (!arr.length) { list.innerHTML = '<div style="color:var(--muted);font-size:13px;">暂无成员</div>'; return; }
        var html = '';
        arr.forEach(function(m){
          var isMe = (m.id === CLIENT_ID);
          var cls = isMe ? ' style="color:#6366f1;font-weight:600;"' : '';
          html += '<div class="mrow">' +
            '<span class="mdot"></span>' +
            '<span class="maddr"' + cls + '>' + escapeHtml(m.ip || '未知') + (isMe ? '（我）' : '') + '</span>' +
            '<span class="mlast">' + fmtTime(m.last) + '</span>' +
          '</div>';
        });
        list.innerHTML = html;
      })
      .catch(function(){ $('memberList').innerHTML = '<div style="color:var(--muted);font-size:13px;">加载失败</div>'; });
  }

  // ============ 发送文本 ============
  function sendText() {
    var input = $('input');
    var text = input.value.trim();
    if (!text) return;
    input.value = ''; input.style.height = 'auto';
    apiFetch('/api/text', { method:'POST', body:text })
      .then(function(r){ return r.json(); })
      .then(function(j){
        if (j.id) {
          renderMsg(j.id, 'text', '', text, true, CLIENT_ID, 0, Date.now());
        } else toast(j.error || '发送失败');
      })
      .catch(function(){ toast('网络错误'); });
  }

  // ============ 发送文件 ============
  function sendFile(file) {
    toast('上传中 ' + file.name + ' …');
    apiFetch('/api/file', {
      method:'POST',
      headers:{ 'X-File-Name': encodeURIComponent(file.name) },
      body:file
    })
      .then(function(r){ return r.json(); })
      .then(function(j){
        if (j.id) {
          renderMsg(j.id, 'file', file.name, '', true, CLIENT_ID, file.size, Date.now());
        } else toast(j.error || '上传失败');
      })
      .catch(function(){ toast('网络错误'); });
  }

  // ============ 轮询（同步消息 + 成员） ============
  function poll() {
    apiFetch('/api/list')
      .then(function(r){ return r.json(); })
      .then(function(j){
        var items = j.items || [];
        $('statusText').textContent = '在线 · ' + items.length + ' 条';
        var arr = items.slice().reverse(); // 从旧到新
        arr.forEach(function(it){
          // 通过 from（客户端标识）判断是否自己发送：刷新后仍正确。
          var mineFlag = (it.from === CLIENT_ID);
          if (it.type === 'file') {
            renderMsg(it.id, 'file', it.name, '', mineFlag, it.from, it.size, it.time);
          } else {
            if (!seen[it.id]) {
              apiFetch('/api/text/' + encodeURIComponent(it.id))
                .then(function(r){ return r.text(); })
                .then(function(text){
                  renderMsg(it.id, 'text', '', text, it.from === CLIENT_ID, it.from, 0, it.time);
                })
                .catch(function(){});
            }
          }
        });
        scrollBottom();
      })
      .catch(function(){ $('statusText').textContent = '离线'; });

    // 成员（非展开时也更新计数）
    apiFetch('/api/members')
      .then(function(r){ return r.json(); })
      .then(function(j){
        var n = (j.members || []).length;
        $('statusText').textContent = n + ' 人在线 · ' + chatEl.querySelectorAll('.msg').length + ' 条';
        if (membersShown) loadMembers();
      })
      .catch(function(){});
  }

  // ============ 文件下载 / 图片预览 ============
  // 分段拉取后再下载，避免大文件在部分网络环境被截断。
  function downloadFile(id, name) {
    toast('正在下载 ' + (name || '文件') + ' …');
    fetchFile(id, name)
      .then(function(blob){
        var url = URL.createObjectURL(blob);
        var a = document.createElement('a');
        a.href = url;
        a.download = name || 'download.bin';
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        setTimeout(function(){ URL.revokeObjectURL(url); }, 5000);
        toast('下载完成');
      })
      .catch(function(e){ toast(e.message || '下载失败'); });
  }

  // 图片点击：先预览原图，底部提供"下载原图"。
  function viewImage(id, name) {
    toast('加载原图 …');
    $('overlayName').textContent = name || '';
    $('overlayDl').onclick = function(){ downloadFile(id, name); };
    fetchFile(id, name)
      .then(function(blob){
        var url = URL.createObjectURL(blob);
        $('overlayImg').src = url;
        $('overlay').classList.add('show');
        $('overlay')._url = url;
      })
      .catch(function(){ toast('图片加载失败'); });
  }
  $('overlay').addEventListener('click', function(){
    this.classList.remove('show');
    if (this._url) { URL.revokeObjectURL(this._url); this._url = null; }
  });

  // ============ 事件绑定 ============
  $('sendBtn').addEventListener('click', sendText);
  $('input').addEventListener('keydown', function(e){
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); sendText(); }
  });
  $('input').addEventListener('input', function(){
    this.style.height = 'auto';
    this.style.height = Math.min(this.scrollHeight, 100) + 'px';
  });
  $('plusBtn').addEventListener('click', function(){ $('fileInput').click(); });
  $('fileInput').addEventListener('change', function(){
    if (this.files && this.files.length) sendFile(this.files[0]);
    this.value = '';
  });

  // ============ 移动端键盘适配 ============
  // visualViewport：键盘弹出时调整视口高度，保证输入栏可见
  if (window.visualViewport) {
    window.visualViewport.addEventListener('resize', function(){
      // 让输入栏贴住键盘顶
      var vvh = window.visualViewport.height;
      document.body.style.height = vvh + 'px';
      scrollBottom();
    });
  }

  // ============ 启动 ============
  poll();
  setInterval(poll, POLL_MS);
  setInterval(loadMembers, POLL_MS); // 后台更新成员（用于"我"地址推断）
})();
