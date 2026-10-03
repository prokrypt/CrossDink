#pragma once

#include <HalStorage.h>
#include <WebServer.h>

#include <atomic>

class WebDAVHandler : public RequestHandler {
 public:
  // stopping: the server's stop flag. A PUT in flight when it is set is
  // aborted (temp file removed) instead of holding up stop() until it ends.
  explicit WebDAVHandler(const std::atomic<bool>* stopping) : _stopping(stopping) {}

  // RequestHandler interface
  bool canHandle(WebServer& server, HTTPMethod method, const String& uri) override;
  bool canRaw(WebServer& server, const String& uri) override;
  void raw(WebServer& server, const String& uri, HTTPRaw& raw) override;
  bool handle(WebServer& server, HTTPMethod method, const String& uri) override;

 private:
  const std::atomic<bool>* _stopping;
  // PUT streaming state (raw() is called in chunks)
  HalFile _putFile;
  String _putPath;
  bool _putOk = false;
  bool _putExisted = false;

  // WebDAV method handlers
  static void handleOptions(WebServer& s);
  void handlePropfind(WebServer& s);
  void handleGet(WebServer& s);
  void handleHead(WebServer& s);
  void handlePut(WebServer& s);
  void handleDelete(WebServer& s);
  void handleMkcol(WebServer& s);
  void handleMove(WebServer& s);
  void handleCopy(WebServer& s);
  void handleLock(WebServer& s);
  static void handleUnlock(WebServer& s);

  // Utilities
  String getRequestPath(WebServer& s) const;
  String getDestinationPath(WebServer& s) const;
  void urlEncodePath(const String& path, String& out) const;
  bool isProtectedPath(const String& path) const;
  int getDepth(WebServer& s) const;
  bool getOverwrite(WebServer& s) const;
  void sendPropEntry(WebServer& s, const String& href, bool isDir, size_t size, const String& lastModified) const;
  static String getMimeType(const String& path);
};
