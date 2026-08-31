# Swagger UI

The browser page behind the remote control's `/docs`, vendored rather than
loaded from a CDN so that the player serves a working page with no internet at
all — which is the ordinary case for something bound to loopback.

`MANIFEST` records the version, the licence and four hashes; see it before
changing anything here.

## What this costs

It is 409 KB of minified JavaScript that nobody reviews, and a standing
obligation to update it when a CVE lands against it. That is a real maintenance
cost and it is named here rather than discovered later. The mitigations are the
ones available: an exact pinned version, hashes in `MANIFEST`, and
`tests/core/test_remote_assets.cpp` asserting the embedded bytes still match,
so a replacement is loud.

What limits the damage if it were ever compromised is the page's own
constraints. `/docs` is served with a Content-Security-Policy of `default-src
'none'` plus `'self'` for scripts, styles and fetches, so the page can reach
nothing but this server; there are no CORS headers anywhere, so no other origin
can reach the API; and the token is typed into the page and kept in
`sessionStorage` rather than being an ambient credential the browser would
attach on its own.

## The page itself

`index.html` and `docs.js` are ours, not upstream's. They differ from the stock
Swagger UI page in one way that matters: a browser cannot attach `Authorization:
Bearer` to a top-level navigation, so `/docs` and the three files it loads are the
only unauthenticated things the server has. The page asks for the token, keeps it
in `sessionStorage`, and installs a `requestInterceptor` that attaches it to the
spec fetch and to every try-it-out call. Everything the page can actually *do*
still needs the token.

**The script is a separate file because of the policy above.** `script-src 'self'`
blocks inline execution outright, so an inline `<script>` here is not a matter of
taste -- it is a page that loads and then does nothing at all. That shipped once:
the CSP was asserted by its text in a test and the page was never opened in a
browser, so nothing noticed that the two disagreed. There is a test now that every
`<script>` tag carries a `src`.

Its strings are English only. Nothing in this project translates HTML, and core
— where the server lives — has no catalogue and never will; the JSON keys and
error codes it serves are protocol rather than interface text. See
`app/locale/README.md` for what else is deliberately left untranslated.
