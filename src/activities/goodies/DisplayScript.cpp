#include "DisplayScript.h"

#if CROSSDINK_GOODIES

#include <cstdlib>
#include <cstring>

namespace display_script {

namespace {
constexpr size_t MAX_OPS = 256;
constexpr int MAX_DEPTH = 4;

std::string trim(const char* begin, const char* end) {
  while (begin < end && (*begin == ' ' || *begin == '\t')) ++begin;
  while (end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) --end;
  return std::string(begin, end);
}

// Splits `rest` into whitespace-separated words (at most `max`).
int words(const std::string& rest, std::string* out, int max) {
  int n = 0;
  size_t i = 0;
  while (i < rest.size() && n < max) {
    while (i < rest.size() && (rest[i] == ' ' || rest[i] == '\t')) ++i;
    if (i >= rest.size()) break;
    const size_t start = i;
    while (i < rest.size() && rest[i] != ' ' && rest[i] != '\t') ++i;
    out[n++] = rest.substr(start, i - start);
  }
  return n;
}

bool number(const std::string& s, int32_t& out) {
  if (s.empty()) return false;
  char* end = nullptr;
  const long v = strtol(s.c_str(), &end, 0);
  if (*end != '\0') return false;
  out = static_cast<int32_t>(v);
  return true;
}

// Parses `count` numbers from args[first..] into op.a[0..].
bool numbers(const std::string* args, int argc, int first, int count, Op& op) {
  if (argc - first < count) return false;
  for (int i = 0; i < count; ++i) {
    if (!number(args[first + i], op.a[i]) || op.a[i] < 0) return false;
  }
  return true;
}

const char* parseLine(const std::string& verb, const std::string& rest, Op& op) {
  std::string args[6];
  const int argc = words(rest, args, 6);
  if (verb == "name") {
    op.code = OpCode::Name;
    op.text = rest;
    return rest.empty() ? "name needs text" : nullptr;
  }
  if (verb == "label") {
    op.code = OpCode::Label;
    op.text = rest;
    return rest.empty() ? "label needs text" : nullptr;
  }
  if (verb == "note") {
    op.code = OpCode::Note;
    op.text = rest;
    return nullptr;
  }
  if (verb == "ask") {
    op.code = OpCode::Ask;
    const size_t bar1 = rest.find('|');
    const size_t bar2 = bar1 == std::string::npos ? bar1 : rest.find('|', bar1 + 1);
    if (bar2 == std::string::npos) return "ask needs: question | A | B";
    op.text = trim(rest.data(), rest.data() + bar1);
    op.options[0] = trim(rest.data() + bar1 + 1, rest.data() + bar2);
    op.options[1] = trim(rest.data() + bar2 + 1, rest.data() + rest.size());
    return op.options[0].empty() || op.options[1].empty() ? "ask needs two answers" : nullptr;
  }
  if (verb == "fill") {
    op.code = OpCode::Fill;
    if (argc == 1 && args[0] == "white") return nullptr;
    op.a[0] = 1;
    return argc == 1 && args[0] == "black" ? nullptr : "fill white|black";
  }
  if (verb == "pattern") {
    if (argc < 1) return "pattern needs a kind";
    const std::string& kind = args[0];
    if (kind == "text") {
      op.code = OpCode::Text;
      return nullptr;
    }
    if (kind == "checker")
      op.code = OpCode::Checker;
    else if (kind == "hstripes")
      op.code = OpCode::HStripes;
    else if (kind == "vstripes")
      op.code = OpCode::VStripes;
    else
      return "pattern checker|hstripes|vstripes|text";
    return numbers(args, argc, 1, 1, op) && op.a[0] > 0 ? nullptr : "pattern needs a size > 0";
  }
  if (verb == "box") {
    op.code = OpCode::Box;
    if (!numbers(args, argc, 0, 4, op)) return "box x y w h [white]";
    op.a[4] = argc > 4 && args[4] == "white";
    return nullptr;
  }
  if (verb == "invert") {
    op.code = OpCode::Invert;
    return nullptr;
  }
  if (verb == "refresh") {
    op.code = OpCode::Refresh;
    const std::string& m = argc > 0 ? args[0] : std::string();
    if (m == "full")
      op.a[0] = static_cast<int32_t>(Mode::Full);
    else if (m == "half")
      op.a[0] = static_cast<int32_t>(Mode::Half);
    else if (m == "fast")
      op.a[0] = static_cast<int32_t>(Mode::Fast);
    else if (m == "du")
      op.a[0] = static_cast<int32_t>(Mode::Du);
    else
      return "refresh full|half|fast|du";
    return nullptr;
  }
  if (verb == "frames") {
    op.code = OpCode::Frames;
    return numbers(args, argc, 0, 1, op) && op.a[0] >= 1 && op.a[0] <= 63 ? nullptr : "frames 1..63";
  }
  if (verb == "pll") {
    op.code = OpCode::Pll;
    return numbers(args, argc, 0, 1, op) && op.a[0] <= 0xFF ? nullptr : "pll 0x00..0xFF";
  }
  if (verb == "window") {
    op.code = OpCode::Window;
    if (argc == 1 && args[0] == "off") return nullptr;
    return numbers(args, argc, 0, 4, op) && op.a[2] > 0 && op.a[3] > 0 ? nullptr : "window x y w h | off";
  }
  if (verb == "resync") {
    op.code = OpCode::Resync;
    op.a[0] = argc == 1 && args[0] == "on";
    return argc == 1 && (args[0] == "on" || args[0] == "off") ? nullptr : "resync on|off";
  }
  if (verb == "scrub") {
    op.code = OpCode::Scrub;
    op.a[0] = argc == 1 && args[0] == "du";
    return argc == 1 && (args[0] == "half" || args[0] == "du") ? nullptr : "scrub half|du";
  }
  if (verb == "wait") {
    op.code = OpCode::Wait;
    return numbers(args, argc, 0, 1, op) && op.a[0] <= 60000 ? nullptr : "wait 0..60000";
  }
  if (verb == "repeat") {
    op.code = OpCode::Repeat;
    return numbers(args, argc, 0, 1, op) && op.a[0] >= 1 && op.a[0] <= 1000 ? nullptr : "repeat 1..1000";
  }
  if (verb == "end") {
    op.code = OpCode::End;
    return nullptr;
  }
  return "unknown command";
}
}  // namespace

// Built-in tests. Coordinates are logical and stay inside 480x480 so they fit
// either orientation.
const BuiltIn BUILT_INS[] = {
    {"Refresh modes",
     "fill white\nrefresh full\n"
     "label Next: FULL\nrefresh fast\nwait 1200\nnote full\npattern checker 32\nlabel FULL refresh\nrefresh full\nwait "
     "2000\nfill white\nlabel FULL back to white\nrefresh full\nwait 1500\nfill white\n"
     "label Next: HALF\nrefresh fast\nwait 1200\nnote half\npattern checker 32\nlabel HALF refresh\nrefresh half\nwait "
     "2000\nfill white\nlabel HALF back to white\nrefresh half\nwait 1500\nfill white\n"
     "label Next: FAST\nrefresh fast\nwait 1200\nnote fast\npattern checker 32\nlabel FAST refresh\nrefresh fast\nwait "
     "2000\nfill white\nlabel FAST back to white\nrefresh fast\nwait 1500\nfill white\n"
     "label Next: DU\nrefresh fast\nwait 1200\nnote du\npattern checker 32\nlabel DU refresh\nrefresh du\nwait "
     "2000\nfill white\nlabel DU back to white\nrefresh du\nwait 1500\nfill white\n"
     "ask Cleaner, Half or Fast? | Half | Fast\n"
     "ask Cleaner, Fast or DU? | Fast | DU\n"},
    {"Fast x20 text ghosting",
     "fill white\nrefresh half\n"
     "repeat 10\npattern text\nrefresh fast\nfill white\nrefresh fast\nend\n"
     "ask Ghosting visible? | Yes | No\n"},
    {"Moving box",
     "fill white\nrefresh half\n"
     "repeat 2\n"
     "fill white\nbox 20 40 120 120\nrefresh fast\n"
     "fill white\nbox 180 40 120 120\nrefresh fast\n"
     "fill white\nbox 340 40 120 120\nrefresh fast\n"
     "fill white\nbox 340 200 120 120\nrefresh fast\n"
     "fill white\nbox 180 200 120 120\nrefresh fast\n"
     "fill white\nbox 20 200 120 120\nrefresh fast\n"
     "end\nfill white\nrefresh fast\n"
     "ask Trail left behind? | Yes | No\n"},
    {"DU frames sweep",
     "fill white\nrefresh half\n"
     "frames 3\npattern checker 16\nrefresh du\nfill white\nrefresh du\nwait 700\n"
     "frames 6\npattern checker 16\nrefresh du\nfill white\nrefresh du\nwait 700\n"
     "frames 9\npattern checker 16\nrefresh du\nfill white\nrefresh du\nwait 700\n"
     "frames 12\npattern checker 16\nrefresh du\nfill white\nrefresh du\n"
     "ask Cleaner? | 3-6 frames | 9-12 frames\n"},
    {"Window vs full upload",
     "fill white\nrefresh half\n"
     "note windowed\npattern text\nwindow 0 0 480 160\nrefresh fast\nwait 1000\n"
     "note full\nwindow off\nrefresh fast\nwait 1000\n"
     "fill white\nrefresh fast\n"},
    {"Half scrub after 10 fast",
     "fill white\nrefresh half\n"
     "repeat 10\npattern checker 16\nrefresh fast\nfill white\nrefresh fast\nend\n"
     "ask Ghosting before scrub? | Yes | No\n"
     "scrub half\nrefresh fast\n"
     "ask Clean after scrub? | Yes | No\n"},
    {"Invert toggle",
     "fill white\npattern text\nrefresh half\n"
     "repeat 6\ninvert\nrefresh fast\nend\n"
     "ask Ghosting visible? | Yes | No\n"},
};
const int BUILT_IN_COUNT = sizeof(BUILT_INS) / sizeof(BUILT_INS[0]);

Script parse(const char* src, const size_t len) {
  Script script;
  int openRepeats[MAX_DEPTH];
  int depth = 0;
  int lineNo = 0;
  const char* p = src;
  const char* const end = src + len;
  auto fail = [&script, &lineNo](const char* why) {
    script.errorLine = lineNo;
    script.error = why;
    script.ops.clear();
  };
  while (p < end) {
    const char* nl = static_cast<const char*>(memchr(p, '\n', end - p));
    const char* lineEnd = nl ? nl : end;
    ++lineNo;
    std::string line = trim(p, lineEnd);
    p = nl ? nl + 1 : end;
    if (line.empty() || line[0] == '#') continue;
    const size_t space = line.find_first_of(" \t");
    const std::string verb = line.substr(0, space);
    const std::string rest =
        space == std::string::npos ? std::string() : trim(line.data() + space, line.data() + line.size());
    Op op{};
    if (const char* why = parseLine(verb, rest, op)) {
      fail(why);
      return script;
    }
    if (op.code == OpCode::Name) {
      script.name = op.text;
      continue;
    }
    if (script.ops.size() >= MAX_OPS) {
      fail("too many commands (256)");
      return script;
    }
    if (op.code == OpCode::Repeat) {
      if (depth >= MAX_DEPTH) {
        fail("repeat nested too deep (4)");
        return script;
      }
      openRepeats[depth++] = static_cast<int>(script.ops.size());
    } else if (op.code == OpCode::End) {
      if (depth == 0) {
        fail("end without repeat");
        return script;
      }
      const int start = openRepeats[--depth];
      op.a[0] = start;
      script.ops[start].a[1] = static_cast<int32_t>(script.ops.size());
    }
    script.ops.push_back(std::move(op));
  }
  if (depth != 0) fail("repeat without end");
  return script;
}

const char* modeName(const Mode mode) {
  switch (mode) {
    case Mode::Full:
      return "full";
    case Mode::Half:
      return "half";
    case Mode::Fast:
      return "fast";
    case Mode::Du:
      return "du";
  }
  return "?";
}

}  // namespace display_script

#endif  // CROSSDINK_GOODIES
