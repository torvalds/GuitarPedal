#!/usr/bin/env python3
import http.server
import socketserver
import sys

PORT = 8080

class Handler(http.server.SimpleHTTPRequestHandler):
    # Optionally add headers for Web MIDI or Cross-Origin policies if needed in future
    def end_headers(self):
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
