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

// Splits `s` on '|' into trimmed fields.
std::vector<std::string> fields(const std::string& s) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    const size_t bar = s.find('|', start);
    const size_t stop = bar == std::string::npos ? s.size() : bar;
    out.push_back(trim(s.data() + start, s.data() + stop));
    if (bar == std::string::npos) return out;
    start = bar + 1;
  }
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
    auto f = fields(rest);
    if (f.size() != 3 || f[1].empty() || f[2].empty()) return "ask needs: question | A | B";
    op.text = std::move(f[0]);
    op.options = {std::move(f[1]), std::move(f[2])};
    return nullptr;
  }
  if (verb == "pick") {
    op.code = OpCode::Pick;
    auto f = fields(rest);
    std::string nums[6];
    if (words(f[0], nums, 6) != 6 || !numbers(nums, 6, 0, 6, op) || op.a[2] <= 0 || op.a[3] <= 0) {
      return "pick x y cellW cellH cols rows | question | name1 | ...";
    }
    const int cells = op.a[4] * op.a[5];
    if (cells < 2 || cells > 16 || static_cast<int>(f.size()) != cells + 2) return "pick needs cols*rows (2..16) names";
    op.text = std::move(f[1]);
    op.options.assign(f.begin() + 2, f.end());
    return nullptr;
  }
  if (verb == "text") {
    op.code = OpCode::DrawText;
    std::string xy[2];
    if (words(rest, xy, 2) != 2 || !numbers(xy, 2, 0, 2, op)) return "text x y <text>";
    const size_t at = rest.find_first_not_of(
        " \t", rest.find_first_of(" \t", rest.find_first_not_of(" \t", rest.find_first_of(" \t"))));
    if (at == std::string::npos) return "text x y <text>";
    op.text = rest.substr(at);
    return nullptr;
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
  if (verb == "window" || verb == "resync") {
    // Both can leave the OLD plane unlike the panel (a windowed upload of a
    // full-screen change, or a skipped resync that relies on N2OCP), so the next
    // refresh drives pixels through a transition they never made: one-way.
    return "window/resync removed (unsafe OLD plane)";
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
// either orientation. Every refresh carries a label (what it is | what to look
// for | what is next) and every question names what it asks about. An ask
// leaves its band in the framebuffer, so the next step starts with `fill white`.
const BuiltIn BUILT_INS[] = {
    {"Refresh modes",
     "frames 6\n"
     "fill white\n"
     "label Refresh modes | Squares on, then white, in each mode | Next: FULL\n"
     "refresh full\n"
     "wait 2500\n"
     "fill white\n"
     "pattern checker 32\n"
     "label FULL: squares on | Look: solid, sharp black squares | Next: FULL back to white\n"
     "refresh full\n"
     "wait 2500\n"
     "fill white\n"
     "label FULL: back to white | Look: faint gray squares = ghost | Next: question\n"
     "refresh full\n"
     "wait 2000\n"
     "ask FULL: faint squares left on the white? | Yes | No\n"
     "fill white\n"
     "pattern checker 32\n"
     "label HALF: squares on | Look: solid, sharp black squares | Next: HALF back to white\n"
     "refresh half\n"
     "wait 2500\n"
     "fill white\n"
     "label HALF: back to white | Look: faint gray squares = ghost | Next: question\n"
     "refresh half\n"
     "wait 2000\n"
     "ask HALF: faint squares left on the white? | Yes | No\n"
     "fill white\n"
     "pattern checker 32\n"
     "label FAST: squares on | Look: solid, sharp black squares | Next: FAST back to white\n"
     "refresh fast\n"
     "wait 2500\n"
     "fill white\n"
     "label FAST: back to white | Look: faint gray squares = ghost | Next: question\n"
     "refresh fast\n"
     "wait 2000\n"
     "ask FAST: faint squares left on the white? | Yes | No\n"
     "fill white\n"
     "pattern checker 32\n"
     "label DU (6 frames): squares on | Look: solid, sharp black squares | Next: DU back to white\n"
     "refresh du\n"
     "wait 2500\n"
     "fill white\n"
     "label DU (6 frames): back to white | Look: faint gray squares = ghost | Next: question\n"
     "refresh du\n"
     "wait 2000\n"
     "ask DU (6 frames): faint squares left on the white? | Yes | No\n"},
    {"Ghost pick (8 squares)",
     "fill white\n"
     "label Ghost pick: 8 black squares | Each clears with a different refresh | Next: 1 to 8 turn white, then pick\n"
     "box 12 130 96 96\n"
     "text 52 232 1\n"
     "box 132 130 96 96\n"
     "text 172 232 2\n"
     "box 252 130 96 96\n"
     "text 292 232 3\n"
     "box 372 130 96 96\n"
     "text 412 232 4\n"
     "box 12 280 96 96\n"
     "text 52 382 5\n"
     "box 132 280 96 96\n"
     "text 172 382 6\n"
     "box 252 280 96 96\n"
     "text 292 382 7\n"
     "box 372 280 96 96\n"
     "text 412 382 8\n"
     "refresh half\n"
     "wait 3000\n"
     "note square 1 fast\n"
     "box 12 130 96 96 white\n"
     "refresh fast\n"
     "note square 2 du 3\n"
     "box 132 130 96 96 white\n"
     "frames 3\n"
     "refresh du\n"
     "note square 3 du 4\n"
     "box 252 130 96 96 white\n"
     "frames 4\n"
     "refresh du\n"
     "note square 4 du 6\n"
     "box 372 130 96 96 white\n"
     "frames 6\n"
     "refresh du\n"
     "note square 5 du 9\n"
     "box 12 280 96 96 white\n"
     "frames 9\n"
     "refresh du\n"
     "note square 6 du 12\n"
     "box 132 280 96 96 white\n"
     "frames 12\n"
     "refresh du\n"
     "note square 7 du 15\n"
     "box 252 280 96 96 white\n"
     "frames 15\n"
     "refresh du\n"
     "note square 8 du 20\n"
     "box 372 280 96 96 white\n"
     "frames 20\n"
     "refresh du\n"
     "wait 1500\n"
     "pick 0 120 120 150 4 2 | Cleared squares: which is cleanest? | fast | du 3 | du 4 | du 6 | du 9 | du 12 | du 15 "
     "| "
     "du 20\n"},
    {"Fast x20 text ghosting",
     "fill white\n"
     "label Fast x20 text ghosting | Text page, then white, 10 rounds | Next: rounds start\n"
     "refresh half\n"
     "wait 2500\n"
     "repeat 10\n"
     "fill white\n"
     "pattern text\n"
     "label Fast round: text page | Builds up ghosting | Next: white page\n"
     "refresh fast\n"
     "fill white\n"
     "label Fast round: white page | Look: gray text left behind | Next: text again\n"
     "refresh fast\n"
     "end\n"
     "wait 2000\n"
     "ask After 20 Fast refreshes: gray text on the white? | Yes | No\n"},
    {"Moving box",
     "fill white\n"
     "label Moving box (Fast) | Box jumps to 6 spots, twice | Next: box moves\n"
     "refresh half\n"
     "wait 2500\n"
     "repeat 2\n"
     "fill white\n"
     "box 20 100 120 120\n"
     "label Moving box (Fast) | Look: gray trail where it was | Next: box moves\n"
     "refresh fast\n"
     "fill white\n"
     "box 180 100 120 120\n"
     "label Moving box (Fast) | Look: gray trail where it was | Next: box moves\n"
     "refresh fast\n"
     "fill white\n"
     "box 340 100 120 120\n"
     "label Moving box (Fast) | Look: gray trail where it was | Next: box moves\n"
     "refresh fast\n"
     "fill white\n"
     "box 340 260 120 120\n"
     "label Moving box (Fast) | Look: gray trail where it was | Next: box moves\n"
     "refresh fast\n"
     "fill white\n"
     "box 180 260 120 120\n"
     "label Moving box (Fast) | Look: gray trail where it was | Next: box moves\n"
     "refresh fast\n"
     "fill white\n"
     "box 20 260 120 120\n"
     "label Moving box (Fast) | Look: gray trail where it was | Next: box moves\n"
     "refresh fast\n"
     "end\n"
     "fill white\n"
     "label Moving box done (Fast) | Look: gray boxes where it was | Next: question\n"
     "refresh fast\n"
     "wait 2000\n"
     "ask After 12 Fast moves: gray boxes where it had been? | Yes | No\n"},
    {"DU frames sweep",
     "fill white\n"
     "label DU frames sweep | Squares on/off with 3, 6, 9, 12 frames | Next: 3 frames\n"
     "refresh half\n"
     "wait 2500\n"
     "frames 3\n"
     "fill white\n"
     "pattern checker 16\n"
     "label DU 3 frames: squares on | Look: solid black, no gray | Next: back to white\n"
     "refresh du\n"
     "wait 2000\n"
     "fill white\n"
     "label DU 3 frames: back to white | Look: faint squares = ghost | Next: question\n"
     "refresh du\n"
     "wait 2000\n"
     "ask DU 3 frames: faint squares left on the white? | Yes | No\n"
     "frames 6\n"
     "fill white\n"
     "pattern checker 16\n"
     "label DU 6 frames: squares on | Look: solid black, no gray | Next: back to white\n"
     "refresh du\n"
     "wait 2000\n"
     "fill white\n"
     "label DU 6 frames: back to white | Look: faint squares = ghost | Next: question\n"
     "refresh du\n"
     "wait 2000\n"
     "ask DU 6 frames: faint squares left on the white? | Yes | No\n"
     "frames 9\n"
     "fill white\n"
     "pattern checker 16\n"
     "label DU 9 frames: squares on | Look: solid black, no gray | Next: back to white\n"
     "refresh du\n"
     "wait 2000\n"
     "fill white\n"
     "label DU 9 frames: back to white | Look: faint squares = ghost | Next: question\n"
     "refresh du\n"
     "wait 2000\n"
     "ask DU 9 frames: faint squares left on the white? | Yes | No\n"
     "frames 12\n"
     "fill white\n"
     "pattern checker 16\n"
     "label DU 12 frames: squares on | Look: solid black, no gray | Next: back to white\n"
     "refresh du\n"
     "wait 2000\n"
     "fill white\n"
     "label DU 12 frames: back to white | Look: faint squares = ghost | Next: question\n"
     "refresh du\n"
     "wait 2000\n"
     "ask DU 12 frames: faint squares left on the white? | Yes | No\n"},
    {"Half scrub after 10 fast",
     "fill white\n"
     "label Half scrub after 10 Fast | Squares on/off 10 times, then a scrub | Next: rounds start\n"
     "refresh half\n"
     "wait 2500\n"
     "repeat 10\n"
     "fill white\n"
     "pattern checker 16\n"
     "label Fast round: squares on | Builds up ghosting | Next: white\n"
     "refresh fast\n"
     "fill white\n"
     "label Fast round: white | Look: faint squares = ghost | Next: squares again\n"
     "refresh fast\n"
     "end\n"
     "wait 2000\n"
     "ask Before the scrub: faint squares on the white? | Yes | No\n"
     "fill white\n"
     "label After the Half scrub | Look: faint squares gone | Next: question\n"
     "scrub half\n"
     "refresh fast\n"
     "wait 2000\n"
     "ask After the Half scrub: faint squares gone? | Yes | No\n"},
    {"Invert toggle",
     "fill white\n"
     "pattern text\n"
     "label Invert toggle (Fast) | Page flips black/white 6 times | Next: flips start\n"
     "refresh half\n"
     "wait 2500\n"
     "repeat 6\n"
     "invert\n"
     "refresh fast\n"
     "wait 1000\n"
     "end\n"
     "wait 1000\n"
     "ask After 6 Fast flips: gray text in the white areas? | Yes | No\n"},
    {"DU scrub 15 over text",
     "fill white\n"
     "refresh half\n"
     "pattern text\n"
     "label DU scrub 15: text page | Drawn with Fast | Next: 15-frame DU scrub to squares\n"
     "refresh fast\n"
     "wait 3000\n"
     "fill white\n"
     "pattern checker 40\n"
     "label DU scrub 15: squares | Look: old text under the squares | Next: question\n"
     "frames 15\n"
     "scrub du\n"
     "refresh du\n"
     "wait 2000\n"
     "ask After the 15-frame scrub: old text under the squares? | Yes | No\n"},
    {"DU scrub 20 over text (cold panel)",
     "fill white\n"
     "refresh half\n"
     "pattern text\n"
     "label DU scrub 20: text page | Run with the panel below 15 C | Next: 20-frame DU scrub to squares\n"
     "refresh fast\n"
     "wait 3000\n"
     "fill white\n"
     "pattern checker 40\n"
     "label DU scrub 20: squares | Look: old text under the squares | Next: question\n"
     "frames 20\n"
     "scrub du\n"
     "refresh du\n"
     "wait 2000\n"
     "ask After the 20-frame scrub: old text under the squares? | Yes | No\n"},
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
