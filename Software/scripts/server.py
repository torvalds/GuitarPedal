#!/usr/bin/env python3
import http.server
import socketserver
import sys

PORT = 8080

class Handler(http.server.SimpleHTTPRequestHandler):
    # Never let the browser cache anything from the development server.
    # Without this it applies heuristic caching (there are no cache headers
    # to go by), and you end up editing app.js and testing the old one.
    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

if len(sys.argv) > 1:
    try:
        PORT = int(sys.argv[1])
    except ValueError:
        pass

with socketserver.TCPServer(("", PORT), Handler) as httpd:
    print(f"Serving at http://localhost:{PORT}")
    print("Use this URL in your Web-MIDI capable browser (like Chrome).")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down server.")
