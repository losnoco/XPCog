// The /docs page's own script.
//
// A file rather than an inline <script>, and that is the Content-Security-Policy
// talking rather than taste. The page is served with `script-src 'self'`, which
// blocks inline execution outright -- the alternatives were 'unsafe-inline',
// which gives up most of what the policy is for, or a sha256 hash of this block
// that would have to be recomputed and kept in step every time a line here
// changed. Serving it from the same origin needs neither.
//
// It does two things: keeps the access token for the tab, and attaches it to
// everything Swagger UI sends.

(function () {
  'use strict';

  var KEY = 'xpcog-remote-token';
  var field = document.getElementById('token');

  // sessionStorage throws outright in some private-browsing modes rather than
  // simply being empty, so every touch of it is guarded.
  try {
    field.value = sessionStorage.getItem(KEY) || '';
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
    try {
      sessionStorage.setItem(KEY, currentToken());
    } catch (e) { /* no storage; it still works for this page load */ }
    // Re-fetch, so a token typed in after the page loaded takes effect without a
    // reload the reader has to think to do.
    ui.specActions.download('/openapi.json');
  });

  field.addEventListener('keydown', function (event) {
    if (event.key === 'Enter') {
      document.getElementById('apply').click();
    }
  });
})();
