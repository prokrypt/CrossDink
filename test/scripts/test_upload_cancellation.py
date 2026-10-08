#!/usr/bin/env python3
"""Compile the production upload WRITE/END/ABORTED paths with fake I/O.

Cancellation comes from CrossPointWebServer::stop() (running cleared, then
stopRequested set); the web server runs on its own task, so no input is polled.
Does not emulate Arduino's multipart transport.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/network/CrossPointWebServer.cpp').read_text()


def function(text, signature):
    start = text.index(signature)
    end = text.index('\n}', start) + 2
    return text[start:end] + '\n'


parts = [function(source, signature) for signature in (
    'static void removePartialUpload(',
    'bool CrossPointWebServer::dropUploadIfCancelled()',
    'void CrossPointWebServer::abortUpload(',
    'void CrossPointWebServer::abortFontUpload()',
    'static bool flushUploadBuffer(')]
handler = function(source, 'void CrossPointWebServer::handleUpload(')
# Keep the stop() check and safety guard, skip the START branch's SD/route setup.
prologue_end = handler.index('  const HTTPUpload& upload = server->upload();')
start = handler.index('  } else if (upload.status == UPLOAD_FILE_WRITE)')
parts.append(handler[:prologue_end] + '  const HTTPUpload& upload = server->upload();\n  if' +
             handler[start + len('  } else if'):])
font = function(source, 'void CrossPointWebServer::handleFontUploadData()')
parts.append('void CrossPointWebServer::handleFontUploadData() {\n'
             'HTTPUpload& upload = server->upload();\nswitch(upload.status) {\n' +
             font[font.index('    case UPLOAD_FILE_WRITE:'):])
with tempfile.TemporaryDirectory(prefix='crossdink-upload-cancel-') as tmp:
    root = Path(tmp)
    (root / 'UploadHandlers.inc').write_text('\n'.join(parts))
    executable = root / 'test'
    subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-variable', '-Wno-unused-function',
                    '-fsanitize=address,undefined', '-I' + str(root),
                    str(ROOT / 'test/upload_cancellation/UploadCancellationTest.cpp'), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('PASS: book/font uploads, stop() mid-chunk/final, parked .part, upgrade not cancelled, late END/ABORTED')
