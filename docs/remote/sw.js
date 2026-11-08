/* 大黄智能浇水系统 · 离线壳
 * 只缓存页面本身与图标：让从桌面图标启动时，即使没网也能立刻出界面。
 * HTML 一律 network-first，保证 GitHub Pages 更新后不会被旧缓存锁死。
 * 巴法云 WSS / MQTT 是跨域且非同源静态资源，这里完全不拦截。
 */
const CACHE = 'dahuang-remote-v1';
const SHELL = [
  './',
  './index.html',
  './manifest.webmanifest',
  './icon-192.png',
  './icon-512.png',
  './apple-touch-icon.png'
];

self.addEventListener('install', function (e) {
  e.waitUntil(
    caches.open(CACHE)
      .then(function (c) { return c.addAll(SHELL); })
      .then(function () { return self.skipWaiting(); })
      .catch(function () { return self.skipWaiting(); })
  );
});

self.addEventListener('activate', function (e) {
  e.waitUntil(
    caches.keys()
      .then(function (keys) {
        return Promise.all(keys.filter(function (k) { return k !== CACHE; })
          .map(function (k) { return caches.delete(k); }));
      })
      .then(function () { return self.clients.claim(); })
  );
});

self.addEventListener('fetch', function (e) {
  const req = e.request;
  if (req.method !== 'GET') return;

  let url;
  try { url = new URL(req.url); } catch (err) { return; }
  if (url.origin !== self.location.origin) return;   // 跨域一律放行

  const isHtml = req.mode === 'navigate' ||
    (req.headers.get('accept') || '').indexOf('text/html') >= 0;

  if (isHtml) {
    e.respondWith(
      fetch(req).then(function (res) {
        if (res && res.ok) {
          const copy = res.clone();
          e.waitUntil(caches.open(CACHE).then(function (c) {
            return c.put('./index.html', copy);
          }).catch(function () {}));
        }
        return res;
      }).catch(function () {
        return caches.match('./index.html').then(function (hit) {
          return hit || caches.match('./');
        });
      })
    );
    return;
  }

  // 图标 / manifest 等静态资源：cache-first，命中即返回
  e.respondWith(
    caches.match(req).then(function (hit) {
      return hit || fetch(req);
    })
  );
});
