// The /docs page's own script.
//
// A file rather than an inline <script>, and that is the Content-Security-Policy
// talking rather than taste. The page is served with `script-src 'self'`, which
// blocks inline execution outright -- the alternatives were 'unsafe-inline',
// which gives up most of what the policy is for, or a sha256 hash of this block
// that would have to be recomputed and kept in step every time a line here
// changed. Serving it from the same origin needs neither.
//
// It does two things: keeps the access token in this browser, and attaches it to
// everything Swagger UI sends.
//
// localStorage rather than sessionStorage, which is what this used at first: a
// token that had to be pasted in again for every new tab meant going back to the
// preferences pane to copy it, which is the friction that makes a reader keep the
// credential somewhere worse. It is a per-origin store, so it is readable only by
// a page served from this player's own address and port; a "Forget" button empties
// it. Server with `remoteLoopbackNoToken` on and reached from this machine, the
// field can stay empty altogether.

(function () {
  'use strict';

  var KEY = 'xpcog-remote-token';
  var field = document.getElementById('token');

  // localStorage throws outright in some private-browsing modes rather than
  // simply being empty, so every touch of it is guarded.
  function remember(value) {
    try {
      if (value) {
        localStorage.setItem(KEY, value);
      } else {
        localStorage.removeItem(KEY);
      }
    } catch (e) { /* no storage; it still works for this page load */ }
  }

  try {
    field.value = localStorage.getItem(KEY) || '';
  } catch (e) { /* no storage; the field just starts empty */ }

  function currentToken() {
    return field.value.trim();
  }

  var ui = SwaggerUIBundle({
    // Fetched with the token like everything else, so this document is not a way
    // to learn the shape of the API without one.
    url: '/openapi.json',
    dom_id: '#swagger-ui',
    presets: [SwaggerUIBundle.presets.apis],
    layout: 'BaseLayout',
    deepLinking: true,
    tryItOutEnabled: true,
    requestInterceptor: function (request) {
      var token = currentToken();
      if (token) {
        request.headers['Authorization'] = 'Bearer ' + token;
      }
      return request;
    }
  });

  document.getElementById('apply').addEventListener('click', function () {
    remember(currentToken());
    // Re-fetch, so a token typed in after the page loaded takes effect without a
    // reload the reader has to think to do.
    ui.specActions.download('/openapi.json');
  });

  // The way back out of the browser's store, on a shared machine or a borrowed
  // one. It clears the field as well as the store, so what the page shows and
  // what it will send are never two different things.
  document.getElementById('forget').addEventListener('click', function () {
    field.value = '';
    remember('');
    ui.specActions.download('/openapi.json');
  });

  field.addEventListener('keydown', function (event) {
    if (event.key === 'Enter') {
      document.getElementById('apply').click();
    }
  });
})();
